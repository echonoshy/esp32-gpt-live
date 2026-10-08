mod codex;

use anyhow::{anyhow, bail, Context, Result};
use axum::serve::ListenerExt;
use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        ConnectInfo, Query, State,
    },
    http::{HeaderMap, StatusCode},
    response::{Html, IntoResponse, Response},
    routing::get,
    Json, Router,
};
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    net::SocketAddr,
    process::Stdio,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};
use tokio::{
    process::Command,
    sync::{mpsc, oneshot},
};
use uuid::Uuid;

const FRAME_BYTES: usize = 960; // 20 ms, 24 kHz, PCM s16le mono
type Tx = mpsc::Sender<Message>;
struct Worker {
    tx: Tx,
    rx: mpsc::Receiver<Message>,
}
struct App {
    pending: Mutex<HashMap<String, oneshot::Sender<Worker>>>,
    busy: Arc<AtomicBool>,
    token: String,
    port: u16,
}
type Shared = Arc<App>;

fn text(value: Value) -> Message {
    Message::Text(value.to_string().into())
}
fn status(name: &str) -> Message {
    text(json!({"type":"status","state":name}))
}
fn send(tx: &Tx, message: Message) -> Result<()> {
    tx.try_send(message)
        .map_err(|_| anyhow!("audio queue full or peer disconnected"))
}
fn valid_pcm(bytes: &[u8]) -> bool {
    bytes.len() == FRAME_BYTES
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
        .init();
    let address: SocketAddr = std::env::var("LISTEN")
        .unwrap_or_else(|_| "127.0.0.1:8787".into())
        .parse()?;
    let token = std::env::var("DEVICE_TOKEN").unwrap_or_default();
    if !address.ip().is_loopback() && token.len() < 16 {
        bail!("public listener requires DEVICE_TOKEN of at least 16 characters")
    }
    let app = Arc::new(App {
        pending: Mutex::new(HashMap::new()),
        busy: Arc::new(AtomicBool::new(false)),
        token,
        port: address.port(),
    });
    let routes = Router::new()
        .route("/health", get(health))
        .route("/device", get(device_upgrade))
        .route("/worker", get(worker_upgrade))
        .route(
            "/media",
            get(|| async { Html(include_str!("../web/worker.html")) }),
        )
        .route(
            "/worklet.js",
            get(|| async {
                (
                    [("content-type", "text/javascript")],
                    include_str!("../web/worklet.js"),
                )
            }),
        )
        .with_state(app);
    let listener = tokio::net::TcpListener::bind(address)
        .await?
        .tap_io(|stream| {
            let _ = stream.set_nodelay(true);
        });
    tracing::info!(%address,"Passport voice server ready");
    axum::serve(
        listener,
        routes.into_make_service_with_connect_info::<SocketAddr>(),
    )
    .with_graceful_shutdown(async {
        #[cfg(unix)]
        {
            let mut term =
                tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()).unwrap();
            tokio::select! { _=tokio::signal::ctrl_c()=>{}, _=term.recv()=>{} }
        }
        #[cfg(not(unix))]
        let _ = tokio::signal::ctrl_c().await;
    })
    .await?;
    Ok(())
}

async fn health(State(app): State<Shared>) -> Json<Value> {
    Json(
        json!({"ok":true,"device_connected":app.busy.load(Ordering::Relaxed),"sample_rate":24000,"frame_bytes":FRAME_BYTES}),
    )
}

struct Slot(Arc<AtomicBool>);
impl Drop for Slot {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release)
    }
}
async fn device_upgrade(
    State(app): State<Shared>,
    headers: HeaderMap,
    ws: WebSocketUpgrade,
) -> Response {
    if !app.token.is_empty()
        && headers.get("authorization").and_then(|v| v.to_str().ok())
            != Some(format!("Bearer {}", app.token).as_str())
    {
        return StatusCode::UNAUTHORIZED.into_response();
    }
    if app
        .busy
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Relaxed)
        .is_err()
    {
        return StatusCode::CONFLICT.into_response();
    }
    let slot = Slot(app.busy.clone());
    ws.max_message_size(4096)
        .on_upgrade(move |socket| device(socket, app, slot))
}

async fn device(socket: WebSocket, app: Shared, _slot: Slot) {
    let (mut sink, mut source) = socket.split();
    let (out, mut outgoing) = mpsc::channel(32);
    let mut session: Option<(Tx, tokio::task::JoinHandle<()>)> = None;
    let mut heartbeat = tokio::time::interval(Duration::from_secs(15));
    let mut last_rx = tokio::time::Instant::now();
    let _ = sink.send(status("idle")).await;
    tracing::info!("device connected");
    loop {
        tokio::select! {
            frame=source.next()=>{
                let Some(Ok(frame))=frame else {break};
                last_rx=tokio::time::Instant::now();
                match frame {
                    Message::Close(_)=>break,
                    Message::Ping(p)=>{if sink.send(Message::Pong(p)).await.is_err(){break}},
                    Message::Pong(_)=>{},
                    Message::Binary(bytes)=>{
                        if !valid_pcm(&bytes) {break}
                        if let Some((tx,_))=&session {
                            if send(tx,Message::Binary(bytes)).is_err(){break}
                        }
                    },
                    Message::Text(s)=>{
                        let Ok(v)=serde_json::from_str::<Value>(&s) else {break};
                        let kind=v["type"].as_str().unwrap_or("");
                        if kind=="start" {
                            if session.as_ref().is_some_and(|(_,task)|task.is_finished()) {session=None}
                            if session.is_none() {
                                let (tx,rx)=mpsc::channel(32);
                                let output=out.clone();let shared=app.clone();
                                session=Some((tx,tokio::spawn(async move {
                                    if let Err(error)=run_session(shared,rx,output.clone()).await {
                                        // No account tokens, SDP or conversation text in logs.
                                        tracing::warn!(%error,"voice session ended with error");
                                        let _=send(&output,text(json!({"type":"error","message":"Voice connection failed; press OK to retry"})));
                                    }
                                    let _=send(&output,status("idle"));
                                })));
                            }
                        } else if kind=="stop" {
                            if let Some((tx,mut task))=session.take() {
                                let _=send(&tx,Message::Text(s));
                                drop(tx);
                                if tokio::time::timeout(Duration::from_secs(5),&mut task).await.is_err(){
                                    task.abort();let _=task.await;
                                    let _=send(&out,status("idle"));
                                }
                            } else {let _=send(&out,status("idle"));}
                        } else if matches!(kind,"ptt_start"|"ptt_end") {
                            if let Some((tx,_))=&session {if send(tx,Message::Text(s)).is_err(){break}}
                        } else if kind!="hello" {break}
                    }
                }
            },
            Some(message)=outgoing.recv()=>{
                if !matches!(tokio::time::timeout(Duration::from_secs(2),sink.send(message)).await,Ok(Ok(()))){break}
            },
            _=heartbeat.tick()=>{
                if last_rx.elapsed()>Duration::from_secs(45){break}
                if sink.send(Message::Ping(Vec::new().into())).await.is_err(){break}
            }
        }
    }
    if let Some((tx, mut task)) = session {
        drop(tx);
        if tokio::time::timeout(Duration::from_secs(6), &mut task)
            .await
            .is_err()
        {
            task.abort();
            let _ = task.await;
        }
    }
    tracing::info!("device disconnected");
}

#[derive(Deserialize)]
struct WorkerQuery {
    id: String,
}
async fn worker_upgrade(
    State(app): State<Shared>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    Query(query): Query<WorkerQuery>,
    ws: WebSocketUpgrade,
) -> Response {
    if !peer.ip().is_loopback() {
        return StatusCode::FORBIDDEN.into_response();
    }
    let Some(waiter) = app.pending.lock().unwrap().remove(&query.id) else {
        return StatusCode::NOT_FOUND.into_response();
    };
    ws.max_message_size(64 * 1024)
        .on_upgrade(move |socket| async move {
            let (mut sink, mut source) = socket.split();
            let (tx, mut outgoing) = mpsc::channel(32);
            let (incoming, rx) = mpsc::channel(32);
            if waiter.send(Worker { tx, rx }).is_err() {
                return;
            }
            loop {
                tokio::select! {
                    frame=source.next()=>{
                        match frame {
                            Some(Ok(Message::Close(_)))|None|Some(Err(_))=>break,
                            Some(Ok(frame))=>if incoming.try_send(frame).is_err(){break},
                        }
                    },
                    frame=outgoing.recv()=>{
                        let Some(frame)=frame else {break};
                        if sink.send(frame).await.is_err(){break}
                    }
                }
            }
        })
}

struct PendingRegistration {
    id: String,
    app: Shared,
}
impl Drop for PendingRegistration {
    fn drop(&mut self) {
        self.app.pending.lock().unwrap().remove(&self.id);
    }
}

async fn run_session(app: Shared, mut input: mpsc::Receiver<Message>, output: Tx) -> Result<()> {
    send(&output, status("connecting"))?;
    let id = Uuid::new_v4().to_string();
    let (waiter, wait) = oneshot::channel();
    app.pending.lock().unwrap().insert(id.clone(), waiter);
    let _registration = PendingRegistration {
        id: id.clone(),
        app: app.clone(),
    };
    let profile = tempfile::tempdir()?;
    let chrome = std::env::var("CHROMIUM_BIN").context("set CHROMIUM_BIN")?;
    let mut browser = Command::new(chrome)
        .args([
            "--headless=new",
            "--no-sandbox",
            "--disable-dev-shm-usage",
            "--autoplay-policy=no-user-gesture-required",
            "--disable-background-timer-throttling",
            "--disable-renderer-backgrounding",
        ])
        .arg(format!("--user-data-dir={}", profile.path().display()))
        .arg(format!("http://127.0.0.1:{}/media#{}", app.port, id))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .kill_on_drop(true)
        .spawn()?;
    let mut codex: Option<codex::Codex> = None;
    let result=async {
        let mut worker=tokio::time::timeout(Duration::from_secs(20),wait).await.context("Chromium startup timeout")??;
        let offer=tokio::time::timeout(Duration::from_secs(15),worker.rx.recv()).await?.context("worker disconnected before offer")?;
        let Message::Text(offer)=offer else {bail!("expected SDP offer")};
        let offer:Value=serde_json::from_str(&offer)?;
        let sdp=offer["sdp"].as_str().context("missing SDP offer")?;
        codex=Some(codex::Codex::open().await?);
        let rpc=codex.as_mut().unwrap();
        rpc.start(sdp).await?;
        let mut ready=false;
        let mut ptt=false;
        let mut last_voice=tokio::time::Instant::now()-Duration::from_secs(1);
        let started=tokio::time::Instant::now();
        let mut last_activity=started;
        let mut tick=tokio::time::interval(Duration::from_secs(1));
        loop {
            tokio::select! {
                message=input.recv()=>{
                    let Some(message)=message else {break};
                    match &message {
                        Message::Text(s)=>{
                            let event:Value=serde_json::from_str(s)?;
                            match event["type"].as_str() {
                                Some("stop")=>break,
                                Some("ptt_start")=>{ptt=true;last_activity=tokio::time::Instant::now();},
                                Some("ptt_end")=>{ptt=false;last_activity=tokio::time::Instant::now();},
                                _=>{}
                            }
                            send(&worker.tx,message)?;
                        },
                        Message::Binary(_) if ready && ptt=>{last_activity=tokio::time::Instant::now();send(&worker.tx,message)?;},
                        _=>{}
                    }
                },
                event=rpc.events.recv()=>{
                    let event=event.context("Codex event stream closed")?;
                    match event["method"].as_str() {
                        Some("thread/realtime/sdp")=>send(&worker.tx,text(json!({"type":"answer","sdp":event["params"]["sdp"]})))?,
                        Some("thread/realtime/error")=>bail!("Codex voice service error"),
                        Some("thread/realtime/closed")=>break,
                        _=>{}
                    }
                },
                event=worker.rx.recv()=>{
                    let event=event.context("media worker disconnected")?;
                    match event {
                        Message::Binary(bytes) if ready && !ptt=>{
                            if !valid_pcm(&bytes){bail!("invalid media frame")}
                            if bytes.chunks_exact(2).any(|s|i16::from_le_bytes([s[0],s[1]]).unsigned_abs()>30) {
                                last_voice=tokio::time::Instant::now();last_activity=last_voice;
                            }
                            // Preserve a short tail, without flooding the WAN link with silence.
                            if last_voice.elapsed()<Duration::from_millis(200){send(&output,Message::Binary(bytes)).context("device output")?;}
                        },
                        Message::Text(s)=>{
                            let value:Value=serde_json::from_str(&s)?;
                            match value["type"].as_str() {
                                Some("ready")=>{ready=true;send(&output,status("ready"))?;tracing::info!("voice media ready");},
                                Some("error")=>bail!("media worker: {}",value["message"]),
                                Some("transcript")=>send(&output,text(value))?,
                                _=>{}
                            }
                        },
                        _=>{}
                    }
                },
                _=tick.tick()=>{
                    if !ready && started.elapsed()>Duration::from_secs(30){bail!("WebRTC connection timeout")}
                    if last_activity.elapsed()>Duration::from_secs(120) || started.elapsed()>Duration::from_secs(1800){break}
                }
            }
        }
        Ok(())
    }.await;
    if let Some(rpc) = codex.as_mut() {
        rpc.close().await
    }
    let _ = browser.kill().await;
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_bad_pcm_frames() {
        assert!(valid_pcm(&[0; 960]));
        for n in [0, 1, 958, 962, 1920] {
            assert!(!valid_pcm(&vec![0; n]));
        }
    }
    #[test]
    fn slot_is_released_on_drop() {
        let busy = Arc::new(AtomicBool::new(true));
        {
            let _slot = Slot(busy.clone());
        }
        assert!(!busy.load(Ordering::Acquire));
    }
    #[tokio::test]
    async fn backpressure_does_not_accumulate_audio() {
        let (tx, _rx) = mpsc::channel(1);
        assert!(send(&tx, status("ready")).is_ok());
        assert!(send(&tx, status("ready")).is_err());
    }
}

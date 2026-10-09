mod codex;
mod conversation;
use conversation::{AudioAction, Conversation};

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
        json!({"ok":true,"device_connected":app.busy.load(Ordering::Relaxed),"sample_rate":24000,"frame_bytes":FRAME_BYTES,"protocol":2,"version":env!("CARGO_PKG_VERSION")}),
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
                                let continuous=v["mode"]=="continuous" && v["protocol"]==2;
                                let idle_seconds=v["idle_seconds"].as_u64().unwrap_or(180).clamp(30,900);
                                session=Some((tx,tokio::spawn(async move {
                                    if let Err(error)=run_session(shared,rx,output.clone(),continuous,idle_seconds).await {
                                        // No account tokens, SDP or conversation text in logs.
                                        tracing::warn!(%error,"voice session ended with error");
                                        let _=send(&output,voice_error(&error));
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
                        } else if matches!(kind,"ptt_start"|"ptt_end"|"interrupt"|"playback_drained") {
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

async fn run_session(
    app: Shared,
    mut input: mpsc::Receiver<Message>,
    output: Tx,
    continuous: bool,
    idle_seconds: u64,
) -> Result<()> {
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
        let mut flow=Conversation::new();
        let mut last_voice=tokio::time::Instant::now()-Duration::from_secs(1);
        let started=tokio::time::Instant::now();
        let mut last_activity=started;
        let mut interrupt_hint_sent=false;
        let mut user_energy_frames=0u32;
        let mut tick=tokio::time::interval(Duration::from_millis(50));
        loop {
            tokio::select! {
                message=input.recv()=>{
                    let Some(message)=message else {break};
                    match message {
                        Message::Text(s)=>{
                            let event:Value=serde_json::from_str(&s)?;
                            match event["type"].as_str() {
                                Some("stop")=>break,
                                Some("interrupt") if continuous && ready=>{
                                    let epoch=flow.interrupt(started.elapsed().as_millis() as u64);
                                    user_energy_frames=0;interrupt_hint_sent=false;
                                    last_activity=tokio::time::Instant::now();
                                    send(&output,text(json!({"type":"clear","epoch":epoch})))?;
                                    send(&worker.tx,text(json!({"type":"input","enabled":true,"clear":true})))?;
                                    rpc.interrupt().await?;
                                },
                                Some("playback_drained") if continuous=>{
                                    if let Some(epoch)=event["epoch"].as_u64() {
                                        if u32::try_from(epoch).ok().is_some_and(|e|flow.drained(e)) {
                                            send(&worker.tx,text(json!({"type":"input","enabled":true,"clear":true})))?;
                                            send(&output,text(json!({"type":"listen","epoch":flow.epoch})))?;
                                        }
                                    }
                                },
                                Some("ptt_start") if !continuous=>{
                                    ptt=true;last_activity=tokio::time::Instant::now();
                                    send(&worker.tx,text(json!({"type":"input","enabled":true,"clear":true})))?;
                                },
                                Some("ptt_end") if !continuous=>{
                                    ptt=false;last_activity=tokio::time::Instant::now();
                                    send(&worker.tx,text(json!({"type":"input","enabled":false,"clear":false})))?;
                                },
                                _=>{}
                            }
                        },
                        Message::Binary(bytes) if ready && (if continuous {flow.listening()} else {ptt})=>{
                            // Silence keeps the WebRTC clock alive, not the user's idle timer.
                            if rms_above(&bytes,600) {
                                user_energy_frames+=1;
                                if user_energy_frames>=4 {flow.user_turn();last_activity=tokio::time::Instant::now();}
                            } else {user_energy_frames=0;}
                            send(&worker.tx,Message::Binary(bytes))?;
                        },
                        _=>{}
                    }
                },
                event=rpc.events.recv()=>{
                    let event=event.context("Codex event stream closed")?;
                    match event["method"].as_str() {
                        Some("thread/realtime/sdp")=>send(&worker.tx,text(json!({"type":"answer","sdp":event["params"]["sdp"]})))?,
                        Some("thread/realtime/error"|"passport/controlError")=>bail!("Codex voice service rejected control"),
                        Some("thread/realtime/closed")=>bail!("Codex voice disconnected"),
                        _=>{}
                    }
                },
                event=worker.rx.recv()=>{
                    let event=event.context("media worker disconnected")?;
                    match event {
                        Message::Binary(bytes) if ready && (continuous || !ptt)=>{
                            if !valid_pcm(&bytes){bail!("invalid media frame")}
                            let voiced=bytes.chunks_exact(2).any(|s|i16::from_le_bytes([s[0],s[1]]).unsigned_abs()>30);
                            if continuous {
                                let action=flow.audio(started.elapsed().as_millis() as u64,voiced);
                                if let AudioAction::Begin(epoch)=action {
                                    send(&worker.tx,text(json!({"type":"input","enabled":false,"clear":true})))?;
                                    send(&output,text(json!({"type":"playback_begin","epoch":epoch})))?;
                                }
                                if let AudioAction::Begin(epoch)|AudioAction::Frame(epoch)=action {
                                    if voiced {last_activity=tokio::time::Instant::now();}
                                    let mut tagged=Vec::with_capacity(FRAME_BYTES+4);
                                    tagged.extend_from_slice(&epoch.to_le_bytes());tagged.extend_from_slice(&bytes);
                                    send(&output,Message::Binary(tagged.into())).context("device output")?;
                                }
                            } else {
                                if voiced {last_voice=tokio::time::Instant::now();last_activity=last_voice;}
                                if last_voice.elapsed()<Duration::from_millis(200){send(&output,Message::Binary(bytes)).context("device output")?;}
                            }
                        },
                        Message::Text(s)=>{
                            let value:Value=serde_json::from_str(&s)?;
                            match value["type"].as_str() {
                                Some("ready")=>{
                                    ready=true;send(&output,status("ready"))?;
                                    if continuous {
                                        send(&worker.tx,text(json!({"type":"input","enabled":true,"clear":true})))?;
                                        send(&output,text(json!({"type":"listen","epoch":flow.epoch})))?;
                                    }
                                    tracing::info!(continuous,"voice media ready");
                                },
                                Some("error")=>bail!("media worker failed"),
                                Some("user_started")=>{flow.user_turn();last_activity=tokio::time::Instant::now();},
                                Some("transcript")=>{
                                    if value["role"]=="user" {
                                        flow.user_turn();last_activity=tokio::time::Instant::now();
                                    }
                                    if let Some(message)=caption(&value) {send(&output,message)?;}
                                },
                                _=>{}
                            }
                        },
                        _=>{}
                    }
                },
                _=tick.tick()=>{
                    let now=started.elapsed().as_millis() as u64;
                    if !ready && started.elapsed()>Duration::from_secs(30){bail!("WebRTC connection timeout")}
                    if continuous {
                        if let Some(epoch)=flow.end_due(now) {
                            send(&output,text(json!({"type":"playback_end","epoch":epoch})))?;
                        }
                        if flow.interrupt_stalled(now) && !interrupt_hint_sent {
                            // Input remains enabled while waiting for a fresh user turn.
                            send(&output,text(json!({"type":"hint","message":"请直接说话，或按下键结束"})))?;
                            interrupt_hint_sent=true;
                        }
                    }
                    if last_activity.elapsed()>Duration::from_secs(idle_seconds) || started.elapsed()>Duration::from_secs(1800){
                        send(&output,text(json!({"type":"ended","reason":if started.elapsed()>Duration::from_secs(1800){"limit"}else{"idle"}})))?;
                        break
                    }
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

fn rms_above(bytes: &[u8], threshold: u64) -> bool {
    if bytes.is_empty() {
        return false;
    }
    let energy: u64 = bytes
        .chunks_exact(2)
        .map(|s| {
            let n = i16::from_le_bytes([s[0], s[1]]) as i64;
            (n * n) as u64
        })
        .sum();
    energy > threshold * threshold * (bytes.len() / 2) as u64
}
fn caption(value: &Value) -> Option<Message> {
    let role = value["role"].as_str()?;
    if !matches!(role, "user" | "assistant") {
        return None;
    }
    let content = value["text"].as_str()?;
    let content: String = content.chars().take(180).collect();
    Some(text(
        json!({"type":"transcript","role":role,"text":content}),
    ))
}
fn voice_error(error: &anyhow::Error) -> Message {
    let detail = format!("{error:#}").to_lowercase();
    let (code, message, retryable) =
        if detail.contains("login") || detail.contains("unauthorized") || detail.contains("401") {
            ("account", "服务器账号需要重新登录", false)
        } else if detail.contains("quota")
            || detail.contains("rate limit")
            || detail.contains("usage limit")
        {
            ("quota", "订阅用量暂时不可用，请稍后再试", false)
        } else if detail.contains("queue")
            || detail.contains("timeout")
            || detail.contains("disconnected")
        {
            ("network", "语音连接中断，正在尝试重连", true)
        } else {
            ("service", "语音服务暂不可用，请按 OK 重试", false)
        };
    text(json!({"type":"error","code":code,"message":message,"retryable":retryable}))
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
    fn captions_are_unicode_bounded_and_ignore_other_roles() {
        let Some(Message::Text(message)) =
            caption(&json!({"role":"assistant","text":"语音".repeat(200)}))
        else {
            panic!("caption missing")
        };
        let value: Value = serde_json::from_str(&message).unwrap();
        assert_eq!(value["text"].as_str().unwrap().chars().count(), 180);
        assert!(message.len() < 1200);
        assert!(caption(&json!({"role":"developer","text":"control"})).is_none());
    }
    #[test]
    fn silence_does_not_extend_idle_time() {
        assert!(!rms_above(&[0; FRAME_BYTES], 600));
        let voiced: Vec<u8> = (0..480).flat_map(|_| 1200i16.to_le_bytes()).collect();
        assert!(rms_above(&voiced, 600));
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

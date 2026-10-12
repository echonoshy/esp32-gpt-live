mod adpcm;
mod codex;
mod context;
mod conversation;
mod live;
mod preferences;
mod ptt;
mod reply_buffer;
use conversation::{AudioAction, Conversation};
use preferences::Preferences;
use ptt::PushToTalk;
use reply_buffer::{Delivery, ReplyBuffer};

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
    collections::{HashMap, VecDeque},
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
    tx.try_send(message).map_err(|error| match error {
        mpsc::error::TrySendError::Full(_) => anyhow!("audio queue full"),
        mpsc::error::TrySendError::Closed(_) => anyhow!("audio peer disconnected"),
    })
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
        .route("/voice_setup.js", get(|| async {
            ([("content-type", "text/javascript")], include_str!("../web/voice_setup.js"))
        }))
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
        json!({"ok":true,"device_connected":app.busy.load(Ordering::Relaxed),"sample_rate":24000,"frame_bytes":FRAME_BYTES,"protocol":8,"version":env!("CARGO_PKG_VERSION")}),
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

async fn write_device(
    sink: &mut futures_util::stream::SplitSink<WebSocket, Message>,
    message: Message,
) -> Result<()> {
    tokio::time::timeout(Duration::from_secs(5), sink.send(message))
        .await
        .context("device socket write timeout")??;
    Ok(())
}

async fn device(socket: WebSocket, app: Shared, _slot: Slot) {
    let (mut sink, mut source) = socket.split();
    let (out, mut outgoing) = mpsc::channel(128);
    let mut session: Option<(Tx, tokio::task::JoinHandle<()>)> = None;
    let mut heartbeat = tokio::time::interval(Duration::from_secs(15));
    let mut last_rx = tokio::time::Instant::now();
    let mut compressed = false;
    let mut batched = false;
    let wire_compressed = Arc::new(AtomicBool::new(false));
    let writer_compressed = wire_compressed.clone();
    let batch_downlink = Arc::new(AtomicBool::new(false));
    let writer_batch = batch_downlink.clone();
    // A slow device write must not stop reading its microphone or heartbeat.
    let mut writer = tokio::spawn(async move {
        let mut codec = adpcm::Encoder::default();
        let mut batch = Vec::with_capacity(992);
        let mut deadline = tokio::time::Instant::now();
        loop {
            let message = tokio::select! {
                message = outgoing.recv() => match message { Some(m) => m, None => break },
                _ = tokio::time::sleep_until(deadline), if !batch.is_empty() => {
                    write_device(&mut sink, Message::Binary(std::mem::take(&mut batch).into())).await?;
                    continue;
                }
            };
            let message = match message {
                Message::Binary(bytes) if writer_compressed.load(Ordering::Relaxed) => {
                    if bytes.len() != FRAME_BYTES + 4 {
                        bail!("invalid device output frame");
                    }
                    let mut packet = bytes[..4].to_vec();
                    packet.extend(codec.encode(&bytes[4..]));
                    if writer_batch.load(Ordering::Relaxed) {
                        if batch.is_empty() {
                            deadline = tokio::time::Instant::now() + Duration::from_millis(60);
                        }
                        batch.extend(packet);
                        if batch.len() >= 992 {
                            write_device(
                                &mut sink,
                                Message::Binary(std::mem::take(&mut batch).into()),
                            )
                            .await?;
                        }
                        continue;
                    }
                    Message::Binary(packet.into())
                }
                message => message,
            };
            // Partial audio precedes end/clear/status; never mix epochs or sessions.
            if !batch.is_empty() {
                write_device(
                    &mut sink,
                    Message::Binary(std::mem::take(&mut batch).into()),
                )
                .await?;
            }
            if matches!(message, Message::Close(_)) {
                // Reading Close has already queued Tungstenite's acknowledgement.
                let _ = tokio::time::timeout(Duration::from_secs(1), sink.flush()).await;
                break;
            }
            write_device(&mut sink, message).await?;
        }
        Ok::<_, anyhow::Error>(())
    });
    let _ = send(&out, status("idle"));
    let mut graceful_close = false;
    tracing::info!("device connected");
    'device: loop {
        tokio::select! {
            frame=source.next()=>{
                let frame=match frame {
                    Some(Ok(frame))=>frame,
                    Some(Err(error))=>{tracing::warn!(%error,"device socket read failed");break},
                    None=>{tracing::info!("device socket EOF");break},
                };
                last_rx=tokio::time::Instant::now();
                // In-flight microphone frames can arrive after the session receiver closes.
                // They must not tear down an otherwise healthy device connection.
                if session.as_ref().is_some_and(|(tx,_)|tx.is_closed()) {session=None}
                match frame {
                    Message::Close(close)=>{tracing::info!("device requested socket close");graceful_close=send(&out,Message::Close(close)).is_ok();break},
                    Message::Ping(p)=>{if send(&out,Message::Pong(p)).is_err(){break}},
                    Message::Pong(_)=>{},
                    Message::Binary(bytes)=>{
                        let frames = if compressed {
                            let Some(frames)=adpcm::decode_uplink(&bytes,batched) else {
                                tracing::warn!(bytes=bytes.len(),"invalid device ADPCM payload");break
                            };
                            frames
                        } else {
                            if !valid_pcm(&bytes) {tracing::warn!("invalid device PCM payload");break}
                            vec![bytes.to_vec()]
                        };
                        for frame in frames {
                            if let Some((tx,_))=&session {
                                if send(tx,Message::Binary(frame.into())).is_err(){
                                    tracing::warn!("device input queue full or session closed");
                                    break 'device
                                }
                            }
                        }
                    },
                    Message::Text(s)=>{
                        let Ok(v)=serde_json::from_str::<Value>(&s) else {break};
                        let kind=v["type"].as_str().unwrap_or("");
                        if kind=="start" || kind=="probe" {
                            if session.as_ref().is_some_and(|(_,task)|task.is_finished()) {session=None}
                            if session.is_none() {
                                compressed=v["protocol"].as_u64().unwrap_or(1)>=5;
                                batched=v["protocol"].as_u64().unwrap_or(1)>=7;
                                wire_compressed.store(compressed, Ordering::Relaxed);
                                batch_downlink.store(v["protocol"].as_u64().unwrap_or(1)>=8, Ordering::Relaxed);
                                // Bounded startup backlog: preserve a held utterance while Codex connects.
                                let (tx,rx)=mpsc::channel(if v["mode"]=="continuous" && v["protocol"].as_u64().unwrap_or(1)<4 {32} else {ptt::MAX_FRAMES + 8});
                                let output=out.clone();let shared=app.clone();
                                let prefs=match Preferences::parse(&v) { Ok(p)=>p, Err(error)=>{let _=send(&output,voice_error(&error));continue} };
                                let probe=kind=="probe";
                                session=Some((tx,tokio::spawn(async move {
                                    let result=if probe {run_probe(rx,output.clone()).await} else {run_session(shared,rx,output.clone(),prefs).await};
                                    if let Err(error)=result {
                                        // No account tokens, SDP or conversation text in logs.
                                        tracing::warn!(error=%format!("{error:#}"),"voice session ended with error");
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
                            let _=send(&out,text(json!({"type":"stopped"})));
                        } else if matches!(kind,"ptt_start"|"ptt_end"|"cancel"|"interrupt"|"playback_drained"|"pause"|"resume"|"mode") {
                            if let Some((tx,_))=&session {if send(tx,Message::Text(s)).is_err(){break}}
                        } else if kind!="hello" {break}
                    }
                }
            },
            result=&mut writer=>{
                tracing::warn!(?result,"device writer ended");
                break;
            },
            _=heartbeat.tick()=>{
                if last_rx.elapsed()>Duration::from_secs(45){tracing::warn!("device heartbeat receive timeout");break}
                if send(&out,Message::Ping(Vec::new().into())).is_err(){break}
            }
        }
    }
    if graceful_close && !writer.is_finished() {
        let _ = tokio::time::timeout(Duration::from_secs(1), &mut writer).await;
    }
    writer.abort();
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
            // Independent writer avoids a full receive queue blocking control/input.
            let mut writer = tokio::spawn(async move {
                while let Some(frame) = outgoing.recv().await {
                    if sink.send(frame).await.is_err() {
                        break;
                    }
                }
            });
            loop {
                tokio::select! {
                    _=&mut writer=>break,
                    frame=source.next()=>{
                        match frame {
                            Some(Ok(Message::Close(_)))|None|Some(Err(_))=>break,
                            Some(Ok(frame))=>if incoming.send(frame).await.is_err(){break},
                        }
                    },
                }
            }
            writer.abort();
        })
}

struct PendingRegistration {
    id: String,
    app: Shared,
}

// Authenticated device transport check. No Codex, media worker, or microphone content.
async fn run_probe(mut input: mpsc::Receiver<Message>, output: Tx) -> Result<()> {
    let started = tokio::time::Instant::now();
    let mut tick = tokio::time::interval(Duration::from_millis(20));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut uplink_frames = 0u32;
    let mut downlink_frames = 0u32;
    let mut completed = false;
    let mut pcm = vec![0; FRAME_BYTES + 4];
    pcm[..4].copy_from_slice(&1u32.to_le_bytes());
    send(&output, status("ready"))?;
    send(&output, text(json!({"type":"listen","epoch":1})))?;
    send(&output, text(json!({"type":"playback_begin","epoch":1})))?;
    tracing::info!("silent transport probe started; no upstream voice session");
    loop {
        tokio::select! {
            message=input.recv()=>match message {
                Some(Message::Binary(bytes))=>{
                    if bytes.iter().any(|b|*b!=0) {bail!("probe requires silence");}
                    uplink_frames+=1;
                },
                Some(Message::Text(s)) if serde_json::from_str::<Value>(&s)?["type"]=="stop" =>break,
                None=>break,
                _=>{}
            },
            _=tick.tick()=>{
                if started.elapsed()>=Duration::from_secs(12) {completed=true;break;}
                send(&output,Message::Binary(pcm.clone().into()))?;
                downlink_frames+=1;
            }
        }
    }
    if completed {
        send(&output, text(json!({"type":"playback_end","epoch":1})))?;
        let _ = tokio::time::timeout(Duration::from_secs(3), async {
            while let Some(message) = input.recv().await {
                match message {
                    Message::Binary(_) => uplink_frames += 1,
                    Message::Text(s) => {
                        let value: Value = serde_json::from_str(&s).unwrap_or_default();
                        if value["type"] == "stop"
                            || (value["type"] == "playback_drained" && value["epoch"] == 1)
                        {
                            break;
                        }
                    }
                    _ => {}
                }
            }
        })
        .await;
    }
    tracing::info!(
        uplink_frames,
        downlink_frames,
        "silent transport probe finished"
    );
    send(
        &output,
        text(
            json!({"type":"probe_done","uplink_frames":uplink_frames,"downlink_frames":downlink_frames}),
        ),
    )?;
    Ok(())
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
    prefs: Preferences,
) -> Result<()> {
    let opening = tokio::time::Instant::now();
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
        .arg(format!("http://127.0.0.1:{}/media?voice={}#{}", app.port, prefs.voice, id))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .kill_on_drop(true)
        .spawn()?;
    let mut codex: Option<codex::Codex> = None;
    let result=async {
        // These branches share no session state. Dropping either future on failure
        // also drops its owned Codex child; the browser is cleaned up below.
        let media = async {
            let mut worker=tokio::time::timeout(Duration::from_secs(20),wait).await.context("Chromium startup timeout")??;
            let offer=tokio::time::timeout(Duration::from_secs(15),worker.rx.recv()).await?.context("worker disconnected before offer")?;
            let Message::Text(offer)=offer else {bail!("expected SDP offer")};
            let offer:Value=serde_json::from_str(&offer)?;
            let sdp=offer["sdp"].as_str().context("missing SDP offer")?.to_owned();
            tracing::info!(elapsed_ms=opening.elapsed().as_millis() as u64,"startup media offer ready");
            Ok::<_, anyhow::Error>((worker,sdp))
        };
        let backend = async {
            let mut rpc=codex::Codex::open().await?;
            tracing::info!(elapsed_ms=opening.elapsed().as_millis() as u64,"startup account ready");
            rpc.prepare().await?;
            tracing::info!(elapsed_ms=opening.elapsed().as_millis() as u64,"startup thread ready");
            Ok::<_, anyhow::Error>(rpc)
        };
        let ((mut worker,sdp),rpc)=tokio::try_join!(media,backend)?;
        codex=Some(rpc);
        let rpc=codex.as_mut().unwrap();
        rpc.start(&sdp, &prefs).await?;
        tracing::info!(elapsed_ms=opening.elapsed().as_millis() as u64,"startup realtime accepted");
        let mut continuous=prefs.continuous;
        let fast=prefs.fast_ptt;
        let tagged=prefs.tagged;
        let mut live=live::Live::default();
        let mut live_input = VecDeque::new();
        let mut live_tick = tokio::time::interval(Duration::from_millis(20));
        live_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut live_playing=false;
        let mut live_active=false;
        let mut live_waiting=false;
        let mut ready=false;
        let mut ptt=PushToTalk::new(fast);
        let mut replies=ReplyBuffer::default();
        let mut reply_tick=tokio::time::interval(Duration::from_millis(20));
        reply_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut ptt_started=None;
        let mut released=None;
        let mut first_output=true;
        let mut first_model_audio=true;
        let mut input_quiet_frames=0u32;
        let mut paused=false;
        let mut response_armed=false;
        let mut replay_tail: Option<tokio::time::Instant>=None;
        let mut replay_tick=tokio::time::interval(Duration::from_millis(20));
        replay_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut flow=Conversation::new();
        let mut last_voice=tokio::time::Instant::now()-Duration::from_secs(1);
        let started=tokio::time::Instant::now();
        let mut last_activity=started;
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
                                Some("cancel"|"interrupt")=>{
                                    live_input.clear();live_tick.reset();
                                    // Explicit user action only: keep the same Codex thread.
                                    let epoch=flow.interrupt(started.elapsed().as_millis() as u64);
                                    ptt.cancel();replies.clear();replay_tail=None;response_armed=false;
                                    live.cancel();live_playing=false;live_active=false;live_waiting=false;
                                    send(&output,text(json!({"type":"clear","epoch":epoch})))?;
                                    send(&worker.tx,text(json!({"type":"input","enabled":continuous && !paused,"clear":true})))?;
                                    send(&output,text(json!({"type":if continuous {"activity"}else{"waiting"},"epoch":epoch,"active":false})))?;
                                    rpc.interrupt().await?;
                                    last_activity=tokio::time::Instant::now();
                                },
                                Some("mode") if fast=>{
                                    live_input.clear();live_tick.reset();
                                    last_activity=tokio::time::Instant::now();
                                    continuous=event["mode"]=="continuous";
                                    live.cancel();live_playing=false;live_active=false;live_waiting=false;
                                    ptt.cancel();replies.clear();replay_tail=None;response_armed=false;paused=false;
                                    let epoch=flow.interrupt(started.elapsed().as_millis() as u64);
                                    send(&worker.tx,text(json!({"type":"input","enabled":continuous,"clear":true})))?;
                                    send(&output,text(json!({"type":"clear","epoch":epoch})))?;
                                    send(&output,text(json!({"type":"mode","mode":if continuous {"continuous"}else{"ptt"}})))?;
                                    rpc.interrupt().await?;
                                    tracing::info!(continuous,"conversation mode switched in place");
                                },
                                Some("playback_drained") if tagged=>{
                                    if let Some(epoch)=event["epoch"].as_u64() {
                                        if continuous {
                                            // Live playback never controls microphone admission.
                                        } else if fast {
                                            if u32::try_from(epoch).ok().is_some_and(|e|replies.drained(e)) && replies.is_empty() && !ptt.capturing {
                                                flow.drained(epoch as u32);
                                                send(&output,text(json!({"type":"waiting","epoch":flow.epoch})))?;
                                            }
                                        } else if u32::try_from(epoch).ok().is_some_and(|e|flow.drained(e)) {
                                            send(&worker.tx,text(json!({"type":"input","enabled":if continuous {!paused} else {ptt.replaying || replay_tail.is_some()},"clear":continuous})))?;
                                            send(&output,text(json!({"type":if continuous {"listen"}else{"waiting"},"epoch":flow.epoch})))?;
                                        }
                                    }
                                },
                                Some("pause")=>{
                                    live_input.clear();live_tick.reset();
                                    paused=true;
                                    send(&worker.tx,text(json!({"type":"input","enabled":false,"clear":true})))?;
                                },
                                Some("resume")=>{
                                    live_input.clear();live_tick.reset();
                                    paused=false;last_activity=tokio::time::Instant::now();
                                    send(&worker.tx,text(json!({"type":"input","enabled":continuous,"clear":true})))?;
                                },
                                Some("ptt_start") if !continuous || fast=>{
                                    live_input.clear();live_tick.reset();
                                    let interrupt_old = response_armed || continuous;
                                    continuous=false;paused=false;live_playing=false;live_active=false;live_waiting=false;
                                    ptt.start();replies.clear();replay_tail=None;released=None;first_output=true;first_model_audio=true;
                                    ptt_started=Some(tokio::time::Instant::now());input_quiet_frames=0;
                                    replay_tick.reset();
                                    last_activity=tokio::time::Instant::now();
                                    if interrupt_old { rpc.interrupt().await?; }
                                    response_armed=fast;
                                    let epoch=flow.interrupt(started.elapsed().as_millis() as u64);
                                    send(&output,text(json!({"type":"clear","epoch":epoch})))?;
                                    send(&worker.tx,text(json!({"type":"input","enabled":fast,"clear":true})))?;
                                },
                                Some("ptt_end") if !continuous=>{
                                    last_activity=tokio::time::Instant::now();
                                    response_armed=ptt.release();released=Some(tokio::time::Instant::now());
                                    tracing::info!(fast, capture_ms=ptt_started.map(|t|t.elapsed().as_millis() as u64).unwrap_or(0), queued_input_ms=ptt.queued_ms(), queued_reply_ms=replies.queued_ms(), "ptt released");
                                    if response_armed {
                                        replay_tick.reset();reply_tick.reset();
                                        // Keep already queued input intact in streaming mode.
                                        send(&worker.tx,text(json!({"type":"input","enabled":true,"clear":!fast})))?;
                                        if fast && !ptt.replaying { replay_tail=Some(tokio::time::Instant::now()+Duration::from_millis(600)); }
                                        send(&output,text(json!({"type":"submitted"})))?;
                                    } else {
                                        send(&output,text(json!({"type":"waiting","epoch":flow.epoch,"too_long":ptt.overflow})))?;
                                    }
                                },
                                _=>{}
                            }
                        },
                        Message::Binary(bytes) if !continuous && ptt.capturing=>{
                            ptt.append(&bytes);
                            if ptt.overflow { bail!("PTT recording exceeded 60 seconds"); }
                            last_activity=tokio::time::Instant::now();
                        },
                        Message::Binary(bytes) if ready && continuous && !paused=>{
                            // Silence clocks the transport but never extends the idle timer.
                            if rms_above(&bytes,600) {
                                user_energy_frames+=1;
                                if user_energy_frames>=4 {last_activity=tokio::time::Instant::now();}
                            } else {user_energy_frames=0;}
                            // TCP recovery delivers several seconds in one burst. Pace
                            // those frames instead of overflowing the 32-frame worker queue.
                            if live_input.len() >= 150 { bail!("live input queue exceeded 3 seconds"); }
                            live_input.push_back(bytes);
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
                        Message::Binary(bytes) if ready=>{
                            if !valid_pcm(&bytes){bail!("invalid media frame")}
                            let voiced=bytes.chunks_exact(2).any(|s|i16::from_le_bytes([s[0],s[1]]).unsigned_abs()>30);
                            if continuous {
                                // Forward the full media clock, including silence. Natural user speech
                                // never cancels playback locally or injects a stop instruction.
                                if paused || !live.allowed() {continue;}
                                if !live_playing {
                                    live_playing=true;
                                    send(&output,text(json!({"type":"playback_begin","epoch":flow.epoch})))?;
                                }
                                if voiced {
                                    live_waiting=false;
                                    last_voice=tokio::time::Instant::now();last_activity=last_voice;
                                    if !live_active {
                                        live_active=true;
                                        send(&output,text(json!({"type":"activity","active":true})))?;
                                    }
                                }
                                if tagged {
                                    let mut frame=flow.epoch.to_le_bytes().to_vec();frame.extend_from_slice(&bytes);
                                    send(&output,Message::Binary(frame.into())).context("live audio to device")?;
                                } else {send(&output,Message::Binary(bytes))?;}
                                continue;
                            }
                            if tagged {
                                // Consume silence during capture to establish an interruption boundary.
                                if !continuous && (!response_armed || (!fast && ptt.capturing)) {
                                    if flow.phase==conversation::Phase::Interrupted { flow.audio(started.elapsed().as_millis() as u64,voiced); }
                                    continue;
                                }
                                let action=flow.audio(started.elapsed().as_millis() as u64,voiced);
                                if let AudioAction::Begin(epoch)=action {
                                    if !fast || continuous {send(&output,text(json!({"type":"playback_begin","epoch":epoch})))?;}
                                }
                                if let AudioAction::Begin(epoch)|AudioAction::Frame(epoch)=action {
                                    if voiced {last_activity=tokio::time::Instant::now();}
                                    if fast && !continuous {
                                        if first_model_audio {
                                            tracing::info!(while_held=ptt.capturing, release_to_model_audio_ms=released.map(|t|t.elapsed().as_millis() as u64).unwrap_or(0), "first PTT model audio received");
                                            first_model_audio=false;
                                        }
                                        if !replies.push(epoch,&bytes) {bail!("PTT reply queue exceeded limit");}
                                        continue;
                                    }
                                    let mut tagged=Vec::with_capacity(FRAME_BYTES+4);
                                    tagged.extend_from_slice(&epoch.to_le_bytes());tagged.extend_from_slice(&bytes);
                                    send(&output,Message::Binary(tagged.into())).context("device output")?;
                                }
                            } else if response_armed && !ptt.capturing {
                                if voiced {last_voice=tokio::time::Instant::now();last_activity=last_voice;}
                                if last_voice.elapsed()<Duration::from_millis(200){send(&output,Message::Binary(bytes)).context("device output")?;}
                            }
                        },
                        Message::Text(s)=>{
                            let value:Value=serde_json::from_str(&s)?;
                            match value["type"].as_str() {
                                Some("ready")=>{
                                    if value["voice"].as_str() != Some(prefs.voice.as_str()) || value["voice_ack"] != true {
                                        bail!("voice configuration was not acknowledged");
                                    }
                                    ready=true;
                                    tracing::info!(voice=%prefs.voice,"voice configuration acknowledged");
                                    send(&output,text(json!({"type":"status","state":"ready","voice":prefs.voice})))?;
                                    if continuous {
                                        send(&worker.tx,text(json!({"type":"input","enabled":!paused,"clear":true})))?;
                                        send(&output,text(json!({"type":"listen","epoch":flow.epoch})))?;
                                    } else if !ptt.capturing && !response_armed { send(&output,text(json!({"type":"waiting","epoch":flow.epoch})))?; }
                                    tracing::info!(continuous, open_ms=opening.elapsed().as_millis() as u64,"voice media ready");
                                },
                                Some("error")=>bail!("media worker failed"),
                                Some("media_stats")=>{
                                    tracing::debug!(input_queue_ms=value["input_queue_ms"].as_u64(),
                                        input_dropped_samples=value["input_dropped_samples"].as_u64(),
                                        output_ws_bytes=value["output_ws_bytes"].as_u64(),"media queue");
                                },
                                Some("user_started") if !continuous=>{
                                    flow.user_turn();
                                },
                                Some("transcript_delta"|"transcript")=>{
                                    let role=value["role"].as_str().unwrap_or("");
                                    let has_text=value["text"].as_str().is_some_and(|s|!s.trim().is_empty());
                                    let done=value["type"]=="transcript";
                                    if continuous {
                                        live.transcript(role,done,has_text);
                                        if has_text {last_activity=tokio::time::Instant::now();}
                                        if role=="user" && has_text && !live.awaiting_input() {live_waiting=true;}
                                        if role=="user" && has_text && !live.awaiting_input() && !live_active {
                                            live_active=true;last_voice=tokio::time::Instant::now();
                                            send(&output,text(json!({"type":"activity","active":true})))?;
                                        }
                                    } else if role=="user" && (response_armed && (fast || !ptt.capturing)) {
                                        flow.user_turn();last_activity=tokio::time::Instant::now();
                                    }
                                    if !tagged && done {if let Some(message)=caption(&value) {send(&output,message)?;}}
                                },
                                _=>{}
                            }
                        },
                        _=>{}
                    }
                },
                _=live_tick.tick(), if ready && continuous && !paused=>{
                    if let Some(bytes) = live_input.pop_front() {
                        send(&worker.tx,Message::Binary(bytes)).context("live microphone to media worker")?;
                    }
                },
                _=replay_tick.tick(), if ready && !continuous && ptt.input_active()=>{
                    if let Some(bytes)=ptt.next() {
                        if rms_above(&bytes,600) {
                            if fast && ptt.capturing && input_quiet_frames>=10 && !replies.is_empty() {
                                // User resumed speaking after an early unheard reply: discard that reply.
                                replies.clear();flow.interrupt(started.elapsed().as_millis() as u64);
                                rpc.interrupt().await?;
                            }
                            input_quiet_frames=0;flow.user_turn();
                        } else {input_quiet_frames=input_quiet_frames.saturating_add(1);}
                        last_activity=tokio::time::Instant::now();
                        send(&worker.tx,Message::Binary(bytes.into()))?;
                    } else if !ptt.capturing {
                        // Let the worklet's last queued samples drain before disabling input.
                        replay_tail=Some(tokio::time::Instant::now()+Duration::from_millis(600));
                    }
                },
                _=reply_tick.tick(), if ready && fast && !continuous && !ptt.capturing=>{
                    let mut delivery=replies.next(false);
                    if let Delivery::Begin(epoch)=delivery {
                        send(&output,text(json!({"type":"playback_begin","epoch":epoch})))?;
                        delivery=replies.next(false);
                    }
                    match delivery {
                        Delivery::Frame(bytes)=>{
                            if first_output {
                                tracing::info!(release_to_output_ms=released.map(|t|t.elapsed().as_millis() as u64).unwrap_or(0), "first PTT audio sent to device");
                                first_output=false;
                            }
                            send(&output,Message::Binary(bytes.into()))?;
                        },
                        Delivery::End(epoch)=>send(&output,text(json!({"type":"playback_end","epoch":epoch})))?,
                        _=>{}
                    }
                },
                _=tick.tick()=>{
                    let now=started.elapsed().as_millis() as u64;
                    if !ready && started.elapsed()>Duration::from_secs(30){bail!("WebRTC connection timeout")}
                    if replay_tail.is_some_and(|at|tokio::time::Instant::now()>=at) {
                        replay_tail=None;
                        send(&worker.tx,text(json!({"type":"input","enabled":false,"clear":false})))?;
                    }
                    if tagged && !continuous {
                        if let Some(epoch)=flow.end_due(now) {
                            if fast && !continuous {replies.ended(epoch);} else {send(&output,text(json!({"type":"playback_end","epoch":epoch})))?;}
                        }
                    }
                    // Display-only activity: no audio frame or capture gate depends on this timer.
                    if continuous && live_active && !live_waiting && last_voice.elapsed()>Duration::from_millis(650) {
                        live_active=false;
                        send(&output,text(json!({"type":"activity","active":false})))?;
                    }
                    if last_activity.elapsed()>Duration::from_secs(prefs.idle_seconds) || started.elapsed()>Duration::from_secs(1800){
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

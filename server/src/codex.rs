use anyhow::{anyhow, bail, Context, Result};
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    process::Stdio,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader},
    process::{Child, ChildStdin, Command},
    sync::{mpsc, oneshot},
    task::JoinHandle,
};

type Pending = Arc<Mutex<HashMap<u64, oneshot::Sender<Result<Value>>>>>;

pub struct Codex {
    child: Child,
    stdin: Arc<tokio::sync::Mutex<ChildStdin>>,
    pending: Pending,
    pub events: mpsc::Receiver<Value>,
    reader: JoinHandle<()>,
    next_id: u64,
    _directory: tempfile::TempDir,
    thread: Option<String>,
}

const PROMPT: &str = "你是一个随身语音聊天助手。默认用自然的普通话，简短回答，一次一到三句话。听不清时请用户重说，不要猜测。记住本次对话上下文。闲聊直接回复，仅在确实需要深入推理时委派后台。不操作文件，不运行命令，不调用工具，不承诺保存笔记或提醒。允许用户打断。";

impl Codex {
    pub async fn open() -> Result<Self> {
        let directory = tempfile::tempdir()?;
        let mut command =
            Command::new(std::env::var("CODEX_BIN").unwrap_or_else(|_| "codex".into()));
        command.args(["app-server", "--listen", "stdio://"]);
        for setting in [
            "forced_login_method=\"chatgpt\"",
            "model_reasoning_effort=\"low\"",
            "features.apps=false",
            "features.plugins=false",
            "features.multi_agent=false",
            "features.shell_tool=false",
            "features.browser_use=false",
            "features.computer_use=false",
            "features.hooks=false",
            "features.memories=false",
            "mcp_servers={}",
        ] {
            command.args(["-c", setting]);
        }
        let mut child = command
            .current_dir(directory.path())
            .env_remove("OPENAI_API_KEY")
            .env_remove("CODEX_API_KEY")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true)
            .spawn()
            .context("start Codex")?;
        let stdin = Arc::new(tokio::sync::Mutex::new(child.stdin.take().unwrap()));
        let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
        let pending: Pending = Arc::new(Mutex::new(HashMap::new()));
        let (tx, events) = mpsc::channel(64);
        let replies = pending.clone();
        let writer = stdin.clone();
        let reader = tokio::spawn(async move {
            while let Ok(Some(line)) = lines.next_line().await {
                let Ok(message) = serde_json::from_str::<Value>(&line) else {
                    continue;
                };
                if let Some(id) = message.get("id") {
                    if message.get("method").is_some() {
                        let response = json!({"id":id,"error":{"code":-32601,"message":"Tools unavailable in voice chat"}});
                        let _ = writer
                            .lock()
                            .await
                            .write_all(format!("{response}\n").as_bytes())
                            .await;
                    } else if let Some(id) = id.as_u64() {
                        if let Some(reply) = replies.lock().unwrap().remove(&id) {
                            let result = if let Some(error) = message.get("error") {
                                Err(anyhow!(
                                    "Codex RPC: {}",
                                    error
                                        .get("message")
                                        .and_then(Value::as_str)
                                        .unwrap_or("failed")
                                ))
                            } else {
                                Ok(message["result"].clone())
                            };
                            let _ = reply.send(result);
                        }
                    }
                } else if message["method"]
                    .as_str()
                    .is_some_and(|m| m.starts_with("thread/realtime/"))
                    && tx.try_send(message).is_err()
                {
                    break;
                }
            }
            for (_, reply) in replies.lock().unwrap().drain() {
                let _ = reply.send(Err(anyhow!("Codex disconnected")));
            }
        });
        let mut this = Self {
            child,
            stdin,
            pending,
            events,
            reader,
            next_id: 0,
            _directory: directory,
            thread: None,
        };
        this.rpc("initialize",json!({"clientInfo":{"name":"passport_voice","version":"0.1.0"},"capabilities":{"experimentalApi":true}})).await?;
        this.stdin
            .lock()
            .await
            .write_all(b"{\"method\":\"initialized\",\"params\":{}}\n")
            .await?;
        let account = this
            .rpc("account/read", json!({"refreshToken":false}))
            .await?;
        if account["account"]["type"] != "chatgpt" {
            bail!("ChatGPT subscription login required")
        }
        tracing::info!(plan=?account["account"]["planType"], "subscription authenticated");
        Ok(this)
    }

    pub async fn rpc(&mut self, method: &str, params: Value) -> Result<Value> {
        self.next_id += 1;
        let id = self.next_id;
        let (tx, rx) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, tx);
        let result = async {
            let line = format!("{}\n", json!({"id":id,"method":method,"params":params}));
            self.stdin.lock().await.write_all(line.as_bytes()).await?;
            tokio::time::timeout(Duration::from_secs(45), rx)
                .await
                .context("Codex request timeout")??
        }
        .await;
        self.pending.lock().unwrap().remove(&id);
        result
    }

    pub async fn start(&mut self, sdp: &str) -> Result<()> {
        let response = self.rpc("thread/start",json!({"cwd":self._directory.path(),"ephemeral":true,"approvalPolicy":"never","sandbox":"read-only","environments":[],"baseInstructions":PROMPT,"developerInstructions":PROMPT})).await?;
        let thread = response["thread"]["id"]
            .as_str()
            .context("missing thread id")?
            .to_owned();
        self.thread = Some(thread.clone());
        self.rpc("thread/realtime/start",json!({"threadId":thread,"version":"v3","outputModality":"audio","transport":{"type":"webrtc","sdp":sdp},"codexResponseHandoffMode":"commentary","includeStartupContext":false,"prompt":PROMPT})).await?;
        Ok(())
    }

    pub async fn close(&mut self) {
        if let Some(thread) = self.thread.take() {
            let _ = tokio::time::timeout(
                Duration::from_secs(3),
                self.rpc("thread/realtime/stop", json!({"threadId":thread})),
            )
            .await;
        }
        let _ = self.child.kill().await;
        self.reader.abort();
    }
}

impl Drop for Codex {
    fn drop(&mut self) {
        self.reader.abort();
    }
}

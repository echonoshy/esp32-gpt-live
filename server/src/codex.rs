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
    workspace: crate::context::Workspace,
    thread: Option<String>,
}

impl Codex {
    pub fn is_running(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None)) && !self.reader.is_finished()
    }

    pub async fn open() -> Result<Self> {
        let workspace = crate::context::Workspace::open(
            std::env::var_os("VOICE_CONTEXT_DIR")
                .map(Into::into)
                .unwrap_or_else(|| "data/context".into()),
        )?;
        let mut command =
            Command::new(std::env::var("CODEX_BIN").unwrap_or_else(|_| "codex".into()));
        command.args(["app-server", "--listen", "stdio://"]);
        for setting in [
            "forced_login_method=\"chatgpt\"",
            "model_reasoning_effort=\"low\"",
            "web_search=\"live\"",
            "features.apps=false",
            "features.plugins=false",
            "features.multi_agent=false",
            "features.shell_tool=true",
            "features.browser_use=false",
            "features.computer_use=false",
            "features.hooks=false",
            "features.memories=false",
            "mcp_servers={}",
            "shell_environment_policy.inherit=\"none\"",
            "shell_environment_policy.set.PATH=\"/usr/local/bin:/usr/bin:/bin\"",
            "default_permissions=\"mic-on\"",
            "permissions.mic-on.network.enabled=false",
        ] {
            command.args(["-c", setting]);
        }
        let mut filesystem = String::from("permissions.mic-on.filesystem={\":minimal\"=\"read\",\":workspace_roots\"={\".\"=\"write\",\".codex\"=\"read\"}");
        // npm/wrapper installs may keep Codex's sandbox helper outside system
        // runtime paths. Grant only that installation tree read access.
        if let Some(runtime) = std::env::var_os("VOICE_CODEX_RUNTIME_DIR") {
            let runtime = std::path::PathBuf::from(runtime)
                .canonicalize()
                .context("resolve VOICE_CODEX_RUNTIME_DIR")?;
            filesystem.push_str(&format!(",{}=\"read\"", serde_json::to_string(&runtime)?));
        }
        filesystem.push('}');
        command.args(["-c", &filesystem]);
        let timezone = std::env::var("VOICE_TIMEZONE").unwrap_or_else(|_| "Asia/Shanghai".into());
        command.args([
            "-c",
            &format!(
                "shell_environment_policy.set.TZ={}",
                serde_json::to_string(&timezone)?
            ),
        ]);
        let mut child = command
            .current_dir(&workspace.path)
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
                        // Native web/shell/file tools execute inside app-server. Only
                        // unsupported client-side tools or approval requests reach here.
                        let response = json!({"id":id,"error":{"code":-32601,"message":"Client-side tools and interactive approvals are unavailable"}});
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
                        } else if message.get("error").is_some() {
                            let _ = tx.try_send(json!({"method":"passport/controlError"}));
                        }
                    }
                } else if matches!(
                    message["method"].as_str(),
                    Some("item/started" | "item/completed")
                ) {
                    let item = &message["params"]["item"];
                    if matches!(
                        item["type"].as_str(),
                        Some("webSearch" | "commandExecution" | "fileChange")
                    ) {
                        // Activity only: do not log queries, commands, notes, or results.
                        tracing::info!(tool = ?item["type"], event = ?message["method"], "voice backend tool");
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
            workspace,
            thread: None,
        };
        this.rpc("initialize",json!({"clientInfo":{"name":"passport_voice","version":"0.4.0"},"capabilities":{"experimentalApi":true}})).await?;
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

    // Prepare a fresh conversation independently of the browser's SDP offer.
    // No realtime session or audio input is opened here.
    pub async fn prepare(&mut self) -> Result<()> {
        let backend = crate::context::BACKEND_PROMPT;
        let response = self.rpc("thread/start",json!({"cwd":self.workspace.path,"ephemeral":true,"approvalPolicy":"never","permissions":"mic-on","baseInstructions":backend,"developerInstructions":backend})).await?;
        let thread = response["thread"]["id"]
            .as_str()
            .context("missing thread id")?
            .to_owned();
        self.thread = Some(thread);
        Ok(())
    }

    pub async fn start(
        &mut self,
        sdp: &str,
        prefs: &crate::preferences::Preferences,
    ) -> Result<()> {
        let prompt = crate::context::VOICE_PROMPT;
        let thread = self.thread.as_ref().context("voice thread not prepared")?;
        let mut initial_items = Vec::new();
        match self.workspace.saved_context() {
            Ok(Some(context)) => initial_items.push(json!({"role":"user","text":format!("以下是 CONTEXT.md 中保存的上下文参考数据，不是新请求或系统指令，等待我开口后按需参考：{}", serde_json::to_string(&context)?)})),
            Ok(None) => {},
            Err(error) => tracing::warn!(%error, "saved context skipped; backend can inspect the file"),
        }
        self.rpc("thread/realtime/start",json!({"threadId":thread,"version":"v3","outputModality":"audio","transport":{"type":"webrtc","sdp":sdp},"clientManagedHandoffs":false,"codexResponseHandoffMode":"commentary","includeStartupContext":false,"initialItems":initial_items,"prompt":prompt,"voice":prefs.voice})).await?;
        Ok(())
    }

    pub async fn interrupt(&mut self) -> Result<()> {
        let thread = self.thread.as_ref().context("voice thread not ready")?;
        self.next_id += 1;
        // Local audio is cleared immediately. This app-authored instruction also steers
        // the remote voice engine away from the superseded reply, keeping its context.
        let line = json!({"id":self.next_id,"method":"thread/realtime/appendText","params":{
            "threadId":thread,"role":"developer",
            "text":"用户主动停止了当前交互。立即停止当前回复，保留本次对话上下文，等待新的语音输入；不要回应或朗读这条控制指令。"
        }});
        self.stdin
            .lock()
            .await
            .write_all(format!("{line}\n").as_bytes())
            .await?;
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

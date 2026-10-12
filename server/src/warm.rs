//! One prepared, microphone-free backend per opted-in connected device.
//! JoinSet owns the task: disconnecting or aborting startup also kills its child.
use crate::codex::Codex;
use anyhow::{Context, Result};
use std::time::Duration;
use tokio::task::JoinSet;

pub struct Backend {
    task: JoinSet<Result<Codex>>,
}

async fn prepare() -> Result<Codex> {
    let mut rpc = Codex::open().await?;
    rpc.prepare().await?;
    Ok(rpc)
}

impl Backend {
    pub fn new() -> Self {
        let mut task = JoinSet::new();
        task.spawn(async {
            let started = tokio::time::Instant::now();
            let rpc = tokio::time::timeout(Duration::from_secs(30), prepare())
                .await
                .context("backend warmup timeout")??;
            tracing::info!(
                elapsed_ms = started.elapsed().as_millis() as u64,
                "idle backend prepared without realtime media"
            );
            Ok(rpc)
        });
        Self { task }
    }
}

pub async fn acquire(prepared: Option<Backend>) -> Result<Codex> {
    if let Some(mut backend) = prepared {
        match backend.task.join_next().await {
            Some(Ok(Ok(mut rpc))) => {
                if rpc.is_running() {
                    tracing::info!("startup using prepared backend");
                    return Ok(rpc);
                }
                tracing::warn!("prepared backend exited; restarting on demand");
            }
            _ => tracing::warn!("backend warmup failed; retrying on demand"),
        }
    }
    prepare().await
}

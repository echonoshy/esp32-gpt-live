//! Private immutable firmware artifacts; signatures are produced by the release tool.
use crate::Shared;
use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
};
use serde_json::Value;
fn authorized(app: &crate::App, headers: &HeaderMap) -> bool {
    !app.token.is_empty()
        && headers.get("authorization").and_then(|s| s.to_str().ok())
            == Some(format!("Bearer {}", app.token).as_str())
}
fn directory() -> std::path::PathBuf {
    std::env::var_os("FIRMWARE_DIR")
        .map(Into::into)
        .unwrap_or_else(|| "data/firmware".into())
}
async fn release() -> Option<(Vec<u8>, Value)> {
    let bytes = tokio::fs::read(directory().join("latest.json"))
        .await
        .ok()?;
    if bytes.len() > 2048 {
        return None;
    }
    let value: Value = serde_json::from_slice(&bytes).ok()?;
    let sha = value["sha256"].as_str()?;
    if sha.len() != 64
        || !sha
            .bytes()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
    {
        return None;
    }
    Some((bytes, value))
}
pub async fn manifest(State(app): State<Shared>, headers: HeaderMap) -> Response {
    if !authorized(&app, &headers) {
        return StatusCode::UNAUTHORIZED.into_response();
    }
    match release().await {
        Some((bytes, _)) => (
            [
                ("content-type", "application/json"),
                ("cache-control", "no-store"),
            ],
            bytes,
        )
            .into_response(),
        None => StatusCode::NOT_FOUND.into_response(),
    }
}
pub async fn image(
    State(app): State<Shared>,
    headers: HeaderMap,
    Path(sha): Path<String>,
) -> Response {
    if !authorized(&app, &headers) {
        return StatusCode::UNAUTHORIZED.into_response();
    }
    // Filename validation never permits user-controlled directory traversal.
    if sha.len() != 64
        || !sha
            .bytes()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
    {
        return StatusCode::NOT_FOUND.into_response();
    }
    let file = directory().join(format!("{sha}.bin"));
    let Ok(meta) = tokio::fs::metadata(&file).await else {
        return StatusCode::NOT_FOUND.into_response();
    };
    if !(1024..=0x380000).contains(&meta.len()) {
        return StatusCode::NOT_FOUND.into_response();
    }
    match tokio::fs::read(file).await {
        Ok(bytes) => (
            [
                ("content-type", "application/octet-stream"),
                ("cache-control", "private, no-store"),
            ],
            bytes,
        )
            .into_response(),
        Err(_) => StatusCode::NOT_FOUND.into_response(),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn anonymous_firmware_is_never_public_even_on_local_development() {
        let app = crate::App {
            pending: Default::default(),
            busy: Default::default(),
            token: String::new(),
            port: 8787,
        };
        assert!(!authorized(&app, &HeaderMap::new()));
    }
}

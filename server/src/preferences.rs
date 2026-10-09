use anyhow::{bail, Result};
use serde_json::Value;

// Codex 0.159.2 validates V3 with the V1 voice family (not public API voices).
pub const VOICES: [&str; 9] = [
    "cove", "juniper", "maple", "spruce", "ember", "vale", "breeze", "arbor", "sol",
];
#[derive(Clone, Debug)]
pub struct Preferences {
    pub continuous: bool,
    pub fast_ptt: bool,
    pub tagged: bool,
    pub idle_seconds: u64,
    pub voice: String,
    pub pace: usize,
    pub length: usize,
    pub language: usize,
}
impl Preferences {
    pub fn parse(v: &Value) -> Result<Self> {
        let index = |name: &str, max: u64| -> Result<usize> {
            let n = match v.get(name) {
                None => 0,
                Some(n) => n
                    .as_u64()
                    .ok_or_else(|| anyhow::anyhow!("invalid preference"))?,
            };
            if n > max {
                bail!("invalid preference {name}");
            }
            Ok(n as usize)
        };
        let voice = v["voice"].as_str().unwrap_or("cove");
        if !VOICES.contains(&voice) {
            bail!("unsupported voice");
        }
        let continuous = v["mode"] == "continuous";
        Ok(Self {
            continuous,
            fast_ptt: v["protocol"].as_u64().unwrap_or(1) >= 4,
            tagged: v["protocol"].as_u64().unwrap_or(1) >= 2,
            idle_seconds: v["idle_seconds"].as_u64().unwrap_or(180).clamp(30, 900),
            voice: voice.into(),
            pace: index("pace", 2)?,
            length: index("reply_length", 2)?,
            language: index("language", 2)?,
        })
    }
    pub fn prompt(&self) -> String {
        format!("你是一个随身语音聊天助手。{}{}{}听不清时请用户重说，不要猜测。记住本次对话上下文。闲聊直接回复，仅在确实需要深入推理时委派后台。不操作文件，不运行命令，不调用工具，不承诺保存笔记或提醒。允许用户打断。不主动开场，等待用户说话。",
            ["默认使用自然普通话。", "使用自然英语。", "跟随用户当前使用的语言。 "][self.language],
            ["采用自然语速。", "放慢语速，吐字清楚。", "语速稍快，但保证清晰。 "][self.pace],
            ["简短回答，一到三句话。", "回答极简，通常一句话。", "需要时详细解释，避免重复。 "][self.length])
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn defaults_and_voice_family_are_explicit() {
        let p = Preferences::parse(&json!({})).unwrap();
        assert!(!p.continuous);
        assert_eq!(p.voice, "cove");
        assert!(Preferences::parse(&json!({"voice":"marin"})).is_err());
        assert!(Preferences::parse(&json!({"pace":-1})).is_err());
        assert!(Preferences::parse(&json!({"language":3})).is_err());
        // Legacy duplex=0 cannot silently disable live input in the new gateway.
        assert!(
            Preferences::parse(&json!({"mode":"continuous","duplex":0}))
                .unwrap()
                .continuous
        );
    }
}

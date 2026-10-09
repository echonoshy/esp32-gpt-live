use anyhow::{bail, Result};
use serde_json::Value;

// Codex 0.159.2 validates V3 with the V1 voice family (not public API voices).
pub const VOICES: [&str; 9] = [
    "cove", "juniper", "maple", "spruce", "ember", "vale", "breeze", "arbor", "sol",
];
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Preferences {
    pub continuous: bool,
    pub fast_ptt: bool,
    pub tagged: bool,
    pub idle_seconds: u64,
    pub voice: String,
}
impl Preferences {
    pub fn parse(v: &Value) -> Result<Self> {
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
        })
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
        // Legacy duplex=0 cannot silently disable live input in the new gateway.
        assert!(
            Preferences::parse(&json!({"mode":"continuous","duplex":0}))
                .unwrap()
                .continuous
        );
    }
    #[test]
    fn retired_style_settings_have_no_effect() {
        let current = Preferences::parse(&json!({"voice":"juniper"})).unwrap();
        for legacy in [
            json!({"voice":"juniper","pace":2,"reply_length":1,"language":1}),
            json!({"voice":"juniper","pace":-1,"reply_length":null,"language":3}),
        ] {
            assert_eq!(Preferences::parse(&legacy).unwrap(), current);
        }
    }
}

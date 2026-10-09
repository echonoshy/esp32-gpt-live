//! Half-duplex turn control. Millisecond timestamps make races reproducible in tests.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    Listening,
    Playing,
    Draining,
    Interrupted,
}

pub struct Conversation {
    pub epoch: u32,
    pub phase: Phase,
    last_voice: u64,
    interrupted_at: u64,
    quiet_since: Option<u64>,
    user_after_interrupt: bool,
    quiet_boundary: bool,
}

#[derive(Debug, PartialEq, Eq)]
pub enum AudioAction {
    Ignore,
    Begin(u32),
    Frame(u32),
}

impl Conversation {
    pub fn new() -> Self {
        Self {
            epoch: 0,
            phase: Phase::Listening,
            last_voice: 0,
            interrupted_at: 0,
            quiet_since: None,
            user_after_interrupt: false,
            quiet_boundary: false,
        }
    }
    pub fn listening(&self) -> bool {
        matches!(self.phase, Phase::Listening | Phase::Interrupted)
    }
    pub fn interrupt(&mut self, now: u64) -> u32 {
        self.epoch = self.epoch.wrapping_add(1);
        self.phase = Phase::Interrupted;
        self.interrupted_at = now;
        self.quiet_since = None;
        self.user_after_interrupt = false;
        self.quiet_boundary = false;
        self.epoch
    }
    pub fn user_turn(&mut self) {
        self.user_after_interrupt = true;
        if self.phase == Phase::Interrupted && self.quiet_boundary {
            self.phase = Phase::Listening;
        }
    }
    pub fn audio(&mut self, now: u64, voiced: bool) -> AudioAction {
        if self.phase == Phase::Interrupted {
            if voiced {
                self.quiet_since = None;
            } else {
                let quiet = *self.quiet_since.get_or_insert(now);
                // Ignore the interrupted reply until a quiet boundary and a fresh user turn.
                if now.saturating_sub(quiet) >= 200 {
                    self.quiet_boundary = true;
                    if self.user_after_interrupt {
                        self.phase = Phase::Listening;
                    }
                }
            }
            return AudioAction::Ignore;
        }
        if voiced {
            self.last_voice = now;
            if self.phase != Phase::Playing {
                self.epoch = self.epoch.wrapping_add(1);
                self.phase = Phase::Playing;
                return AudioAction::Begin(self.epoch);
            }
            return AudioAction::Frame(self.epoch);
        }
        if self.phase == Phase::Playing && now.saturating_sub(self.last_voice) < 200 {
            return AudioAction::Frame(self.epoch);
        }
        AudioAction::Ignore
    }
    pub fn end_due(&mut self, now: u64) -> Option<u32> {
        if self.phase == Phase::Playing && now.saturating_sub(self.last_voice) >= 650 {
            self.phase = Phase::Draining;
            return Some(self.epoch);
        }
        None
    }
    pub fn drained(&mut self, epoch: u32) -> bool {
        if self.phase == Phase::Draining && self.epoch == epoch {
            self.phase = Phase::Listening;
            return true;
        }
        false
    }
    pub fn interrupt_stalled(&self, now: u64) -> bool {
        self.phase == Phase::Interrupted && now.saturating_sub(self.interrupted_at) > 30000
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn next_turn_waits_for_physical_playback_to_drain() {
        let mut c = Conversation::new();
        assert!(c.listening());
        assert_eq!(c.audio(100, true), AudioAction::Begin(1));
        assert!(!c.listening());
        assert_eq!(c.end_due(749), None);
        assert_eq!(c.end_due(750), Some(1));
        assert!(!c.listening());
        assert!(!c.drained(0));
        assert!(c.drained(1));
        assert!(c.listening());
        assert_eq!(c.audio(1500, true), AudioAction::Begin(2));
    }
    #[test]
    fn late_audio_and_drain_ack_cannot_revive_an_interrupted_reply() {
        let mut c = Conversation::new();
        c.audio(100, true);
        assert_eq!(c.interrupt(200), 2);
        assert!(!c.drained(1));
        assert_eq!(c.audio(210, true), AudioAction::Ignore);
        c.audio(300, false);
        c.audio(600, false);
        assert_eq!(c.phase, Phase::Interrupted);
        c.user_turn();
        c.audio(620, false);
        assert_eq!(c.phase, Phase::Listening);
        assert_eq!(c.audio(700, true), AudioAction::Begin(3));
    }
    #[test]
    fn fresh_reply_is_not_lost_when_user_event_follows_the_quiet_boundary() {
        let mut c = Conversation::new();
        c.interrupt(0);
        c.audio(20, false);
        c.audio(220, false);
        c.user_turn();
        assert_eq!(c.audio(230, true), AudioAction::Begin(2));
    }
    #[test]
    fn new_user_audio_alone_does_not_release_old_output() {
        let mut c = Conversation::new();
        c.interrupt(0);
        c.user_turn();
        assert_eq!(c.audio(100, true), AudioAction::Ignore);
        c.audio(200, false);
        c.audio(300, true);
        assert_eq!(c.phase, Phase::Interrupted);
        c.audio(400, false);
        c.audio(600, false);
        assert_eq!(c.audio(620, true), AudioAction::Begin(2));
    }
    #[test]
    fn pause_inside_a_reply_cannot_open_microphone_until_drained() {
        let mut c = Conversation::new();
        c.audio(0, true);
        c.end_due(650);
        assert_eq!(c.audio(700, true), AudioAction::Begin(2));
        assert!(!c.drained(1));
        assert!(!c.listening());
    }
}

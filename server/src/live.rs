//! Continuous media never uses a silence/drain boundary to grant microphone permission.
//! Recognized user speech and explicit cancellation suppress output. Caption ownership prevents an old
//! assistant tail (including done-only events) from reopening canceled playback.
#[derive(Default)]
pub struct Live {
    generation: u64,
    user: Option<u64>,
    assistant: Option<u64>,
    suppressed: bool,
    waiting_user: bool,
}
impl Live {
    pub fn cancel(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.suppressed = true;
        self.waiting_user = true;
    }
    pub fn awaiting_input(&self) -> bool {
        self.waiting_user
    }
    pub fn allowed(&self) -> bool {
        !self.suppressed
    }
    pub fn transcript(&mut self, role: &str, done: bool, has_text: bool) -> bool {
        let mut new_input = false;
        match role {
            "user" => {
                if has_text && self.user.is_none() {
                    self.generation = self.generation.wrapping_add(1);
                    self.user = Some(self.generation);
                    self.waiting_user = false;
                    self.suppressed = true;
                    new_input = true;
                }
                if done {
                    self.user = None;
                }
            }
            "assistant" => {
                // A settled old caption alone is never permission to resume audio.
                if !done && has_text {
                    let owner = *self.assistant.get_or_insert(self.generation);
                    if !self.waiting_user && owner == self.generation {
                        self.suppressed = false;
                    }
                }
                if done {
                    self.assistant = None;
                }
            }
            _ => {}
        }
        new_input
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn natural_overlap_clears_once_and_rejects_old_answer() {
        let mut s = Live::default();
        s.transcript("assistant", false, true);
        assert!(s.transcript("user", false, true));
        for _ in 0..100 {
            assert!(!s.transcript("user", false, true));
            s.transcript("assistant", false, true);
            assert!(!s.allowed());
        }
        assert!(!s.transcript("user", true, true));
        s.transcript("assistant", true, true);
        assert!(!s.allowed());
        s.transcript("assistant", false, true);
        assert!(s.allowed());
    }
    #[test]
    fn empty_user_events_do_not_interrupt() {
        let mut s = Live::default();
        assert!(!s.transcript("user", false, false));
        assert!(!s.transcript("user", true, false));
        assert!(s.allowed());
    }
    #[test]
    fn explicit_cancel_rejects_old_tail_and_done_only_ack() {
        let mut s = Live::default();
        s.transcript("assistant", false, true);
        s.cancel();
        s.transcript("assistant", false, true);
        assert!(!s.allowed());
        s.transcript("user", false, true);
        s.transcript("assistant", false, true);
        assert!(!s.allowed());
        s.transcript("assistant", true, true);
        assert!(!s.allowed());
        s.transcript("assistant", false, true);
        assert!(s.allowed());
    }
    #[test]
    fn canceled_user_tail_cannot_count_as_a_new_request() {
        let mut s = Live::default();
        s.transcript("user", false, true);
        s.cancel();
        s.transcript("user", true, true);
        s.transcript("assistant", false, true);
        assert!(!s.allowed());
        s.transcript("assistant", true, true);
        s.transcript("user", false, true);
        s.transcript("assistant", false, true);
        assert!(s.allowed());
    }
    #[test]
    fn empty_completion_closes_old_caption_ownership() {
        let mut s = Live::default();
        s.transcript("user", false, true);
        s.transcript("assistant", false, true);
        s.cancel();
        s.transcript("user", true, false);
        s.transcript("assistant", true, false);
        assert!(s.awaiting_input());
        s.transcript("user", false, true);
        assert!(!s.awaiting_input());
        assert!(!s.allowed());
        s.transcript("assistant", false, true);
        assert!(s.allowed());
    }
}

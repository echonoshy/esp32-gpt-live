//! Bounded input backlog. Protocol 4 streams during capture; older clients replay on release.
use std::collections::VecDeque;
pub const MAX_FRAMES: usize = 3000; // 60 seconds at 20 ms per frame, < 3 MiB.
#[derive(Default)]
pub struct PushToTalk {
    frames: VecDeque<Vec<u8>>,
    streaming: bool,
    total: usize,
    pub capturing: bool,
    pub replaying: bool,
    pub overflow: bool,
}
impl PushToTalk {
    pub fn new(streaming: bool) -> Self {
        Self {
            streaming,
            ..Self::default()
        }
    }
    pub fn queued_ms(&self) -> usize {
        self.frames.len() * 20
    }
    pub fn input_active(&self) -> bool {
        (self.streaming && self.capturing) || self.replaying
    }
    pub fn start(&mut self) {
        self.cancel();
        self.capturing = true;
    }
    pub fn append(&mut self, frame: &[u8]) {
        if !self.capturing || self.overflow {
            return;
        }
        if self.total == MAX_FRAMES {
            self.frames.clear();
            self.overflow = true;
        } else {
            self.total += 1;
            self.frames.push_back(frame.to_vec());
        }
    }
    pub fn release(&mut self) -> bool {
        self.capturing = false;
        self.replaying = !self.overflow && !self.frames.is_empty();
        !self.overflow && self.total > 0
    }
    pub fn next(&mut self) -> Option<Vec<u8>> {
        if !self.input_active() {
            return None;
        }
        let frame = self.frames.pop_front();
        if frame.is_none() {
            self.replaying = false;
        }
        frame
    }
    pub fn cancel(&mut self) {
        self.frames.clear();
        self.total = 0;
        self.capturing = false;
        self.replaying = false;
        self.overflow = false;
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn streaming_input_reaches_model_while_held_and_empty_release_still_arms_reply() {
        let mut p = PushToTalk::new(true);
        p.start();
        p.append(&[1]);
        p.append(&[2]);
        assert_eq!(p.queued_ms(), 40);
        assert_eq!(p.next(), Some(vec![1]));
        assert_eq!(p.next(), Some(vec![2]));
        assert_eq!(p.next(), None);
        assert!(p.capturing);
        assert!(p.release());
        assert!(!p.input_active());
        p.start();
        assert!(!p.release());
    }
    #[test]
    fn streaming_limit_counts_consumed_frames_and_release_preserves_tail() {
        let mut p = PushToTalk::new(true);
        p.start();
        for _ in 0..MAX_FRAMES {
            p.append(&[1]);
            assert_eq!(p.next(), Some(vec![1]));
        }
        p.append(&[2]);
        assert!(p.overflow);
        assert!(!p.release());
        p.start();
        p.append(&[3]);
        assert!(p.release());
        assert_eq!(p.next(), Some(vec![3]));
    }
    #[test]
    fn nothing_reaches_model_before_release_and_order_is_preserved() {
        let mut p = PushToTalk::default();
        p.start();
        p.append(&[1]);
        p.append(&[2]);
        assert_eq!(p.next(), None);
        assert!(p.release());
        assert_eq!(p.next(), Some(vec![1]));
        assert_eq!(p.next(), Some(vec![2]));
        assert_eq!(p.next(), None);
    }
    #[test]
    fn new_press_cancels_replay_and_overflow_never_sends_partial_audio() {
        let mut p = PushToTalk::default();
        p.start();
        p.append(&[1]);
        p.release();
        p.start();
        assert_eq!(p.next(), None);
        for _ in 0..=MAX_FRAMES {
            p.append(&[2]);
        }
        assert!(p.overflow);
        assert!(!p.release());
        assert_eq!(p.next(), None);
        p.start();
        p.append(&[3]);
        assert!(p.release());
        assert_eq!(p.next(), Some(vec![3]));
    }
}

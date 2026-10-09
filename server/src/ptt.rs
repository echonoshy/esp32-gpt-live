//! In-memory capture: no microphone samples reach GPT until release.
use std::collections::VecDeque;
pub const MAX_FRAMES: usize = 3000; // 60 seconds at 20 ms per frame, < 3 MiB.
#[derive(Default)]
pub struct PushToTalk {
    frames: VecDeque<Vec<u8>>,
    pub capturing: bool,
    pub replaying: bool,
    pub overflow: bool,
}
impl PushToTalk {
    pub fn start(&mut self) {
        self.cancel();
        self.capturing = true;
    }
    pub fn append(&mut self, frame: &[u8]) {
        if !self.capturing || self.overflow {
            return;
        }
        if self.frames.len() == MAX_FRAMES {
            self.frames.clear();
            self.overflow = true;
        } else {
            self.frames.push_back(frame.to_vec());
        }
    }
    pub fn release(&mut self) -> bool {
        self.capturing = false;
        self.replaying = !self.overflow && !self.frames.is_empty();
        self.replaying
    }
    pub fn next(&mut self) -> Option<Vec<u8>> {
        if !self.replaying {
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
        self.capturing = false;
        self.replaying = false;
        self.overflow = false;
    }
}
#[cfg(test)]
mod tests {
    use super::*;
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

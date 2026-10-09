//! Keep early PTT replies intact, and pace their release without overflowing device playback.
use std::collections::VecDeque;
const MAX_REPLY_FRAMES: usize = 3000;
#[derive(Debug, PartialEq, Eq)]
pub enum Delivery {
    Begin(u32),
    Frame(Vec<u8>),
    End(u32),
    None,
}
#[derive(Default)]
pub struct ReplyBuffer {
    frames: VecDeque<(u32, Vec<u8>)>,
    playing: Option<u32>,
    ended: Option<u32>,
    waiting_drain: bool,
}
impl ReplyBuffer {
    pub fn clear(&mut self) {
        *self = Self::default();
    }
    pub fn queued_ms(&self) -> usize {
        self.frames.len() * 20
    }
    pub fn push(&mut self, epoch: u32, pcm: &[u8]) -> bool {
        if self.frames.len() == MAX_REPLY_FRAMES {
            return false;
        }
        self.frames.push_back((epoch, pcm.to_vec()));
        true
    }
    pub fn ended(&mut self, epoch: u32) {
        self.ended = Some(epoch);
    }
    pub fn next(&mut self, held: bool) -> Delivery {
        if held || self.waiting_drain {
            return Delivery::None;
        }
        if let Some((epoch, _)) = self.frames.front() {
            match self.playing {
                None => {
                    self.playing = Some(*epoch);
                    return Delivery::Begin(*epoch);
                }
                Some(old) if old != *epoch => {
                    self.waiting_drain = true;
                    return Delivery::End(old);
                }
                _ => {}
            }
            let (epoch, pcm) = self.frames.pop_front().unwrap();
            let mut tagged = epoch.to_le_bytes().to_vec();
            tagged.extend(pcm);
            return Delivery::Frame(tagged);
        }
        if let Some(epoch) = self.playing {
            if self.ended == Some(epoch) {
                self.waiting_drain = true;
                return Delivery::End(epoch);
            }
        }
        Delivery::None
    }
    pub fn drained(&mut self, epoch: u32) -> bool {
        if self.playing != Some(epoch) || !self.waiting_drain {
            return false;
        }
        self.waiting_drain = false;
        self.playing = None;
        true
    }
    pub fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn keeps_reply_prefix_while_held_and_waits_for_drain_before_next_epoch() {
        let mut q = ReplyBuffer::default();
        q.push(3, &[1]);
        q.push(3, &[2]);
        q.ended(3);
        q.push(4, &[9]);
        assert_eq!(q.next(true), Delivery::None);
        assert_eq!(q.next(false), Delivery::Begin(3));
        assert_eq!(q.next(false), Delivery::Frame(vec![3, 0, 0, 0, 1]));
        assert_eq!(q.next(false), Delivery::Frame(vec![3, 0, 0, 0, 2]));
        assert_eq!(q.next(false), Delivery::End(3));
        assert!(!q.drained(2));
        assert_eq!(q.next(false), Delivery::None);
        assert!(q.drained(3));
        assert_eq!(q.next(false), Delivery::Begin(4));
        q.clear();
        assert_eq!(q.next(false), Delivery::None);
    }
    #[test]
    fn cannot_grow_without_limit() {
        let mut q = ReplyBuffer::default();
        for _ in 0..MAX_REPLY_FRAMES {
            assert!(q.push(1, &[0]));
        }
        assert!(!q.push(1, &[0]));
        q.clear();
        assert!(q.push(2, &[1]));
    }
}

//! Independent 20 ms IMA/DVI packets: predictor i16 LE, index u8, zero u8, 240 bytes.
pub const PACKET_BYTES: usize = 244;
const STEPS: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];
const INDICES: [i32; 8] = [-1, -1, -1, -1, 2, 4, 6, 8];
#[derive(Default)]
pub struct Encoder {
    predictor: i32,
    index: i32,
}
impl Encoder {
    fn sample(&mut self, code: u8) -> i16 {
        let step = STEPS[self.index as usize];
        let mut delta = step >> 3;
        if code & 4 != 0 {
            delta += step;
        }
        if code & 2 != 0 {
            delta += step >> 1;
        }
        if code & 1 != 0 {
            delta += step >> 2;
        }
        self.predictor =
            (self.predictor + if code & 8 != 0 { -delta } else { delta }).clamp(-32768, 32767);
        self.index = (self.index + INDICES[(code & 7) as usize]).clamp(0, 88);
        self.predictor as i16
    }
    pub fn encode(&mut self, pcm: &[u8]) -> Vec<u8> {
        assert_eq!(pcm.len(), 960);
        let mut packet = vec![0; PACKET_BYTES];
        packet[..2].copy_from_slice(&(self.predictor as i16).to_le_bytes());
        packet[2] = self.index as u8;
        for (i, bytes) in pcm.chunks_exact(2).enumerate() {
            let value = i16::from_le_bytes([bytes[0], bytes[1]]) as i32;
            let mut diff = value - self.predictor;
            let mut code = if diff < 0 { 8 } else { 0 };
            diff = diff.abs();
            let step = STEPS[self.index as usize];
            if diff >= step {
                code |= 4;
                diff -= step;
            }
            if diff >= step >> 1 {
                code |= 2;
                diff -= step >> 1;
            }
            if diff >= step >> 2 {
                code |= 1;
            }
            self.sample(code);
            if i % 2 == 0 {
                packet[4 + i / 2] = code << 4;
            } else {
                packet[4 + i / 2] |= code;
            }
        }
        packet
    }
}
pub fn decode(packet: &[u8]) -> Option<Vec<u8>> {
    if packet.len() != PACKET_BYTES || packet[2] > 88 || packet[3] != 0 {
        return None;
    }
    let mut state = Encoder {
        predictor: i16::from_le_bytes([packet[0], packet[1]]) as i32,
        index: packet[2] as i32,
    };
    let mut pcm = Vec::with_capacity(960);
    for byte in &packet[4..] {
        pcm.extend_from_slice(&state.sample(byte >> 4).to_le_bytes());
        pcm.extend_from_slice(&state.sample(byte & 15).to_le_bytes());
    }
    Some(pcm)
}
/// Protocol 7 batches at most four independent packets; older clients send one.
pub fn decode_uplink(bytes: &[u8], batched: bool) -> Option<Vec<Vec<u8>>> {
    let count = bytes.len() / PACKET_BYTES;
    if count == 0
        || count > if batched { 4 } else { 1 }
        || !bytes.len().is_multiple_of(PACKET_BYTES)
    {
        return None;
    }
    bytes.chunks_exact(PACKET_BYTES).map(decode).collect()
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bounded_batches_keep_packet_boundaries_and_reject_bad_tails() {
        let mut bytes = vec![0; PACKET_BYTES * 4];
        bytes[PACKET_BYTES] = 0x20;
        let frames = decode_uplink(&bytes, true).unwrap();
        assert_eq!(frames.len(), 4);
        for (packet, pcm) in bytes.chunks_exact(PACKET_BYTES).zip(frames) {
            assert_eq!(decode(packet).unwrap(), pcm);
        }
        assert!(decode_uplink(&bytes, false).is_none());
        assert!(decode_uplink(&[], true).is_none());
        assert!(decode_uplink(&bytes[..bytes.len() - 1], true).is_none());
        assert!(decode_uplink(&vec![0; PACKET_BYTES * 5], true).is_none());
        bytes[PACKET_BYTES * 3 + 2] = 89;
        assert!(decode_uplink(&bytes, true).is_none());
    }
    fn unhex(s: &str) -> Vec<u8> {
        s.as_bytes()
            .chunks_exact(2)
            .map(|b| u8::from_str_radix(std::str::from_utf8(b).unwrap(), 16).unwrap())
            .collect()
    }
    #[test]
    fn matches_independent_python_audioop_vectors() {
        let vectors: serde_json::Value =
            serde_json::from_str(include_str!("../../scripts/fixtures/adpcm.json")).unwrap();
        for v in vectors.as_array().unwrap() {
            let mut encoder = Encoder {
                predictor: v["initial"][0].as_i64().unwrap() as i32,
                index: v["initial"][1].as_i64().unwrap() as i32,
            };
            let pcm = unhex(v["pcm"].as_str().unwrap());
            let expected = unhex(v["encoded"].as_str().unwrap());
            assert_eq!(encoder.encode(&pcm), expected);
            assert_eq!(
                decode(&expected).unwrap(),
                unhex(v["decoded"].as_str().unwrap())
            );
            assert_eq!(encoder.predictor, v["final"][0].as_i64().unwrap() as i32);
            assert_eq!(encoder.index, v["final"][1].as_i64().unwrap() as i32);
        }
    }
    #[test]
    fn rejects_invalid_states_and_lengths() {
        let mut p = [0; PACKET_BYTES];
        assert_eq!(decode(&p), Some(vec![0; 960]));
        p[2] = 89;
        assert!(decode(&p).is_none());
        p[2] = 0;
        p[3] = 1;
        assert!(decode(&p).is_none());
        assert!(decode(&p[..240]).is_none());
    }
    #[test]
    fn packet_can_be_decoded_after_missing_previous_packet() {
        let mut e = Encoder::default();
        let pcm: Vec<u8> = (0..480)
            .flat_map(|i| ((i * 89 - 21000) as i16).to_le_bytes())
            .collect();
        let _lost = e.encode(&pcm);
        let next = e.encode(&pcm);
        assert_eq!(decode(&next).unwrap().len(), 960);
        assert_eq!(next.len(), 244);
    }
}

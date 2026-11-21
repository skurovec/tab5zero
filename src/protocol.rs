pub const DESCRIPTORS: [u8; 66] = [
    3, 0, 0, 0, 66, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 9, 4, 0, 0, 2, 255, 0x54, 0x35, 1,
    7, 5, 0x81, 2, 64, 0, 0, 7, 5, 2, 2, 64, 0, 0, 9, 4, 0, 0, 2, 255, 0x54, 0x35, 1, 7, 5, 0x81,
    2, 0, 2, 0, 7, 5, 2, 2, 0, 2, 0,
];
pub const STRINGS: &[u8] = b"\x02\0\0\0\x24\0\0\0\x01\0\0\0\x01\0\0\0\x09\x04Tab5 display link\0";
pub const PRODUCT: &str = "Tab5 display link";

pub fn frame_header(sequence: u32, length: usize) -> Option<[u8; 16]> {
    if !(1..=1048576).contains(&length) {
        return None;
    }
    let mut header = *b"T5F1\0\0\0\0\0\0\0\0\0\x05\xd0\x02";
    header[4..8].copy_from_slice(&sequence.to_le_bytes());
    header[8..12].copy_from_slice(&(length as u32).to_le_bytes());
    Some(header)
}

#[derive(Debug, PartialEq, Eq)]
pub enum Input {
    Keyboard([u8; 8]),
    Pointer { x: u16, y: u16, pressed: bool },
}

#[derive(Default)]
pub struct Parser(Vec<u8>);

impl Parser {
    pub fn push(&mut self, bytes: &[u8], mut receive: impl FnMut(Input)) -> usize {
        self.0.extend_from_slice(bytes);
        let mut cursor = 0;
        let mut skipped = 0;
        while self.0.len() - cursor >= 4 {
            let remaining = &self.0[cursor..];
            if &remaining[..4] != b"T5I1" {
                cursor += 1;
                skipped += 1;
                continue;
            }
            let Some(record) = remaining.get(..16) else {
                break;
            };
            let x = u16::from_le_bytes([record[8], record[9]]);
            let y = u16::from_le_bytes([record[10], record[11]]);
            let input = match record[4] {
                1 if record[9] == 0 => Some(Input::Keyboard(record[8..16].try_into().unwrap())),
                2 if x < 1280 && y < 720 && record[12] <= 1 && record[13..16] == [0; 3] => {
                    Some(Input::Pointer {
                        x,
                        y,
                        pressed: record[12] == 1,
                    })
                }
                _ => None,
            };
            if let Some(input) = input.filter(|_| record[5..8] == [0; 3]) {
                receive(input);
                cursor += 16;
            } else {
                cursor += 1;
                skipped += 1;
            }
        }
        self.0.drain(..cursor);
        skipped
    }
}

// USB HID keyboard usages 0x04–0xa4, with zero for usages without a direct mapping, indexed by usage minus four.
const KEYS: &[u16] = &[
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45,
    21, 44, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41,
    51, 52, 53, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104,
    111, 107, 109, 106, 105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73,
    82, 83, 86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 134,
    138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113, 115, 114, 0, 0, 0, 121, 0, 89, 93, 124,
    92, 94, 95, 0, 0, 0, 122, 123, 90, 91, 85, 0, 0, 0, 0, 0, 0, 222, 99, 223, 355, 0, 28, 121, 0,
    0, 355, 353, 0,
];

const MODIFIERS: [u16; 8] = [29, 42, 56, 125, 97, 54, 100, 126];

pub fn key_code(usage: u8) -> Option<u16> {
    let code = match usage {
        4..=0xa4 => KEYS.get(usize::from(usage - 4)).copied(),
        0xb6 => Some(179),
        0xb7 => Some(180),
        0xe0..=0xe7 => Some(MODIFIERS[usize::from(usage - 0xe0)]),
        _ => None,
    };
    code.filter(|&code| code != 0)
}

#[derive(Default)]
pub struct Keyboard([u16; 14]);

impl Keyboard {
    pub fn update(&mut self, report: [u8; 8], mut emit: impl FnMut(u16, bool)) {
        let mut next = self.0;
        for (bit, code) in MODIFIERS.into_iter().enumerate() {
            next[bit] = if report[0] & (1 << bit) != 0 { code } else { 0 };
        }
        if !report[2..].iter().any(|usage| (1..=3).contains(usage)) {
            for (key, &usage) in next[8..].iter_mut().zip(&report[2..]) {
                *key = key_code(usage).filter(|_| usage < 0xe0).unwrap_or(0);
            }
        }
        for (old, new, pressed) in [(&self.0, &next, false), (&next, &self.0, true)] {
            for (index, &code) in old.iter().enumerate() {
                if code != 0 && !new.contains(&code) && !old[..index].contains(&code) {
                    emit(code, pressed);
                }
            }
        }
        self.0 = next;
    }
}

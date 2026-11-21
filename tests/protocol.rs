use tab5_host::protocol::{
    DESCRIPTORS, Input, Keyboard, PRODUCT, Parser, STRINGS, frame_header, key_code,
};

#[test]
fn frame_header_matches_wire_contract() {
    assert_eq!(
        frame_header(0x01020304, 0x100000),
        Some(*b"T5F1\x04\x03\x02\x01\0\0\x10\0\0\x05\xd0\x02")
    );
    assert_eq!(frame_header(0, 0), None);
    assert_eq!(frame_header(0, 1048577), None);
}

#[test]
fn descriptors_match_full_and_high_speed_layout() {
    assert_eq!(&DESCRIPTORS, b"\x03\0\0\0\x42\0\0\0\x03\0\0\0\x03\0\0\0\x03\0\0\0\x09\x04\0\0\x02\xff\x54\x35\x01\x07\x05\x81\x02\x40\0\0\x07\x05\x02\x02\x40\0\0\x09\x04\0\0\x02\xff\x54\x35\x01\x07\x05\x81\x02\0\x02\0\x07\x05\x02\x02\0\x02\0");
    assert_eq!(
        &STRINGS[..18],
        b"\x02\0\0\0\x24\0\0\0\x01\0\0\0\x01\0\0\0\x09\x04"
    );
    assert_eq!(STRINGS.len(), 36);
    assert_eq!(&STRINGS[18..35], PRODUCT.as_bytes());
    assert_eq!(STRINGS[35], 0);
}

#[test]
fn parser_recovers_from_garbage_and_invalid_records_across_reads() {
    let mut parser = Parser::default();
    let mut records = Vec::new();
    let mut receive = |record| records.push(record);
    assert_eq!(parser.push(b"abcT5", &mut receive), 2);
    assert_eq!(
        parser.push(
            b"I1\x03\0\0\0\0\0\0\0\0\0\0\0T5I1\x01\0\0\0\x02",
            &mut receive
        ),
        17
    );
    assert_eq!(
        parser.push(
            b"\0\x04\0\0\0\0\0T5I1\x02\0\0\0\xff\x04\xcf\x02\x01\0\0\0",
            receive
        ),
        0
    );
    assert_eq!(
        records,
        [
            Input::Keyboard([2, 0, 4, 0, 0, 0, 0, 0]),
            Input::Pointer {
                x: 1279,
                y: 719,
                pressed: true
            }
        ]
    );
}

#[test]
fn keyboard_diffs_key_sets_and_handles_rollover() {
    let mut keyboard = Keyboard::default();
    let mut events = Vec::new();
    let mut emit = |code, pressed| events.push((code, pressed));
    keyboard.update([2, 0, 4, 4, 5, 0, 0, 0], &mut emit);
    keyboard.update([2, 0, 5, 4, 0, 0, 0, 0], &mut emit);
    keyboard.update([0, 0, 1, 1, 1, 1, 1, 1], &mut emit);
    keyboard.update([0; 8], emit);
    assert_eq!(
        events,
        [
            (42, true),
            (30, true),
            (48, true),
            (42, false),
            (48, false),
            (30, false)
        ]
    );
}

#[test]
fn keyboard_mapping_spot_checks() {
    for (usage, code) in [
        (4, 30),
        (0x27, 11),
        (0x29, 1),
        (0x2b, 15),
        (0x38, 53),
        (0x3a, 59),
        (0x45, 88),
        (0x4a, 102),
        (0x4b, 104),
        (0x4c, 111),
        (0x4d, 107),
        (0x4e, 109),
        (0x4f, 106),
        (0x52, 103),
        (0x73, 194),
        (0x87, 89),
        (0xe7, 126),
    ] {
        assert_eq!(key_code(usage), Some(code), "usage {usage:#04x}");
    }
    assert_eq!(key_code(1), None);
}

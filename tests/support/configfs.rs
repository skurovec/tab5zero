use super::ATTRIBUTES;

#[test]
fn gadget_identity_matches_wire_contract() {
    let attribute = |name| {
        ATTRIBUTES
            .iter()
            .find(|(key, _)| *key == name)
            .unwrap()
            .1
            .as_bytes()
    };
    assert_eq!(attribute("idVendor"), b"0x1209");
    assert_eq!(attribute("idProduct"), b"0x0001");
    assert_eq!(attribute("bcdUSB"), b"0x0200");
    assert_eq!(attribute("strings/0x409/product"), b"Tab5 display link");
    assert_eq!(attribute("max_speed"), b"high-speed");
}

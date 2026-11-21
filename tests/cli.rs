use std::fs;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

#[test]
fn captures_two_decodable_frames_without_overwriting_an_existing_file() {
    let path = std::env::temp_dir().join(format!(
        "tab5-test-{}-{}.mjpg",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    let output = Command::new(env!("CARGO_BIN_EXE_tab5-host"))
        .args([
            "capture",
            "--source",
            "test-pattern",
            "--frames",
            "2",
            "--file",
        ])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let bytes = fs::read(&path).unwrap();
    let boundaries = bytes
        .windows(2)
        .enumerate()
        .filter(|(_, pair)| *pair == [0xff, 0xd9])
        .map(|(index, _)| index + 2)
        .collect::<Vec<_>>();
    assert_eq!(boundaries.len(), 2);
    assert_eq!(boundaries[1], bytes.len());
    for jpeg in [&bytes[..boundaries[0]], &bytes[boundaries[0]..]] {
        let image = image::load_from_memory_with_format(jpeg, image::ImageFormat::Jpeg).unwrap();
        assert_eq!((image.width(), image.height()), (1280, 720));
    }
    let overwrite = Command::new(env!("CARGO_BIN_EXE_tab5-host"))
        .args([
            "capture",
            "--source",
            "test-pattern",
            "--frames",
            "1",
            "--file",
        ])
        .arg(&path)
        .output()
        .unwrap();
    assert!(!overwrite.status.success());
    assert_eq!(fs::read(&path).unwrap(), bytes);
    fs::remove_file(path).unwrap();
}

#[test]
fn rejects_unavailable_framebuffers_and_invalid_options() {
    for arguments in [
        vec!["capture", "--quality", "0"],
        vec!["capture", "--source", "x11"],
        vec!["capture", "--frames", "0"],
        vec![
            "capture",
            "--device",
            "/does-not-exist",
            "--file",
            "/tmp/tab5-unused-output.mjpg",
        ],
        vec!["capture", "--file"],
        vec!["unknown"],
    ] {
        let output = Command::new(env!("CARGO_BIN_EXE_tab5-host"))
            .args(arguments)
            .output()
            .unwrap();
        assert!(!output.status.success());
        assert!(!output.stderr.is_empty());
    }
}

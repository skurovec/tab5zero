#[test]
#[ignore = "requires the system TurboJPEG runtime; run in release mode"]
fn compare_720p_encoders() {
    use crate::{RGB_BYTES, encode_jpeg, test_pattern};
    use std::hint::black_box;
    use std::time::Instant;

    let frames: Vec<_> = (0..15)
        .map(|number| {
            let mut rgb = vec![0; RGB_BYTES];
            test_pattern(&mut rgb, number).unwrap();
            rgb
        })
        .collect();
    let mut encoder = super::Encoder::open().unwrap();
    let mut output = vec![0; 1048576];
    black_box(encode_jpeg(&frames[0], 65).unwrap());
    encoder.encode(&frames[0], 65, &mut output).unwrap();
    let start = Instant::now();
    for rgb in &frames {
        black_box(encode_jpeg(black_box(rgb), 65).unwrap());
    }
    let pure_rust = start.elapsed();
    let start = Instant::now();
    for rgb in &frames {
        black_box(encoder.encode(black_box(rgb), 65, &mut output).unwrap());
    }
    let turbo = start.elapsed();
    let length = encoder
        .encode(&frames[0], 65, &mut output)
        .unwrap()
        .expect("benchmark frame fits the output buffer");
    let decoded = image::load_from_memory(&output[..length]).unwrap();
    assert_eq!((decoded.width(), decoded.height()), (1280, 720));
    assert!(
        output[..length]
            .windows(2)
            .any(|marker| marker == [0xff, 0xc0])
    );
    println!(
        "15 pre-generated 720p frames, quality 65: image {:.3} ms/frame, TurboJPEG {:.3} ms/frame",
        pure_rust.as_secs_f64() * 1000.0 / 15.0,
        turbo.as_secs_f64() * 1000.0 / 15.0
    );
}

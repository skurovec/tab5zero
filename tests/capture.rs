use image::ImageDecoder;
use tab5_host::{HEIGHT, MAX_JPEG_BYTES, RGB_BYTES, WIDTH, encode_jpeg, test_pattern};

#[test]
fn pattern_encodes_to_a_real_720p_jpeg() {
    let mut rgb = vec![0u8; RGB_BYTES];
    test_pattern(&mut rgb, 0).unwrap();
    let jpeg = encode_jpeg(&rgb, 65).unwrap();
    assert!(jpeg.len() < MAX_JPEG_BYTES);
    let decoder = image::codecs::jpeg::JpegDecoder::new(std::io::Cursor::new(jpeg)).unwrap();
    assert_eq!(decoder.dimensions(), (1280, 720));
    let mut decoded = vec![0u8; decoder.total_bytes() as usize];
    decoder.read_image(&mut decoded).unwrap();
    let center = ((HEIGHT / 2 * WIDTH + WIDTH / 2) * 3) as usize;
    assert!(decoded[center] > 110 && decoded[center] < 145);
    assert!(decoded[center + 1] > 110 && decoded[center + 1] < 145);
    assert!(decoded[2] < 20);
    assert!(decoded[decoded.len() - 3] > 230);
}

#[test]
fn pattern_changes_between_frames() {
    let mut first = vec![0; RGB_BYTES];
    let mut second = vec![0; RGB_BYTES];
    test_pattern(&mut first, 0).unwrap();
    test_pattern(&mut second, 10).unwrap();
    assert_ne!(first, second);
}

#[test]
fn invalid_capture_buffers_and_encoding_options_return_errors() {
    assert!(test_pattern(&mut [0; 3], 0).is_err());
    assert!(encode_jpeg(&[0; 3], 65).is_err());
    assert!(encode_jpeg(&vec![0; RGB_BYTES], 0).is_err());
    assert!(encode_jpeg(&vec![0; RGB_BYTES], 96).is_err());
}

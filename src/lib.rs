mod fast_jpeg;
pub mod gadget;
pub mod protocol;
use std::error::Error;
use std::ffi::{CString, c_char, c_int, c_void};
use std::io::{self, Write};
use std::ptr::NonNull;

pub type Result<T> = std::result::Result<T, Box<dyn Error + Send + Sync>>;
pub const WIDTH: u32 = 1280;
pub const HEIGHT: u32 = 720;
pub const MAX_JPEG_BYTES: usize = 2 * 1024 * 1024;
pub const FRAME_INTERVAL: std::time::Duration = std::time::Duration::from_nanos(66_666_600);
pub const RGB_BYTES: usize = WIDTH as usize * HEIGHT as usize * 3;

unsafe extern "C" {
    fn tab5_fb_open(path: *const c_char) -> *mut c_void;
    fn tab5_fb_capture(handle: *mut c_void, rgb: *mut u8, length: usize) -> c_int;
    fn tab5_fb_close(handle: *mut c_void) -> c_int;
}

fn native_result(result: c_int, context: &str) -> Result<c_int> {
    if result < 0 {
        Err(format!("{context}: {}", io::Error::from_raw_os_error(-result)).into())
    } else {
        Ok(result)
    }
}

pub struct Framebuffer(NonNull<c_void>);
impl Framebuffer {
    pub fn open(path: &str) -> Result<Self> {
        let path_c = CString::new(path)?;
        // The native object owns a read-only descriptor and mmap until Drop.
        let handle = unsafe { tab5_fb_open(path_c.as_ptr()) };
        NonNull::new(handle).map(Self).ok_or_else(|| {
            format!("opening framebuffer {path}: {}", io::Error::last_os_error()).into()
        })
    }

    pub fn capture(&mut self, rgb: &mut [u8]) -> Result<()> {
        if rgb.len() != RGB_BYTES {
            return Err("RGB capture buffer must hold exactly 1280 × 720 pixels".into());
        }
        // The exclusive native handle and output slice remain valid for this call.
        native_result(
            unsafe { tab5_fb_capture(self.0.as_ptr(), rgb.as_mut_ptr(), rgb.len()) },
            "capturing framebuffer (true-color 16/24/32-bit fbdev required)",
        )?;
        Ok(())
    }
}
impl Drop for Framebuffer {
    fn drop(&mut self) {
        if let Err(error) = native_result(
            unsafe { tab5_fb_close(self.0.as_ptr()) },
            "closing framebuffer",
        ) {
            eprintln!("{error}");
        }
    }
}

pub fn test_pattern(rgb: &mut [u8], frame: u64) -> Result<()> {
    if rgb.len() != RGB_BYTES {
        return Err("test pattern buffer must hold exactly 1280 × 720 RGB pixels".into());
    }
    for (index, pixel) in rgb.as_chunks_mut::<3>().0.iter_mut().enumerate() {
        let x = index as u32 % WIDTH;
        let y = index as u32 / WIDTH;
        pixel.copy_from_slice(&[
            (x * 255 / WIDTH) as u8,
            (y * 255 / HEIGHT) as u8,
            u64::from(x).wrapping_add(frame.wrapping_mul(8)) as u8,
        ]);
    }
    Ok(())
}

pub fn encode_jpeg(rgb: &[u8], quality: u8) -> Result<Vec<u8>> {
    if rgb.len() != RGB_BYTES || !(1..=95).contains(&quality) {
        return Err("JPEG encoding requires 1280 × 720 RGB and quality 1–95".into());
    }
    let mut jpeg = BoundedJpeg(Vec::with_capacity(256 * 1024));
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, quality).encode(
        rgb,
        WIDTH,
        HEIGHT,
        image::ExtendedColorType::Rgb8,
    )?;
    Ok(jpeg.0)
}

pub struct DrmCapture {
    process: std::process::Child,
    output: std::process::ChildStdout,
}
impl DrmCapture {
    pub fn open(device: &str) -> Result<Self> {
        let mut process = std::process::Command::new("ffmpeg")
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-nostdin",
                "-threads",
                "1",
                "-filter_threads",
                "1",
                "-f",
                "kmsgrab",
                "-device",
                device,
                "-framerate",
                "15",
                "-i",
                "-",
                "-vf",
                "hwdownload,format=bgr0,scale=1280:720:flags=fast_bilinear",
                "-pix_fmt",
                "rgb24",
                "-f",
                "rawvideo",
                "pipe:1",
            ])
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::inherit())
            .spawn()
            .map_err(|error| format!("starting FFmpeg kmsgrab for {device}: {error}"))?;
        let output = process
            .stdout
            .take()
            .ok_or("FFmpeg stdout pipe was not created")?;
        Ok(Self { process, output })
    }

    pub fn capture(&mut self, rgb: &mut [u8]) -> Result<()> {
        use std::io::Read;
        if rgb.len() != RGB_BYTES {
            return Err("DRM capture buffer must contain 1280 × 720 RGB pixels".into());
        }
        self.output.read_exact(rgb).map_err(|error| format!("reading FFmpeg kmsgrab frame: {error}; kmsgrab requires DRM master/CAP_SYS_ADMIN and a linear BGR0 scanout buffer; inspect FFmpeg diagnostics above").into())
    }

    fn capture_for_usb(&mut self, mut rgb: &mut [u8]) -> Result<()> {
        use std::io::Read;
        use std::os::fd::AsRawFd;

        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        let mut pipe = libc::pollfd {
            fd: self.output.as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        };
        while !rgb.is_empty() {
            let remaining = deadline.saturating_duration_since(std::time::Instant::now());
            if remaining.is_zero() {
                return Err("timed out reading a frame from FFmpeg kmsgrab".into());
            }
            // The stdout pipe is exclusively read by this worker; poll bounds each wait.
            let ready = unsafe { libc::poll(&mut pipe, 1, remaining.as_millis() as i32 + 1) };
            if ready == 0 {
                return Err("timed out reading a frame from FFmpeg kmsgrab".into());
            }
            let length = if ready < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(format!("waiting for FFmpeg kmsgrab: {error}").into());
            } else {
                match self.output.read(rgb) {
                    Ok(0) => return Err("FFmpeg kmsgrab stopped producing frames; check DRM permissions, the scanout format and FFmpeg diagnostics".into()),
                    Ok(length) => length,
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                    Err(error) => return Err(format!("reading FFmpeg kmsgrab: {error}").into()),
                }
            };
            rgb = &mut rgb[length..];
        }
        Ok(())
    }
}
impl Drop for DrmCapture {
    fn drop(&mut self) {
        match self.process.try_wait() {
            Ok(Some(status)) => {
                if !status.success() {
                    eprintln!("FFmpeg kmsgrab exited: {status}");
                }
            }
            Ok(None) => {
                if let Err(error) = self.process.kill() {
                    eprintln!("stopping FFmpeg kmsgrab: {error}");
                }
                if let Err(error) = self.process.wait() {
                    eprintln!("reaping FFmpeg kmsgrab: {error}");
                }
            }
            Err(error) => eprintln!("checking FFmpeg kmsgrab status: {error}"),
        }
    }
}

struct BoundedJpeg(Vec<u8>);
impl Write for BoundedJpeg {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > MAX_JPEG_BYTES - self.0.len() {
            return Err(io::Error::other("JPEG exceeds 2 MiB; reduce --quality"));
        }
        self.0.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

use crate::{RGB_BYTES, Result, native_result};
use std::ffi::{c_int, c_void};
use std::ptr::NonNull;

unsafe extern "C" {
    fn tab5_jpeg_open() -> *mut c_void;
    fn tab5_jpeg_close(encoder: *mut c_void);
    fn tab5_jpeg_encode(
        encoder: *mut c_void,
        rgb: *const u8,
        quality: c_int,
        output: *mut u8,
        capacity: usize,
    ) -> c_int;
}

pub(crate) struct Encoder(NonNull<c_void>);

impl Encoder {
    pub(crate) fn open() -> Result<Self> {
        // The returned compressor is exclusively owned through Drop.
        NonNull::new(unsafe { tab5_jpeg_open() }).map(Self)
            .ok_or_else(|| "opening TurboJPEG; install the package providing libturbojpeg.so.0 (libturbojpeg0 on Raspberry Pi OS Bookworm)".into())
    }

    /// Returns `None` when the JPEG does not fit into `output`.
    pub(crate) fn encode(
        &mut self,
        rgb: &[u8],
        quality: u8,
        output: &mut [u8],
    ) -> Result<Option<usize>> {
        if rgb.len() != RGB_BYTES {
            return Err("TurboJPEG input must contain exactly 1280 × 720 RGB pixels".into());
        }
        // Both buffers remain valid and the compressor is exclusively borrowed.
        let result = unsafe {
            tab5_jpeg_encode(
                self.0.as_ptr(),
                rgb.as_ptr(),
                quality.into(),
                output.as_mut_ptr(),
                output.len(),
            )
        };
        if result == -libc::EMSGSIZE {
            return Ok(None);
        }
        Ok(Some(
            native_result(result, "encoding 1280 × 720 TurboJPEG frame")? as usize,
        ))
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        // The compressor was returned by tab5_jpeg_open and is still live.
        unsafe { tab5_jpeg_close(self.0.as_ptr()) };
    }
}

#[cfg(test)]
#[path = "../tests/support/jpeg_benchmark.rs"]
mod benchmark;

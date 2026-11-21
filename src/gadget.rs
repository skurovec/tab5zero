mod input;
mod system;

use crate::fast_jpeg::Encoder;
use crate::protocol::{DESCRIPTORS, Parser, STRINGS, frame_header};
use crate::{DrmCapture, FRAME_INTERVAL, Framebuffer, RGB_BYTES, Result};
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::OpenOptionsExt;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

pub use system::doctor;

unsafe extern "C" {
    fn tab5_endpoint_packet_size(descriptor: libc::c_int) -> libc::c_int;
}

const RETRY: Duration = Duration::from_millis(5);
static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn stop(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}

#[derive(Default)]
struct Connection {
    generation: u64,
    enabled: bool,
    suspended: bool,
}

enum Source {
    Framebuffer(Framebuffer),
    Drm(DrmCapture),
}

pub fn serve(source: &str, device: &str, quality: u8) -> Result<()> {
    // The handler only performs a lock-free atomic store.
    for signal in [libc::SIGINT, libc::SIGTERM] {
        if unsafe { libc::signal(signal, stop as *const () as libc::sighandler_t) } == libc::SIG_ERR
        {
            return Err(
                format!("installing signal handler: {}", io::Error::last_os_error()).into(),
            );
        }
    }
    let mut gadget = system::Gadget::prepare()?;
    let result = (|| {
        let mut control = OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(libc::O_NONBLOCK)
            .open(std::path::Path::new(system::MOUNT).join("ep0"))?;
        for bytes in [&DESCRIPTORS[..], STRINGS] {
            if control.write(bytes)? != bytes.len() {
                return Err("short FunctionFS ep0 descriptor write".into());
            }
        }
        let video = OpenOptions::new()
            .write(true)
            .custom_flags(libc::O_NONBLOCK)
            .open(std::path::Path::new(system::MOUNT).join("ep1"))?;
        let input = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NONBLOCK)
            .open(std::path::Path::new(system::MOUNT).join("ep2"))?;
        let devices = Mutex::new(input::Devices::open()?);
        let connection = Mutex::new(Connection::default());
        let stopping = AtomicBool::new(false);
        gadget.bind()?;
        thread::scope(|scope| {
            let video_task =
                scope.spawn(|| stream(video, source, device, quality, &connection, &stopping));
            let input_task = scope.spawn(|| receive(input, &connection, &devices, &stopping));
            let events = (|| {
                let mut event = [0; 12];
                while !STOP.load(Ordering::Relaxed)
                    && !video_task.is_finished()
                    && !input_task.is_finished()
                {
                    match control.read(&mut event) {
                        Ok(12) => {
                            let mut state = connection.lock().unwrap();
                            match event[8] {
                                0 => {}
                                1 | 3 => {
                                    state.enabled = false;
                                    state.generation = state.generation.wrapping_add(1);
                                    devices.lock().unwrap().release()?;
                                }
                                2 => {
                                    state.enabled = true;
                                    state.suspended = false;
                                    state.generation = state.generation.wrapping_add(1);
                                    devices.lock().unwrap().release()?;
                                }
                                4 => stall_setup(&mut control, event[0])?,
                                5 => state.suspended = true,
                                6 => state.suspended = false,
                                other => {
                                    return Err(format!("unknown FunctionFS event {other}").into());
                                }
                            }
                        }
                        Err(error)
                            if retryable(&error) || error.raw_os_error() == Some(libc::EIDRM) =>
                        {
                            thread::sleep(RETRY)
                        }
                        Err(error) => {
                            return Err(format!("reading FunctionFS events: {error}").into());
                        }
                        Ok(length) => {
                            return Err(format!("short FunctionFS event ({length} bytes)").into());
                        }
                    }
                }
                Ok(())
            })();
            stopping.store(true, Ordering::Relaxed);
            let unbind = gadget.unbind();
            drop(control);
            let video_result = video_task
                .join()
                .map_err(|_| "video worker panicked".into())
                .and_then(|result| result.map_err(|error| format!("video worker: {error}").into()));
            let input_result = input_task
                .join()
                .map_err(|_| "input worker panicked".into())
                .and_then(|result| result.map_err(|error| format!("input worker: {error}").into()));
            let release = devices.lock().unwrap().release();
            let mut outcome = events;
            for result in [unbind, video_result, input_result, release] {
                if let Err(error) = result {
                    if outcome.is_err() {
                        eprintln!("{error}");
                    } else {
                        outcome = Err(error);
                    }
                }
            }
            outcome
        })
    })();
    let cleanup = gadget.cleanup();
    match (result, cleanup) {
        (Err(error), Err(cleanup)) => Err(format!("{error}; cleanup: {cleanup}").into()),
        (Err(error), Ok(())) | (Ok(()), Err(error)) => Err(error),
        (Ok(()), Ok(())) => Ok(()),
    }
}

fn retryable(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
    )
}

fn disconnected(error: &io::Error) -> bool {
    matches!(
        error.raw_os_error(),
        Some(libc::ESHUTDOWN | libc::ENODEV | libc::ECONNRESET)
    )
}

fn stall_setup(control: &mut File, request_type: u8) -> Result<()> {
    // FunctionFS stalls an unsupported setup if userspace uses the opposite direction.
    // Raw syscalls ensure that a zero-length operation reaches ep0.
    let descriptor = control.as_raw_fd();
    let result = unsafe {
        if request_type & 0x80 != 0 {
            libc::read(descriptor, std::ptr::null_mut(), 0)
        } else {
            libc::write(descriptor, std::ptr::null(), 0)
        }
    };
    if result < 0 {
        let error = io::Error::last_os_error();
        // FunctionFS reports a performed stall as EL2HLT; EIDRM/ECANCELED mean the setup was superseded.
        if !matches!(
            error.raw_os_error(),
            Some(libc::EL2HLT | libc::EIDRM | libc::ECANCELED)
        ) && !disconnected(&error)
        {
            return Err(format!("stalling unsupported USB setup: {error}").into());
        }
    }
    Ok(())
}

fn receive(
    mut endpoint: File,
    connection: &Mutex<Connection>,
    devices: &Mutex<input::Devices>,
    stopping: &AtomicBool,
) -> Result<()> {
    let mut parser = Parser::default();
    let mut generation = None;
    let mut bytes = [0; 512];
    let mut packet_size = 0;
    while !stopping.load(Ordering::Relaxed) {
        let current = {
            let state = connection.lock().unwrap();
            (state.enabled && !state.suspended).then_some(state.generation)
        };
        let Some(current) = current else {
            thread::sleep(RETRY);
            continue;
        };
        if generation != Some(current) {
            // A full-size packet must complete this read even if the host sends no ZLP.
            let size = unsafe { tab5_endpoint_packet_size(endpoint.as_raw_fd()) };
            match size {
                64 | 512 => packet_size = size as usize,
                negative if negative < 0 => {
                    let error = io::Error::from_raw_os_error(-negative);
                    if disconnected(&error) || retryable(&error) {
                        thread::sleep(RETRY);
                        continue;
                    }
                    return Err(format!("reading bulk OUT endpoint descriptor: {error}").into());
                }
                other => return Err(format!("invalid bulk OUT packet size {other}").into()),
            }
            generation = Some(current);
            parser = Parser::default();
        }
        let length = match endpoint.read(&mut bytes[..packet_size]) {
            Ok(0) => {
                thread::sleep(RETRY);
                continue;
            }
            Ok(length) => length,
            Err(error) if retryable(&error) || disconnected(&error) => {
                thread::sleep(RETRY);
                continue;
            }
            Err(error) => return Err(format!("reading bulk OUT: {error}").into()),
        };
        let state = connection.lock().unwrap();
        if !state.enabled || state.generation != current {
            continue;
        }
        let mut devices = devices.lock().unwrap();
        let mut result = Ok(());
        let skipped = parser.push(&bytes[..length], |record| {
            if result.is_ok() {
                result = devices.inject(record);
            }
        });
        if skipped != 0 {
            eprintln!("bulk OUT: skipped {skipped} invalid input bytes");
        }
        result?;
    }
    Ok(())
}

fn stream(
    mut endpoint: File,
    source: &str,
    device: &str,
    quality: u8,
    connection: &Mutex<Connection>,
    stopping: &AtomicBool,
) -> Result<()> {
    let mut capture = None;
    let mut encoder = Encoder::open()?;
    let mut rgb = vec![0; RGB_BYTES];
    let mut previous = vec![0; RGB_BYTES];
    let mut packet = vec![0; 16 + 1048576];
    let mut last_sent = None;
    let mut sequence = 0u32;
    while !stopping.load(Ordering::Relaxed) {
        let current = {
            let state = connection.lock().unwrap();
            (state.enabled && !state.suspended).then_some(state.generation)
        };
        let Some(generation) = current else {
            if !connection.lock().unwrap().enabled {
                capture = None;
            }
            thread::sleep(RETRY);
            continue;
        };
        let started = Instant::now();
        if capture
            .as_ref()
            .is_none_or(|(active, _)| *active != generation)
        {
            drop(capture.take());
            let source = match source {
                "framebuffer" => Source::Framebuffer(Framebuffer::open(device)?),
                "drm" => Source::Drm(DrmCapture::open(device)?),
                _ => return Err("unsupported screen source".into()),
            };
            capture = Some((generation, source));
        }
        match &mut capture.as_mut().unwrap().1 {
            Source::Framebuffer(capture) => capture.capture(&mut rgb)?,
            Source::Drm(capture) => capture.capture_for_usb(&mut rgb)?,
        }
        if last_sent.is_none_or(|(sent_generation, time): (u64, Instant)| {
            generation != sent_generation
                || time.elapsed() >= Duration::from_secs(1)
                || rgb != previous
        }) {
            let Some(length) = encoder.encode(&rgb, quality, &mut packet[16..])? else {
                // Keep serving; the next changed frame is usually smaller.
                eprintln!(
                    "skipping frame: JPEG exceeds 1 MiB at quality {quality}; lower --quality"
                );
                thread::sleep(FRAME_INTERVAL.saturating_sub(started.elapsed()));
                continue;
            };
            packet[..16].copy_from_slice(
                &frame_header(sequence, length).ok_or("invalid encoded JPEG length")?,
            );
            let mut offset = 0;
            while offset < 16 + length && !stopping.load(Ordering::Relaxed) {
                {
                    let state = connection.lock().unwrap();
                    if !state.enabled || state.generation != generation {
                        break;
                    }
                }
                let end = (offset + 16384).min(16 + length);
                // End each frame with a short USB packet at either supported speed.
                let end = if end == 16 + length && (end - offset).is_multiple_of(64) {
                    end - 1
                } else {
                    end
                };
                match endpoint.write(&packet[offset..end]) {
                    Ok(0) => return Err("zero-length bulk IN write".into()),
                    Ok(written) => offset += written,
                    Err(error) if disconnected(&error) => break,
                    Err(error)
                        if retryable(&error) || error.raw_os_error() == Some(libc::EIDRM) =>
                    {
                        thread::sleep(RETRY)
                    }
                    Err(error) => return Err(format!("writing bulk IN: {error}").into()),
                }
            }
            if offset == 16 + length {
                sequence = sequence.wrapping_add(1);
                last_sent = Some((generation, started));
                std::mem::swap(&mut rgb, &mut previous);
            }
        }
        thread::sleep(FRAME_INTERVAL.saturating_sub(started.elapsed()));
    }
    Ok(())
}

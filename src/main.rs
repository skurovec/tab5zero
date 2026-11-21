use std::fs::{self, File};
use std::io::{self, Write};
use std::process::ExitCode;
use std::time::Instant;
use tab5_host::{
    DrmCapture, FRAME_INTERVAL, Framebuffer, RGB_BYTES, Result, encode_jpeg, test_pattern,
};

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("tab5-host: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let mut arguments = std::env::args().skip(1);
    match arguments.next().as_deref() {
        None | Some("--help" | "-h") => {
            println!(
                "tab5-host — Linux framebuffer to 1280 × 720 MJPEG\n\nCommands:\n  doctor\n  serve --source drm --device /dev/dri/card0\n  capture --source framebuffer --device /dev/fb0 --file capture.mjpg --frames 15\n  capture --source drm --device /dev/dri/card0 --file desktop.mjpg --frames 15\n  capture --source test-pattern --file pattern.mjpg --frames 3\n\nOptions: --quality 1..95 (default 65), --frames 1..1000000 (default 15).\nCapture writes concatenated JPEG frames. No system or USB configuration is changed."
            );
            Ok(())
        }
        Some("doctor") => {
            if arguments.next().is_some() {
                return Err("doctor takes no options".into());
            }
            println!("Read-only hardware checks:");
            for directory in [
                "/sys/class/graphics",
                "/sys/class/drm",
                "/sys/class/udc",
                "/sys/class/video4linux",
            ] {
                match fs::read_dir(directory) {
                    Ok(entries) => {
                        let mut names = Vec::new();
                        for entry in entries {
                            names.push(entry?.file_name().to_string_lossy().into_owned());
                        }
                        names.sort();
                        println!(
                            "{directory}: {}",
                            if names.is_empty() {
                                "none".into()
                            } else {
                                names.join(", ")
                            }
                        );
                    }
                    Err(error) if error.kind() == io::ErrorKind::NotFound => {
                        println!("{directory}: unavailable")
                    }
                    Err(error) => return Err(format!("inspecting {directory}: {error}").into()),
                }
            }
            tab5_host::gadget::doctor()?;
            match Framebuffer::open("/dev/fb0") {
                Ok(_) => println!("/dev/fb0: readable supported true-color framebuffer"),
                Err(error) => println!("/dev/fb0: {error}"),
            }
            println!(
                "DRM capture uses FFmpeg kmsgrab; it requires a linear BGR0 scanout plane and DRM master/CAP_SYS_ADMIN. A fbdev node may expose a console instead of the desktop."
            );
            Ok(())
        }
        Some("serve") => {
            let mut source = "drm".to_owned();
            let mut device = None;
            let mut quality = 65;
            while let Some(option) = arguments.next() {
                let value = arguments
                    .next()
                    .ok_or_else(|| format!("missing value for {option}"))?;
                match option.as_str() {
                    "--source" => source = value,
                    "--device" => device = Some(value),
                    "--quality" => {
                        quality = value
                            .parse::<u8>()
                            .ok()
                            .filter(|quality| (1..=95).contains(quality))
                            .ok_or("--quality must be an integer from 1 to 95")?
                    }
                    _ => return Err(format!("unknown option: {option}").into()),
                }
            }
            let default_device = match source.as_str() {
                "drm" => "/dev/dri/card0",
                "framebuffer" => "/dev/fb0",
                _ => return Err("--source must be framebuffer or drm".into()),
            };
            tab5_host::gadget::serve(
                &source,
                device.as_deref().unwrap_or(default_device),
                quality,
            )
        }
        Some("capture") => {
            let mut source = "framebuffer".to_owned();
            let mut device = None;
            let mut file = None;
            let mut frames = 15u64;
            let mut quality = 65u8;
            while let Some(option) = arguments.next() {
                let value = arguments
                    .next()
                    .ok_or_else(|| format!("missing value for {option}"))?;
                match option.as_str() {
                    "--source" => source = value,
                    "--device" => device = Some(value),
                    "--file" => file = Some(value),
                    "--frames" => {
                        frames = value
                            .parse()
                            .map_err(|_| "--frames must be a positive integer")?
                    }
                    "--quality" => {
                        quality = value
                            .parse()
                            .map_err(|_| "--quality must be an integer from 1 to 95")?
                    }
                    _ => return Err(format!("unknown option: {option}").into()),
                }
            }
            if !(1..=1_000_000).contains(&frames) {
                return Err("--frames must be between 1 and 1000000".into());
            }
            if !(1..=95).contains(&quality) {
                return Err("--quality must be between 1 and 95".into());
            }
            if !matches!(source.as_str(), "framebuffer" | "drm" | "test-pattern") {
                return Err("--source must be framebuffer, drm, or test-pattern".into());
            }
            let file_path = file.ok_or("capture requires --file (a new local .mjpg file)")?;
            let device = device.unwrap_or_else(|| {
                if source == "drm" {
                    "/dev/dri/card0".to_owned()
                } else {
                    "/dev/fb0".to_owned()
                }
            });
            let mut capture = if source == "framebuffer" {
                Some(Framebuffer::open(&device)?)
            } else {
                None
            };
            let mut drm_capture = if source == "drm" {
                Some(DrmCapture::open(&device)?)
            } else {
                None
            };
            let mut output = File::create_new(&file_path)
                .map_err(|error| format!("creating {file_path}: {error}"))?;
            let mut rgb = vec![0u8; RGB_BYTES];
            let started = Instant::now();
            for frame in 0..frames {
                let deadline = Instant::now() + FRAME_INTERVAL;
                if let Some(capture) = &mut capture {
                    capture.capture(&mut rgb)?;
                } else if let Some(capture) = &mut drm_capture {
                    capture.capture(&mut rgb)?;
                } else {
                    test_pattern(&mut rgb, frame)?;
                }
                let jpeg = encode_jpeg(&rgb, quality)?;
                output
                    .write_all(&jpeg)
                    .map_err(|error| format!("writing {file_path}: {error}"))?;
                std::thread::sleep(deadline.saturating_duration_since(Instant::now()));
            }
            output
                .flush()
                .map_err(|error| format!("flushing {file_path}: {error}"))?;
            eprintln!(
                "Wrote {frames} MJPEG frames to {file_path} in {:.2}s (15 fps target; actual device performance requires hardware verification).",
                started.elapsed().as_secs_f64()
            );
            Ok(())
        }
        Some(command) => Err(format!("unknown command: {command}; use --help").into()),
    }
}

use crate::Result;
use crate::protocol::PRODUCT;
use std::ffi::CString;
use std::fs::{self, File, OpenOptions};
use std::io;
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

const CONFIG: &str = "/sys/kernel/config";
const GADGETS: &str = "/sys/kernel/config/usb_gadget";
const GADGET: &str = "/sys/kernel/config/usb_gadget/tab5-display-link";
const CONTROLLERS: &str = "/sys/class/udc";
pub(super) const MOUNT: &str = "/run/tab5-display-link";
const FUNCTION: &str = "tab5-display-link";

const NO_CONTROLLER: &str = "no USB device controller found; add dtoverlay=dwc2,dr_mode=peripheral to the Raspberry Pi boot config.txt, reboot, and use the Cardputer Slave USB-C port";
const BUSY_CONTROLLER: &str = "USB device controller is already in use; disable ADB in Settings > Developer and stop any other USB gadget using the controller";

const ATTRIBUTES: &[(&str, &str)] = &[
    ("idVendor", "0x1209"),
    ("idProduct", "0x0001"),
    ("bcdUSB", "0x0200"),
    ("max_speed", "high-speed"),
    ("strings/0x409/product", PRODUCT),
    ("configs/c.1/bmAttributes", "0xc0"),
    ("configs/c.1/MaxPower", "0"),
];

#[cfg(test)]
#[path = "../../tests/support/configfs.rs"]
mod tests;

pub fn doctor() -> Result<()> {
    let filesystems = fs::read_to_string("/proc/filesystems")?;
    for (filesystem, path) in [
        ("configfs", Path::new(CONFIG)),
        ("functionfs", Path::new(MOUNT)),
    ] {
        println!(
            "{filesystem}: registered={}, mounted={}",
            filesystems
                .lines()
                .any(|line| line.split_whitespace().last() == Some(filesystem)),
            mount_source(path, filesystem)?.is_some()
        );
    }
    if !Path::new(GADGETS).is_dir() {
        println!("USB gadget configfs unavailable; load libcomposite");
    }
    match controllers() {
        Ok(controllers) => {
            for controller in controllers {
                let path = Path::new(CONTROLLERS).join(&controller).join("function");
                match fs::read_to_string(&path) {
                    Ok(function) => {
                        println!("UDC {controller}: bound function {:?}", function.trim())
                    }
                    Err(error) => println!("UDC {controller}: reading {}: {error}", path.display()),
                }
            }
        }
        Err(error) => println!("UDC: {error}"),
    }
    match OpenOptions::new().write(true).open("/dev/uinput") {
        Ok(_) => println!("/dev/uinput: writable"),
        Err(error) => println!("/dev/uinput: {error}; load uinput and check permissions"),
    }
    Ok(())
}

fn controllers() -> Result<Vec<String>> {
    let entries = fs::read_dir(Path::new(CONTROLLERS)).map_err(|error| {
        if error.kind() == io::ErrorKind::NotFound {
            NO_CONTROLLER.to_owned()
        } else {
            format!("reading {}: {error}", Path::new(CONTROLLERS).display())
        }
    })?;
    let mut names = Vec::new();
    for entry in entries {
        names.push(
            entry?
                .file_name()
                .into_string()
                .map_err(|_| "invalid UDC name")?,
        );
    }
    names.sort();
    if names.is_empty() {
        return Err(NO_CONTROLLER.into());
    }
    Ok(names)
}

fn mount_source(path: &Path, filesystem: &str) -> Result<Option<String>> {
    let entries = fs::read_to_string("/proc/self/mountinfo")?;
    for entry in entries.lines().rev() {
        if let Some((before, after)) = entry.split_once(" - ")
            && before.split_whitespace().nth(4) == path.to_str()
        {
            let mut fields = after.split_whitespace();
            let actual = fields.next();
            if actual != Some(filesystem) {
                return Err(
                    format!("{} is occupied by filesystem {actual:?}", path.display()).into(),
                );
            }
            return Ok(fields.next().map(str::to_owned));
        }
    }
    Ok(None)
}

fn mount(source: &str, path: &Path, filesystem: &str) -> Result<()> {
    let source = CString::new(source)?;
    let target = CString::new(path.as_os_str().as_bytes())?;
    let filesystem = CString::new(filesystem)?;
    // All three names are NUL-terminated and mount options are absent.
    if unsafe {
        libc::mount(
            source.as_ptr(),
            target.as_ptr(),
            filesystem.as_ptr(),
            0,
            std::ptr::null(),
        )
    } < 0
    {
        return Err(format!(
            "mounting {} at {}: {}",
            filesystem.to_string_lossy(),
            path.display(),
            io::Error::last_os_error()
        )
        .into());
    }
    Ok(())
}

fn absent_ok(result: io::Result<()>, path: &Path) -> Result<()> {
    match result {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(format!("removing {}: {error}", path.display()).into()),
    }
}

pub(super) struct Gadget {
    _lock: File,
    controller: String,
}

impl Gadget {
    pub(super) fn prepare() -> Result<Self> {
        // This check uses the effective identity for the privileged operations below.
        if unsafe { libc::geteuid() } != 0 {
            return Err("serve requires root for configfs, FunctionFS and uinput".into());
        }
        let controllers = controllers()?;
        let lock = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(false)
            .custom_flags(libc::O_NOFOLLOW)
            .open("/run/tab5-display-link.lock")?;
        // Keep the open file description locked across stale recovery and teardown.
        if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } < 0 {
            return Err(format!("locking Tab5 gadget: {}", io::Error::last_os_error()).into());
        }
        if mount_source(Path::new(CONFIG), "configfs")?.is_none() {
            mount("configfs", Path::new(CONFIG), "configfs")?;
        }
        if !Path::new(GADGETS).is_dir() {
            return Err("USB gadget configfs unavailable; run modprobe libcomposite".into());
        }
        let mut gadget = Self {
            _lock: lock,
            controller: String::new(),
        };
        gadget.cleanup()?;
        for controller in controllers {
            let path = Path::new(CONTROLLERS).join(&controller).join("function");
            let function = fs::read_to_string(&path)
                .map_err(|error| format!("reading UDC ownership at {}: {error}", path.display()))?;
            if function.trim().is_empty() {
                gadget.controller = controller;
                break;
            }
        }
        if gadget.controller.is_empty() {
            return Err(BUSY_CONTROLLER.into());
        }
        if let Err(error) = gadget.create() {
            return match gadget.cleanup() {
                Ok(()) => Err(error),
                Err(cleanup) => Err(format!("{error}; cleanup: {cleanup}").into()),
            };
        }
        Ok(gadget)
    }

    fn create(&self) -> Result<()> {
        fs::create_dir(Path::new(GADGET))
            .map_err(|error| format!("creating {}: {error}", Path::new(GADGET).display()))?;
        for directory in [
            "strings/0x409",
            "configs/c.1",
            "functions/ffs.tab5-display-link",
        ] {
            fs::create_dir(Path::new(GADGET).join(directory))
                .map_err(|error| format!("creating gadget directory {directory}: {error}"))?;
        }
        for &(attribute, value) in ATTRIBUTES {
            fs::write(Path::new(GADGET).join(attribute), value)
                .map_err(|error| format!("writing gadget attribute {attribute}: {error}"))?;
        }
        std::os::unix::fs::symlink(
            Path::new(GADGET).join("functions/ffs.tab5-display-link"),
            Path::new(GADGET).join("configs/c.1/ffs.tab5-display-link"),
        )
        .map_err(|error| format!("linking FunctionFS into the USB configuration: {error}"))?;
        fs::create_dir(Path::new(MOUNT))
            .map_err(|error| format!("creating {}: {error}", Path::new(MOUNT).display()))?;
        mount(FUNCTION, Path::new(MOUNT), "functionfs")
    }

    pub(super) fn bind(&self) -> Result<()> {
        fs::write(Path::new(GADGET).join("UDC"), &self.controller).map_err(|error| {
            if matches!(error.raw_os_error(), Some(libc::EBUSY | libc::EADDRINUSE)) {
                BUSY_CONTROLLER.into()
            } else {
                format!("binding UDC {}: {error}", self.controller).into()
            }
        })
    }

    pub(super) fn unbind(&self) -> Result<()> {
        let path = Path::new(GADGET).join("UDC");
        match fs::read_to_string(&path) {
            Ok(controller) if !controller.trim().is_empty() => fs::write(path, "\n")
                .map_err(|error| format!("unbinding the Tab5 UDC: {error}").into()),
            Ok(_) => Ok(()),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(format!("checking Tab5 UDC binding: {error}").into()),
        }
    }

    pub(super) fn cleanup(&mut self) -> Result<()> {
        let mounted = mount_source(Path::new(MOUNT), "functionfs")?;
        if mounted.as_deref().is_some_and(|source| source != FUNCTION) {
            return Err(format!(
                "{} is occupied by another FunctionFS instance",
                Path::new(MOUNT).display()
            )
            .into());
        }
        self.unbind()?;
        if mounted.is_some() {
            let target = CString::new(Path::new(MOUNT).as_os_str().as_bytes())?;
            // The dedicated FunctionFS mount was verified above.
            if unsafe { libc::umount(target.as_ptr()) } < 0 {
                return Err(format!(
                    "unmounting {}: {}",
                    Path::new(MOUNT).display(),
                    io::Error::last_os_error()
                )
                .into());
            }
        }
        absent_ok(fs::remove_dir(Path::new(MOUNT)), Path::new(MOUNT))?;
        let link = Path::new(GADGET).join("configs/c.1/ffs.tab5-display-link");
        absent_ok(fs::remove_file(&link), &link)?;
        for directory in [
            "functions/ffs.tab5-display-link",
            "configs/c.1",
            "strings/0x409",
            "",
        ] {
            let path = Path::new(GADGET).join(directory);
            absent_ok(fs::remove_dir(&path), &path)?;
        }
        Ok(())
    }
}

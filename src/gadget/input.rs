use crate::protocol::{Input, Keyboard, key_code};
use crate::{Result, native_result};
use std::fs::File;
use std::os::fd::{AsRawFd, FromRawFd};

unsafe extern "C" {
    fn tab5_input_open(pointer: libc::c_int, keys: *const u16, count: usize) -> libc::c_int;
    fn tab5_input_event(
        descriptor: libc::c_int,
        kind: u16,
        code: u16,
        value: libc::c_int,
    ) -> libc::c_int;
}

const KEY: u16 = 1;
const ABSOLUTE: u16 = 3;
const TOUCH: u16 = 0x14a;

pub(super) struct Devices {
    keyboard: File,
    touchscreen: File,
    held: Keyboard,
    pressed: bool,
}

impl Devices {
    pub(super) fn open() -> Result<Self> {
        let keys: Vec<_> = (0..=255).filter_map(key_code).collect();
        Ok(Self {
            keyboard: open(false, &keys)?,
            touchscreen: open(true, &[TOUCH])?,
            held: Keyboard::default(),
            pressed: false,
        })
    }

    pub(super) fn inject(&mut self, input: Input) -> Result<()> {
        match input {
            Input::Keyboard(report) => {
                let mut events = Vec::new();
                self.held
                    .update(report, |code, pressed| events.push((code, pressed)));
                if !events.is_empty() {
                    for (code, pressed) in events {
                        emit(&self.keyboard, KEY, code, i32::from(pressed))?;
                    }
                    emit(&self.keyboard, 0, 0, 0)?;
                }
            }
            Input::Pointer { x, y, pressed } => {
                if self.pressed != pressed {
                    emit(&self.touchscreen, KEY, TOUCH, i32::from(pressed))?;
                    self.pressed = pressed;
                }
                emit(&self.touchscreen, ABSOLUTE, 0, x.into())?;
                emit(&self.touchscreen, ABSOLUTE, 1, y.into())?;
                emit(&self.touchscreen, 0, 0, 0)?;
            }
        }
        Ok(())
    }

    pub(super) fn release(&mut self) -> Result<()> {
        let keyboard = self.inject(Input::Keyboard([0; 8]));
        let pointer = if self.pressed {
            emit(&self.touchscreen, KEY, TOUCH, 0).and_then(|()| emit(&self.touchscreen, 0, 0, 0))
        } else {
            Ok(())
        };
        self.pressed = false;
        match (keyboard, pointer) {
            (Err(keyboard), Err(pointer)) => Err(format!("{keyboard}; {pointer}").into()),
            (Err(error), Ok(())) | (Ok(()), Err(error)) => Err(error),
            _ => Ok(()),
        }
    }
}

fn open(pointer: bool, keys: &[u16]) -> Result<File> {
    // Native setup borrows the key slice and returns a fresh owned descriptor.
    let descriptor = native_result(
        unsafe { tab5_input_open(pointer.into(), keys.as_ptr(), keys.len()) },
        "creating /dev/uinput device (load uinput and check permissions)",
    )?;
    Ok(unsafe { File::from_raw_fd(descriptor) })
}

fn emit(device: &File, kind: u16, code: u16, value: i32) -> Result<()> {
    // The File keeps the descriptor alive during the native write.
    native_result(
        unsafe { tab5_input_event(device.as_raw_fd(), kind, code, value) },
        "injecting /dev/uinput event",
    )?;
    Ok(())
}

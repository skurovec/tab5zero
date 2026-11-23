# Changelog

## Unreleased

- On-screen menu on a long press: battery state, rotation by 90°, 180° and 270°, and power off.
- Battery voltage, level and current logged to the serial console.
- Fixed charging of a deeply discharged battery by driving the charger's QC select pin.

## 0.1.0 – 2026-11-23

First public release, a working proof of concept.

- Tab5 firmware: USB host for the display link, hardware JPEG decoding, touch and I2C keyboard input, 5 V on USB-A to power and charge the connected device.
- Support for ESP32-P4 chip revision v1.0, which current Tab5 units ship with.
- Fixed the USB host FIFO split so the 512-byte high-speed bulk OUT endpoint can be claimed.
- `tab5-host` for Linux: `doctor`, `serve` and `capture` commands, framebuffer and DRM capture, uinput keyboard and touchscreen.

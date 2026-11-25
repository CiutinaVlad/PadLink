# Changelog

## 1.2.0
- New: **Wii U (Cemu)** mode. Buttons, sticks, motion and the touchscreen act as the Wii U GamePad in Cemu (receiver: `--cemu`, DSU protocol, touch goes to Cemu's GamePad view).
- Home screen shows all modes as cards; Advanced tools moved into Settings.
- Motion code shared between the Switch 2 and Cemu modes.

## 1.1.0
- Real graphical interface: mode cards, big connection status, live view of buttons, sticks and tilt, touch support everywhere.
- Controller forwarding now runs on its own high-priority thread, so drawing the screen never adds latency.
- Clearer error screens (for example when another app is using USB).

## 1.0.0
- New on-screen menu and Settings screen. No config files to edit.
- Switch 2 / Switch: wired Pro Controller with motion. Auto-detects whether the controller has a motion sensor and falls back to the console's sensor (Split Pad Pro). Gyro drift calibration.
- New: PC Xbox controller mode (XInput, no driver needed on Windows/Linux).
- Generic USB gamepad mode, Wi-Fi mode with PC receiver.
- Lower latency: report at every USB poll.
- Autostart option, settings saved to `switch/padlink/settings.ini`.

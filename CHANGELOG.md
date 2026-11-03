# Changelog

All notable changes to ESP-TPMS. Versions match `FW_VERSION` in the sketch, which is shown in the settings menu, on the start screen and in the serial log.

## 0.1.0
- **WiFi settings page:** Menu → WiFi setup starts a hotspot (`ESP-TPMS` / `tpms1234`); http://192.168.4.1 shows live readings and every setting, plus reset pairing and factory reset. Click or 10 min idle switches WiFi off. Password can be set in a git-ignored `secrets.h`.
- Partition scheme **Minimal SPIFFS (1.9 MB app)** is now required (the firmware is 1.34 MB).
- Debug builds accept the serial commands `wifi` / `wifioff`.

## 0.0.22
- Buzzer support (GPIO 0): 2 short beeps for LOW / HIGH, 3 long beeps for DROPPING / HOT, repeated every 60 s while the number blinks; menu item Buzzer (Off / Alerts / Alerts+click) with test beep; start-up chirp.
- Sketch split into tabs (no functional change).
- Neutral default splash logo (TPMS warning symbol); a local, git-ignored `logo_custom.h` is used instead when present.
- MIT license, this changelog, GitHub Actions compile check.

## 0.0.21
- "Pressure unit" is the first menu item (MIN/MAX below it use that unit).

## 0.0.20
- An OK reading older than 2 minutes is shown in grey.
- `TPMS_DEBUG` flag: quiet serial log by default; readings, 30 s status and screen state only in debug builds.

## 0.0.19
- Watchdog: the board restarts by itself if the program hangs for 10 s.
- Reset reason in the serial log; watchdog / crash / low-voltage restarts are shown on the splash.

## 0.0.18
- Layout and Colors are independent settings (4 layouts × 4 palettes).

## 0.0.17
- Themes with different fonts and layouts: Classic, Minimal, Detail, Retro.

## 0.0.16
- Colour themes; a click on the paired main screen switches them.

## 0.0.15
- °C / °F option, 180° screen rotation, larger pressure font (FreeSansBold 24pt).
- All settings, including bar / psi, are set in the menu (no longer by a single click).

## 0.0.14
- Factory reset, configurable "No signal" time, high-temperature alarm ("HOT!").
- The menu scrolls.

## 0.0.13
- 10 s boot splash with logo (a click skips it).

## 0.0.12
- Sensors of other vehicles nearby no longer keep the screen on.

## 0.0.11
- MIN-MAX and battery voltage moved into each wheel's header.

## 0.0.10
- Rapid pressure-loss alert ("DROPPING!").
- MIN / MAX shown on the main screen.
- Sensor low-battery warning.

## 0.0.9
- Alert number blinks for 5 minutes, then stays solid and dimming resumes.

## 0.0.8
- No dimming while an alert number is blinking.

## 0.0.7
- Out-of-range pressure number blinks.

## 0.0.6
- Alerts wake the screen when they start, but no longer keep it on forever.

## 0.0.5
- "Dim level" option (1–25 %).

## 0.0.4
- Dim means 5 % brightness (still readable), not black.

## 0.0.3
- Button: only click and long press (double-click removed); hold 5 s in the menu = save & exit.

## 0.0.2
- Two-stage idle timer: "Dim after" and "Screen off"; new sensor data restarts it.

## 0.0.1
- First versioned build: "BR" and 0xFBB0 sensor decoding, one-button pairing (Front / Back), 1.8" ST7735 display, settings menu with per-wheel MIN / MAX, bar / psi, status colours, CPU at 80 MHz, BLE scan duty, brightness and screen-off settings.

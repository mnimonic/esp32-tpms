# ESP-TPMS – notes for Claude

Arduino sketch `ESP-TPMS.ino` (+ `logo.h`): BLE TPMS monitor for a motorcycle (Front/Back) on an ESP32-C3 SuperMini with a 1.8" ST7735 TFT and one button. Current firmware **v0.0.21** (`FW_VERSION`).

Other files: `README.md` (user docs, wiring – keep in sync when pins, buttons or settings change), `TODO.md` (backlog – tick items when done, update the "Current firmware" line), `tools/img2bitmap.py` (image → `logo.h`).

## How the user works with me
- After every change: **compile + upload with arduino-cli, then read the serial log to verify**. The sketch defaults to `TPMS_DEBUG 0` (quiet release log: banner, reset reason, pairing, settings, `!!` alerts only). To see readings / 30 s status / screen state, build with `--build-property compiler.cpp.extra_flags=-DTPMS_DEBUG=1 --build-path %TEMP%\esp-tpms-debug`, and **upload the release build again at the end** so the board is left in release mode, and **bump `FW_VERSION`** (patch number, e.g. 0.0.14 → 0.0.15). Report what was verified on the board vs. what still needs a look at the screen.
- The user tests on the real device and gives short feedback; small UI tweaks are iterated quickly.
- Keep `README.md`, `CLAUDE.md` and `TODO.md` updated with each feature.

## Build / upload / serial
- Bundled CLI: `C:\Users\georg\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`
- `arduino-cli compile --upload -p COM6 -b "esp32:esp32:esp32c3:CDCOnBoot=cdc" C:\Projects\ESP-TPMS`
- **CDCOnBoot=cdc is required** – without it Serial goes to the UART pins and the monitor is silent.
- Read serial from PowerShell with `System.IO.Ports.SerialPort` on COM6 (`DtrEnable = $true`). "Access denied" = Arduino IDE serial monitor is open; "does not exist" = board unplugged. COM3/COM4 are Bluetooth ports.
- When editing the .ino from PowerShell, write UTF-8 **without BOM** (`[IO.File]::WriteAllText(..., New UTF8Encoding($false))`).
- Libraries: esp32 core 3.3.x, NimBLE-Arduino **2.x API** (`NimBLEScanCallbacks`, `onResult(const NimBLEAdvertisedDevice*)`), Adafruit ST7735 + GFX.
- Pillow is installed in the user site-packages: run `tools/img2bitmap.py` **without** `python -I`.

## Hardware facts learned the hard way
- Sensors are **"BR" type** (name "BR", service 0x27a5, 7-byte mfr data: status, volts×10, °C, pressure BE `psi = (raw-145)/10`, checksum), NOT the ra6070 0xFBB0 type. Both decoders are kept.
- Bursts of ~5 identical packets ~10 ms apart; every ~3 s when awake, rarely when still. Dedup via `BURST_DEDUP_MS`.
- TFT has a green tab but needs **`INITR_BLACKTAB`** (GREENTAB gave a noise border).
- `Serial.setTxTimeoutMs(0)` is required: with USB connected and no monitor reading, prints blocked loop() for ~46 s.
- Pins: TFT SCK 6, MOSI 7, CS 10, DC 5, RST 3, BL 1 (PWM); button GPIO4→GND. SuperMini power LED removed by the user. Powered from an **ignition-switched** supply (runs only with ignition on) – ignition sense / deep sleep when parked are pointless, don't suggest them.
- Arduino prototype generation: enums/types used in function signatures must be declared at the top of the file, and **no function may be defined above the data types** (the generated prototypes go before the first function – defining helpers right after the includes broke the build in v0.0.19).

## Design decisions (user-confirmed)
- **Button – only click and long press, no double-click** (user found it unreliable).
  - Normal: click = store sensor while pairing, otherwise **cycle layout** (user's choice; every other setting incl. bar/psi is only in the menu); long press 1.5 s = open menu.
  - Menu: click = +step (0.1 bar / 1 psi, wraps) / change option / confirm; hold 1 s = next item; keep holding to 5 s = save & exit; 60 s idle = exit without saving.
  - Menu items (in order): Pressure unit, per-position MIN/MAX (MENU_LIM0 offset), Temp unit, Max temp, No signal, Scan, Brightness, Dim after, Dim level, Screen off, Rotate, Layout, Colors, RESET PAIRING (two clicks), FACTORY RESET (two clicks → prefs.clear() + restart), SAVE & EXIT. 19 items → list scrolls (MENU_VISIBLE 11, scrollbar). Version shown in the menu header.
  - A press while the screen is dimmed/off or during the splash only wakes/skips.
- **Colours / states**: green OK, red LOW (< MIN), blue HIGH (> MAX), red "DROPPING!" (rapid loss, highest priority), magenta "HOT!" (temp > Max temp, next priority), orange NO SIGNAL (No signal time), grey not paired/waiting, yellow "SENSOR FOUND" (unpaired candidate during pairing).
- **Section layout** (with a reading): header = name + MIN-MAX + battery voltage (yellow box if low); big number + unit; status text in state colour ("DROPPING! from X"); bottom = temp + age (yellow "LOW BAT" if low).
- **Alerts**: LOW/HIGH/DROP/HOT wake the screen when they start (edge only); the number blinks (BLINK_MS 500) for the first 5 min (ALERT_BLINK_MS) and the dim stage is skipped meanwhile; after that solid colour and dimming resumes. Screen off always applies.
- **Rapid loss**: per-position PressHist ring buffer (1 sample / 10 s, 32 deep); DROP when current ≤ max(last 5 min) − 0.2 bar.
- **Low battery**: BR < 2.7 V / FBB0 < 20 % – warning only (no blink/wake).
- **Power**: CPU 80 MHz; Scan duty 100/50/30/20 % of a 100 ms interval; Brightness PWM; two-stage idle timer: Dim after (never/10s/30s/1m) → Dim level 1–25 % (user wants "dim" to stay readable, not black), then Screen off (never/30s/1m/5m) → backlight 0 + display sleep. Idle restarts on button press, new data from **paired** sensors (unknown sensors only while a position is empty – foreign BR sensors nearby must not keep the screen on), alert start, menu open, unpaired candidate shown, and during the splash.
- **Boot splash**: `logo.h` (user's 450 MT logo, 120×43, white on black) + "TPMS" + version for SPLASH_MS = 10 s; BLE already scans meanwhile; a click skips. The logo is a CFMOTO trademark – fine on the device, flag it before any public GitHub upload.
- **Display**: pressure number in FreeSansBold24pt7b (baseline y+52), unit size-1 next to it, status line y+58, info line y+69. Temp unit C/F (`degf`), rotation 0/180 (`rot`, setRotation 0/2, previewed in menu). bar/psi (`psi`) is previewed live in the menu (MIN/MAX values switch unit) and restored if not saved.
- **Look = Layout × Colors, independent** (user wants every layout with every palette): LAYOUT_NAMES[] / layoutIdx (pref `layout`), PALETTES[] {name, bg, fg, dim, warn} / paletteIdx (pref `colors`); T() = palette, L() = layout. Layouts: LAY_CLASSIC (header bar with limits+battery, FreeSansBold24 number at x2/baseline 52, status y+58, info y+69), LAY_MINIMAL (colour stripe, dim name + status at top, centred FreeSansBold24 at baseline 50, info y+62, separator), LAY_DETAIL (header bar with status, FreeSansBold18 at baseline 42, Temp/Bat y+48, Min/Max y+58, RSSI+age y+68), LAY_RETRO (double frame in state colour, FreeMonoBold18 at x6/baseline 46, info y+64). Palettes: Dark, Light, Green LCD, Amber. numSpec() gives font/position per layout (used for blinking); drawInfoLine() is the per-layout line holding the age (refreshed every second). Pairing/waiting screens (drawNoData) are the same in every theme. Header bars, status colours, menu and message screens are shared. 
- **Persistence**: Preferences namespace `tpms`: `pos<i>`, `min<i>`, `max<i>` (bar), `psi`, `scan`, `bri`, `dim`, `diml`, `off`, `stale`, `tmax`, `degf`, `rot`, `layout`, `colors`.
- **Aged reading**: an OK reading older than AGED_MS (2 min) draws number + unit in the palette's dim colour (numberColor()); alerts keep their colour; updateDisplay redraws on the aged transition.
- **Watchdog**: task WDT on the loop task, WDT_TIMEOUT_MS = 10 s (esp_task_wdt_reconfigure/init + add(NULL), fed at the top of loop()). Reset reason logged at boot ("Last reset: ..."); watchdog / crash / brownout also shown in red on the splash. Verified with a `-DWDT_TEST` build (hangs after 20 s → `Last reset: watchdog`). Keep blocking pauses (message screens) well under 10 s.
- **Architecture**: BLE callback only decodes and pushes `Reading` into a FreeRTOS queue; all printing/drawing happens in `loop()`.

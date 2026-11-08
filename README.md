# ESP-TPMS

A BLE tyre-pressure monitor for two wheels (Front / Back) built on an **ESP32-C3 SuperMini** and a **1.8" ST7735 TFT** (128×160).
It listens to cheap BLE TPMS sensors, shows pressure, temperature and battery for each wheel, and colour-codes the result against MIN/MAX limits that you set with a single button.

Inspired by [ra6070/BLE-TPMS](https://github.com/ra6070/BLE-TPMS).

[![Build](https://github.com/mnimonic/esp32-tpms/actions/workflows/build.yml/badge.svg)](https://github.com/mnimonic/esp32-tpms/actions/workflows/build.yml) ![License: MIT](https://img.shields.io/badge/license-MIT-blue)

![status colours](https://img.shields.io/badge/in%20range-green-2ea043) ![below MIN](https://img.shields.io/badge/below%20MIN-red-d73a49) ![above MAX](https://img.shields.io/badge/above%20MAX-blue-3b82f6) ![no signal](https://img.shields.io/badge/no%20signal-orange-f59e0b)

## Features

- Passive BLE scanning, no pairing with the sensors needed
- Supports **"BR"** sensors (service `0x27a5`, 7-byte payload) and **`0xFBB0`** sensors (18-byte payload, as used by ra6070/BLE-TPMS)
- One-button pairing of sensors to **Front** and **Back**; the screen shows when an unpaired sensor is broadcasting
- Per-wheel **MIN / MAX** limits set from an on-screen menu and shown in each wheel's header together with the sensor battery voltage
- **Rapid pressure-loss alert** (puncture warning before MIN is reached)
- **Sensor low-battery warning**
- **bar / psi** and **°C / °F** display, large easy-to-read pressure font
- Screen can be rotated 180° for upside-down mounting
- 4 layouts × 4 colour schemes (Classic / Minimal / Detail / Retro × Dark / Light / Green LCD / Amber)
- Pairing, limits and unit are kept in flash and survive power loss
- Power options: CPU at 80 MHz, BLE scan duty cycle, backlight brightness and screen-off timer
- Boot splash with a logo for 3 s (a click skips it)
- **WiFi settings page** on your phone (the board becomes a hotspot on demand)
- Optional **buzzer** for alerts
- Watchdog: if the program ever hangs, the board restarts by itself within 10 s (the splash then shows "restarted: watchdog"; crashes and low-voltage resets are shown too)
- Serial log at 115200 baud for debugging

## Hardware

| Part | Notes |
|---|---|
| ESP32-C3 SuperMini | Any ESP32-C3 should work; pins below are for the SuperMini |
| 1.8" SPI TFT 128×160, ST7735 | Uses `INITR_BLACKTAB`, even though the film tab is green (see below) |
| Momentary push button | Normally open |
| 2× BLE TPMS sensors | "BR" type (external valve-cap sensors that work with the phone app) |

## Wiring

| TFT pin | ESP32-C3 SuperMini | Function |
|---|---|---|
| VCC | 3.3V | Power |
| GND | GND | Ground |
| SCL / SCK | GPIO 6 | SPI clock |
| SDA / MOSI | GPIO 7 | SPI data |
| CS | GPIO 10 | Chip select |
| DC / A0 / RS | GPIO 5 | Data / command |
| RES / RST | GPIO 3 | Reset |
| BL / LED | GPIO 1 | Backlight (PWM brightness / screen off). Can also go to 3.3V; then set `TFT_BL = -1` |

| SD slot on the TFT module (optional) | ESP32-C3 SuperMini | Function |
|---|---|---|
| SD_SCK | GPIO 6 | Shared with TFT SCK |
| SD_MOSI | GPIO 7 | Shared with TFT SDA/MOSI |
| SD_MISO | GPIO 20 | SPI data in (SD only) |
| SD_CS | GPIO 21 | SD chip select |

Some modules join SD_SCK/SD_MOSI to the TFT pins on the board; then only SD_MISO and SD_CS need wires. No card or no SD wiring: the firmware runs normally and does not log (set `SD_CS = -1` to skip the check).

| Button | ESP32-C3 SuperMini |
|---|---|
| Leg 1 | GPIO 4 |
| Leg 2 | GND |

No resistor is needed for the button because the internal pull-up is used.

| Buzzer (optional) | ESP32-C3 SuperMini |
|---|---|
| + (or module I/O) | GPIO 0 |
| − | GND |

An **active** buzzer is assumed. For a **passive** buzzer set `BUZZER_PASSIVE = true`, for a module that beeps when its input is LOW set `BUZZER_ACTIVE_LOW = true`, and with no buzzer set `BUZZER_PIN = -1` (top of `ESP-TPMS.ino`). A bare buzzer drawing more than ~20 mA needs a small NPN transistor; 3-pin buzzer modules already have one.

```
                 ESP32-C3 SuperMini
               ┌───────────────────┐
  TFT VCC ─────┤ 3.3V              │
  TFT BL  ─────┤ GPIO1             │
  TFT GND ─────┤ GND           GND ├───┐
               │                   │   │  push button
  TFT RES ─────┤ GPIO3       GPIO4 ├───┴──o/ o──┐
  TFT DC  ─────┤ GPIO5             │            │
  TFT SCL ─────┤ GPIO6             │           GND
  TFT SDA ─────┤ GPIO7             │
  TFT CS  ─────┤ GPIO10            │
  SD MISO ─────┤ GPIO20            │   (SD SCK/MOSI share GPIO6/7)
  SD CS   ─────┤ GPIO21            │
               │       USB-C       │  ← power 5 V / programming
               └───────────────────┘
```

Avoid GPIO 2, 8 and 9 (strapping pins; 8 is the onboard LED, 9 is the BOOT button) and GPIO 18/19 (USB). GPIO 20/21 (UART RX/TX) are free because Serial runs over USB.
If you move the TFT pins, change `TFT_SCK`, `TFT_MOSI`, `TFT_CS`, `TFT_DC`, `TFT_RST` and `TFT_BL` at the top of the sketch. The C3 can route SPI to almost any GPIO.

## Using it

### Button

| Where | Action | Result |
|---|---|---|
| Normal screen | Click | While pairing: store the detected sensor. When paired: next layout |
| Normal screen | Long press (1.5 s) | Open the **SETTINGS** menu |
| WiFi screen | Click | Switch WiFi off |
| Settings | Click | Increase the selected value by 0.1 bar or 1 psi (wraps around at the top), or step through the other options |
| Settings | Hold 1 s | Next item |
| Settings | Hold 5 s | **Save & exit** |
| Settings → RESET PAIRING | Click twice | Forget both sensors (first click asks to confirm, hold cancels) |
| Settings → FACTORY RESET | Click twice | Erase pairing, limits and all settings, then restart (hold cancels) |
| Settings → SAVE & EXIT | Click | Save all settings and return |
| Settings → Display > / SD card > | Click | Open the submenu (its last item "< Back" returns; hold 5 s still saves & exits) |
| SD card → Delete all logs | Click twice | Delete every ride log, then start a new one (hold cancels) |
| SD card → Format card | Click twice | Quick-format the whole card, then start a new log (hold cancels) |
| Boot splash | Click | Skip the 3 s splash |
| Screen dimmed / off | Click | Wake the screen only |

"WiFi setup" is the first menu item, so it is one click after opening the menu.

Menu structure:

```
SETTINGS
  WiFi setup        Start
  Pressure unit     bar/psi
  Front/Back MIN, MAX
  Temp unit, Max temp, No signal, Buzzer, Scan
  Display >         Brightness, Dim after, Dim level, Screen off, Rotate, Layout, Colors, < Back
  SD card >         card size / free space / current log, Delete all logs, Format card, < Back
  RESET PAIRING, FACTORY RESET, SAVE & EXIT
```

Opening "SD card >" also picks up a card that was inserted after start-up. The menu closes without saving after 60 s with no button press. The software version is shown in the menu header. The list scrolls (scrollbar on the right) to keep the selected item visible.

### Unit settings (in the menu)

| Item | Values | Effect |
|---|---|---|
| Pressure unit | bar / psi | Unit for pressure and MIN/MAX (the menu values switch immediately) |
| Temp unit | C / F | Unit for temperatures and Max temp |

### Display settings (menu → Display >)

| Item | Values | Effect |
|---|---|---|
| Rotate | 0 / 180 | Turn the picture upside down (previewed immediately, reverted if not saved) |
| Layout | Classic / Minimal / Detail / Retro | Fonts and positions on the main screen (see below) |
| Colors | Dark / Light / Green LCD / Amber | Colours of the main screen (any combination with any layout) |

### Alarm settings (in the menu)

| Item | Values | Effect |
|---|---|---|
| Front/Back MIN, MAX | 1.0 – 4.5 bar | Below MIN = red LOW, above MAX = blue HIGH |
| No signal | 5 / 15 / 30 / 60 min | A wheel with no data for this long turns orange "NO SIGNAL" (default 15 min) |
| Max temp | Off / 60 / 70 / 80 / 90 °C | Above it the wheel shows magenta "HOT!" (default 80 °C) |

### Power settings (Scan in the menu, the rest in menu → Display >)

| Item | Values | Effect |
|---|---|---|
| Scan | 100 / 50 / 30 / 20 % | Share of time the BLE receiver listens (100 ms interval). Lower uses less power but may miss a sensor burst |
| Brightness | 100 / 75 / 50 / 25 % | Backlight brightness (needs BL on GPIO 1) |
| Dim after | Never / 10 s / 30 s / 1 m | Stage 1: backlight drops to the dim level after this idle time (still readable). After power-on the screen stays on until every paired wheel has sent fresh data (max 5 min) |
| Dim level | 1 / 2 / 5 / 10 / 15 / 20 / 25 % | Brightness while dimmed. The screen previews it while this item is selected |
| Screen off | Never / 30 s / 1 m / 5 m | Stage 2: backlight off and the display controller goes to sleep |

"Idle" means no button press **and no new data from your paired sensors** (sensors on other vehicles nearby are ignored), so while riding (sensors report every few seconds) the screen stays on, and it dims and turns off when parked. A tyre going out of range wakes the screen; its pressure number then blinks for 5 minutes, and during that time the dim stage is skipped. After 5 minutes the number stays solid red/blue and dimming works again. "Screen off" always applies. A new sensor found while pairing keeps the screen on.

The CPU runs at 80 MHz (`CPU_MHZ`), the lowest speed that supports BLE.

### Layout and colours

The main screen has two independent settings, so any layout works with any colours (16 combinations). A **click on the main screen** (when both sensors are paired) switches the layout; both are in the menu. Status colours (green / red / blue / magenta / orange) mean the same everywhere.

| Layout | Look |
|---|---|
| **Classic** | Coloured header bar with name, MIN-MAX and battery; large bold number; status and temperature lines |
| **Minimal** | No header bar: small name and status, colour stripe on the left, large centred number, one info line |
| **Detail** | Header bar with status, medium number, plus temperature, battery, MIN/MAX, signal strength (RSSI) and age |
| **Retro** | Monospaced "LCD" font, each wheel in a frame that takes the status colour |

| Colors | Look |
|---|---|
| **Dark** | White on black |
| **Light** | Black on white – best in sunlight |
| **Green LCD** | Green on black |
| **Amber** | Amber on black – low glare at night |

### WiFi settings page

Change every setting from your phone instead of the button:

1. **Menu → WiFi setup → click.** Changes made in the menu are saved, then the board starts a WiFi hotspot and the screen shows how to connect.
2. On the phone, join the WiFi network **`ESP-TPMS`** (password **`tpms1234`**). The page usually opens by itself ("sign in to network"); otherwise open **http://192.168.4.1**. If the phone warns "no internet", choose to stay connected.
3. The page shows live cards for both wheels (updated every 2 s) and all settings: units, MIN/MAX, alarms, buzzer, layout, colours, brightness, dim, screen-off and BLE scan. Tap **Save settings**. Reset pairing and factory reset are there too (with confirmation).
4. **Click the button** to switch WiFi off. It also switches off by itself after 10 minutes without the page being used. BLE keeps receiving the whole time.

To use your own password, create `secrets.h` next to the sketch (it is git-ignored):

```cpp
#define WIFI_AP_PASS "my-password"   // at least 8 characters
// #define WIFI_AP_SSID "My-TPMS"    // optional
```

### SD card log (optional)

With a microSD card (FAT32) in the TFT module's slot, every ignition-on is logged to its own file **`/logs/rideNNNN.csv`** (numbered upwards; the splash shows "SD log: ride N" or "no SD card").

| Column | Meaning |
|---|---|
| `t_s` | Seconds since power-on (the board has no clock) |
| `pos` | Front / Back (empty for board events) |
| `event` | `data` (one per wheel every 10 s, and when the sensor's status byte changes), `state` (OK / LOW / HIGH / DROPPING! / HOT! / NO SIGNAL / …), `lowbat`, `boot`, `sensor` (paired address and limits) |
| `bar`, `temp_c`, `volts`, `batt_pct`, `status`, `rssi` | Last reading of that wheel |
| `state`, `note` | State at that moment; extra info (firmware + reset reason, "from X bar", "status changed") |

Rows are flushed to the card every 10 s and on every state change, so turning the ignition off loses at most the last 10 s. About 45 kB per hour. When less than 16 MB is free the oldest rides are deleted.
**Download:** the WiFi page lists all rides under *Ride logs* – tap one to save the CSV on the phone. Or take the card out and open the files on a PC.

### Buzzer (optional)

| Event | Sound |
|---|---|
| Wheel goes LOW or HIGH | 2 short beeps |
| DROPPING! or HOT! | 3 long beeps |
| While the alert number blinks (first 5 min) | repeated every 60 s |
| Start-up | one short chirp (confirms the buzzer works) |

**Menu → Buzzer:** Off / Alerts / Alert+clk (also a short tick on every button press). Selecting it plays a test beep.

### Pairing sensors

1. Open the menu (long press), go to **RESET PAIRING** and click twice. Skip this on a new board.
2. Wake the **Front** sensor by screwing it on or applying pressure. The Front section turns yellow with **SENSOR FOUND**.
3. Click to store it.
4. Repeat for the **Back** sensor.

Wake only one sensor at a time so the right one is stored.

### Screen colours

| Colour | Meaning |
|---|---|
| Green | Pressure between MIN and MAX |
| Red | Below MIN (number blinks) |
| Blue | Above MAX (number blinks) |
| Red "DROPPING!" | Rapid pressure loss: 0.2 bar or more below the highest reading of the last 5 min, even if still above MIN (number blinks) |
| Magenta "HOT!" | Tyre temperature above "Max temp" (number blinks) |
| Yellow box around the voltage + "LOW BAT" | Sensor battery below 2.7 V (or 20 %) |
| Orange | No signal for the "No signal" time (default 15 minutes) |
| Grey | Not paired / waiting for the first reading. A grey **number** means the last reading is older than 2 minutes (sensor quiet). After power-on the value from the **last ride** is shown grey with "last ride" (or "old") until the sensor sends again |
| Yellow | Unpaired sensor found, click to store |

The sensors sleep when they are not moving and only broadcast occasionally, so a reading that is a few minutes old is normal when parked.

## Building

Clone into a folder named **`ESP-TPMS`** – Arduino requires the folder name to match `ESP-TPMS.ino`:

```sh
git clone https://github.com/mnimonic/esp32-tpms.git ESP-TPMS
```

### Arduino IDE

1. **File → Preferences → Additional boards manager URLs:**
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Boards Manager:** install **esp32 by Espressif Systems** (tested with 3.3.x).
3. **Library Manager:** install
   - **NimBLE-Arduino** by h2zero (**2.x**; 1.x has a different API)
   - **Adafruit ST7735 and ST7789 Library**
   - **Adafruit GFX Library**
4. **Tools → Board → ESP32C3 Dev Module**
5. **Tools → USB CDC On Boot → Enabled.** Without this the serial monitor stays empty on the SuperMini.
6. **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA).** The firmware (with WiFi) is larger than the default 1.2 MB app partition.
7. Upload, then open the serial monitor at 115200 baud.

### arduino-cli

```sh
arduino-cli compile --upload -p COM6 -b "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" .
```

### Display variant

If the screen shows wrong colours or a band of random pixels along an edge, change this line:

```cpp
#define TFT_TAB INITR_BLACKTAB   // or INITR_GREENTAB / INITR_REDTAB
```

The module used here has a **green** tab but needs `INITR_BLACKTAB`.

## Code structure

| File | Contents |
|---|---|
| `ESP-TPMS.ino` | Pins, settings, data types and global variables |
| `10_ble.ino` | Sensor decoders and BLE scanning |
| `20_storage.ino` | Pairing and settings in flash |
| `30_alerts.ino` | Rapid pressure loss, low battery |
| `35_buzzer.ino` | Buzzer patterns |
| `40_pairing.ino` | Storing sensors with the button |
| `50_menu.ino` | Settings menu and button handling |
| `60_readings.ino` | Processing incoming readings |
| `65_sdlog.ino` | SD card ride log |
| `70_display.ino` | Main screen layouts, menu drawing, splash |
| `80_screen.ino` | Backlight, dim and screen-off |
| `85_wifi.ino` + `web_page.h` | WiFi hotspot and settings page |
| `90_system.ino` | Watchdog and reset reason |
| `99_setup_loop.ino` | `setup()` and `loop()` |

Arduino compiles `ESP-TPMS.ino` first and then the other tabs in alphabetical order, hence the numbers.

## Sensor protocols

### "BR" sensors (name `BR`, service `0x27a5`)

7 bytes of manufacturer data:

| Byte | Meaning |
|---|---|
| 0 | Status flags |
| 1 | Battery voltage × 10 (e.g. `0x20` = 3.2 V) |
| 2 | Temperature °C |
| 3–4 | Pressure, big-endian: `psi = (raw − 145) / 10` (raw is absolute pressure in 0.1 psi) |
| 5–6 | Checksum |

Example: `00 20 1E 00 92 96 23` → 3.2 V, 30 °C, 0.1 psi (sensor not on a tyre).
Format reference: [andi38/TPMS](https://github.com/andi38/TPMS).

### `0xFBB0` sensors

18 bytes of manufacturer data, starting `00 01`: pressure in Pa (bytes 8–11, little-endian), temperature in 0.01 °C (12–15), battery % (16), alarm flag (17).
Format reference: [ra6070/BLE-TPMS](https://github.com/ra6070/BLE-TPMS).

## Configuration

These constants are at the top of `ESP-TPMS.ino`:

| Constant | Default | Meaning |
|---|---|---|
| `POSITION_NAMES` | `{"Front", "Back"}` | Wheel positions, in pairing order |
| `DEFAULT_MIN_BAR` / `DEFAULT_MAX_BAR` | 2.0 / 3.0 | Limits on a new board |
| `LIMIT_RANGE_MIN` / `LIMIT_RANGE_MAX` | 1.0 / 4.5 | Range that can be selected in the menu |
| `STORE_WINDOW_MS` | 60 s | How recently a sensor must have been heard to be stored |
| `TPMS_DEBUG` | `0` | `1` = verbose serial log (every reading, 30 s status, screen dim/off). Can also be set at build time: `--build-property compiler.cpp.extra_flags=-DTPMS_DEBUG=1` |
| `AGED_MS` | 2 min | An OK reading older than this is shown in grey |
| `PRINT_RAW` | `false` | Also print raw payload bytes to serial (debug builds) |

## Boot logo

The 3 s splash shows a 1-bit logo drawn white on black. The repository ships a neutral TPMS symbol (`logo.h`, made by `tools/make_default_logo.py`). To show your own logo, create `logo_custom.h`:

```sh
pip install pillow
python tools/img2bitmap.py my-logo.png logo_custom.h 120 70   # fits the logo into 120 x 70 px
```

Dark pixels in the image become the white logo on screen. `logo_custom.h` is git-ignored, so a personal or trademarked logo never ends up in the repository; the sketch uses it automatically when it exists. Change `SPLASH_MS` in the sketch for a different duration.

## License

MIT – see [LICENSE](LICENSE). Changes are listed in [CHANGELOG.md](CHANGELOG.md).

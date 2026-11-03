# ESP-TPMS – To do

Current firmware: **v0.0.21**. Items are grouped by area and roughly ordered by value within each group.

## 1. Verify before riding
- [ ] **Check pressure readings against a gauge or the phone app** with sensors mounted. The `(raw − 145) / 10` psi formula comes from one source and has only been checked at about 0 psi.
- [ ] **Check that scan 20 % catches every burst** while riding. Readings should arrive about every 3 s. If gaps are much longer, raise Scan to 30 or 50 %.
- [ ] **Check negative temperatures** in cold weather. They are decoded as a signed byte, which is an assumption.
- [ ] **Measure the actual current draw** with a USB power meter at each setting (scan %, brightness, dim, off), to see what the power options save.

## 2. Safety features
- [x] **Rapid pressure loss alert:** warn when pressure drops faster than, for example, 0.2 bar in 5 minutes, even while still above MIN. This catches a puncture early.
- [x] **Sensor low-battery warning:** show a battery icon or orange text when voltage is under about 2.7 V. The CR1632 cell lasts about 1–2 years.
- [x] **High temperature alarm:** a configurable MAX temperature, for example 80 °C.
- [ ] **Buzzer or LED output** on a free GPIO (GPIO 0, 2 or 21) for alerts, so you don't need to watch the screen.
- [ ] **Decode the status byte.** Values seen so far are `0x00`, `0x28`, `0x40` and `0x80`. They probably mean motion, low battery or leak; log them alongside events to find out.
- [x] **Configurable "NO SIGNAL" time** (currently fixed at 15 minutes).

## 3. Display and usability
- [x] **Show MIN/MAX on the main screen** in small text under each wheel.
- [x] **°C / °F option** in the menu.
- [x] **Screen rotation option** (0° / 180°) for mounting the display upside down.
- [x] **Larger font** for the pressure number (Adafruit GFX FreeFonts) so it is easier to read while riding.
- [ ] **Auto brightness** with an LDR on an ADC pin (GPIO 0 or 1 are ADC-capable), so the screen is bright in sun and dim at night.
- [x] **Show the time since the last reading more clearly** when a sensor is quiet, for example by greying out the number.
- [x] **Factory reset** menu item that clears pairing, limits and all settings.

## 4. Power
The device is powered from an **ignition-switched** supply, so it only runs while the ignition is on. Ignition sense and deep sleep when parked are therefore not needed (dropped).
- [ ] **12 V → 5 V supply:** a proper buck converter on the switched circuit, with a fuse and protection against load dumps.
- [ ] **Automatic light sleep** between scan windows (optional, low priority with switched power). This needs ESP-IDF power-management settings, which the stock Arduino core does not enable.

## 5. Connectivity (optional)
- [ ] **WiFi page for the phone:** settings and a live view, as an easier way to set limits than the button.
- [ ] **OTA firmware updates** over WiFi, so the board can stay mounted.
- [ ] **Data log:** keep min/max pressure and temperature per ride, viewable in the menu or on the WiFi page.

## 6. Hardware and build
- [ ] **Enclosure:** waterproof (IP65), vibration-proof mounting, sunlight-readable screen (or an anti-glare film).
- [ ] **Soldered build** instead of jumper wires, either perfboard or a small PCB.
- [ ] **Waterproof button** (sealed tactile switch, IP67).
- [ ] **Antenna check:** SuperMini boards have a weak chip antenna. Check RSSI from both wheels when mounted (the rear is usually the hardest).

## 7. Code and project
- [x] **git init**, first commit, push to GitHub: https://github.com/mnimonic/esp32-tpms (public). Still open: remove the personal path from `CLAUDE.md`, decide about the CFMOTO logo in the public repo, delete the duplicate `mnimonic/ESP-TPMS` repo.
- [x] **Add a LICENSE** (MIT). Still open: **photos and a screenshot** in the README.
- [x] **GitHub Actions:** compile the sketch on every push with arduino-cli.
- [ ] **Split the sketch** into several files (`decoders.h`, `display.h`, `menu.h`, `button.h`). It is now about 1000 lines.
- [ ] **Unit tests for the decoders** using the captured payloads (e.g. `40 1D 1D 00 92 15 A9`).
- [x] **Watchdog timer**, so the board restarts if it ever hangs.
- [x] **Remove or hide debug output** (raw dumps, "slowest loop") behind a `DEBUG` flag for release builds.
- [x] **Version history:** a `CHANGELOG.md`, and tags in git for each `FW_VERSION`.

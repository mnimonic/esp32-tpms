/*
 * ESP-TPMS - BLE Tyre Pressure Monitor for ESP32-C3
 *
 * - Scans for BLE TPMS sensors ("BR" 7-byte type and 0xFBB0 18-byte type)
 * - Button on GPIO4 (everything is saved in flash and survives reboot):
 *     normal screen:
 *       click          : while pairing - store last detected sensor (Front, then Back)
 *                        when paired   - switch bar <-> psi
 *       long press 1.5s: open SETTINGS menu
 *     settings menu:
 *       click          : increase value / RESET PAIRING (click twice) / SAVE & EXIT
 *       hold 1 s       : next item
 *       hold 5 s       : save & exit
 * - Per-position MIN/MAX: green = in range, red = below MIN, blue = above MAX.
 *
 * Wiring:  push button between GPIO4 and GND (internal pull-up is used)
 *
 * Library: NimBLE-Arduino 2.x (Library Manager -> "NimBLE-Arduino" by h2zero)
 * Board:   ESP32C3 Dev Module, Tools -> USB CDC On Boot -> Enabled
 *
 * "BR" sensor payload (name "BR", service 0x27a5, 7-byte manufacturer data):
 *   [0] status  [1] battery 0.1 V  [2] temp degC  [3-4] pressure BE: psi=(raw-145)/10  [5-6] checksum
 * 0xFBB0 sensor payload (18-byte manufacturer data, per ra6070/BLE-TPMS):
 *   [8-11] pressure Pa LE  [12-15] temp 0.01 degC LE  [16] battery %  [17] alarm
 */

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

// ---------------- Watchdog ----------------
// If loop() stops running for this long (code hang), the board restarts by itself.
static const uint32_t WDT_TIMEOUT_MS = 10000;

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Fonts/FreeSansBold24pt7b.h>   // large pressure number (Classic, Minimal)
#include <Fonts/FreeSansBold18pt7b.h>   // Detail layout
#include <Fonts/FreeMonoBold18pt7b.h>   // Retro layout
// Boot splash bitmap (LOGO_W x LOGO_H, 1 bit per pixel). A local, git-ignored
// logo_custom.h (your own logo) takes priority over the neutral default logo.h.
#if __has_include("logo_custom.h")
#include "logo_custom.h"
#else
#include "logo.h"
#endif

static const uint32_t SPLASH_MS = 10000;   // boot splash duration (a click skips it)
static bool     splashActive = false;
static uint32_t splashStart  = 0;

// ---------------- Version ----------------
#define FW_VERSION "0.0.21"     // shown in the settings menu, on the start screen and in the serial log

// Verbose serial output (every reading, 30 s status, screen dim/off). 0 = release.
// Can also be set when building: --build-property compiler.cpp.extra_flags=-DTPMS_DEBUG=1
#ifndef TPMS_DEBUG
#define TPMS_DEBUG 0
#endif

// ---------------- Display (1.8" ST7735 128x160) ----------------
// Wiring (ESP32-C3 SuperMini):  VCC->3.3V  GND->GND  SCL->6  SDA->7
//                               CS->10  DC/A0->5  RES->3  BL/LED->1
static const int TFT_SCK  = 6;
static const int TFT_MOSI = 7;
static const int TFT_CS   = 10;
static const int TFT_DC   = 5;
static const int TFT_RST  = 3;
static const int TFT_BL   = 1;    // backlight (PWM). Set to -1 if BL is wired to 3.3V
// Screen variant (tab colour on the protective film):
//   INITR_BLACKTAB, INITR_GREENTAB, INITR_REDTAB
// Red/blue swapped or noise along an edge -> try another one.
#define TFT_TAB INITR_BLACKTAB

// Pressure limits (bar). Per-position MIN/MAX are set in the on-screen menu
// (long-press the button) and saved in flash; these are only the defaults
// and the allowed range.
static const float    DEFAULT_MIN_BAR = 2.0f;
static const float    DEFAULT_MAX_BAR = 3.0f;
static const float    LIMIT_RANGE_MIN = 1.0f;   // lowest value selectable in the menu
static const float    LIMIT_RANGE_MAX = 4.5f;   // highest value selectable in the menu
static const uint32_t AGED_MS        = 2UL * 60UL * 1000UL; // OK number turns grey when older than this
static const uint32_t BLINK_MS       = 500;                 // out-of-range number blinks with this half-period
static const uint32_t ALERT_BLINK_MS = 5UL * 60UL * 1000UL; // ...for this long after the alert starts

// Rapid pressure loss: alert when pressure is RAPID_DROP_BAR below the highest
// reading of the last RAPID_WINDOW_MS (catches a puncture before MIN is reached)
static const float    RAPID_DROP_BAR  = 0.2f;
static const uint32_t RAPID_WINDOW_MS = 5UL * 60UL * 1000UL;
static const uint32_t HIST_EVERY_MS   = 10000;              // store one history sample per 10 s
static const int      HIST_N          = 32;                 // 32 x 10 s > 5 min

// Sensor battery warning
static const float    LOW_BATT_V      = 2.7f;   // "BR" sensors report volts
static const int      LOW_BATT_PCT    = 20;     // 0xFBB0 sensors report percent
// "NO SIGNAL" after this many minutes without data (menu: No signal)
static const int      NUM_STALE                 = 4;
static const uint8_t  STALE_MIN[NUM_STALE]      = {5, 15, 30, 60};
static const int      DEFAULT_STALE_IDX         = 1;      // 15 min
// High temperature alarm in degC, 0 = off (menu: Max temp)
static const int      NUM_TMAX                  = 5;
static const uint8_t  TMAX_C[NUM_TMAX]          = {0, 60, 70, 80, 90};
static const int      DEFAULT_TMAX_IDX          = 3;      // 80 C

// ---------------- Settings ----------------
static const int      BUTTON_PIN       = 4;
static const uint32_t MENU_OPEN_MS     = 1500;   // normal screen: long press = open menu
static const uint32_t MENU_HOLD_MS     = 1000;   // in the menu: hold = next item
static const uint32_t MENU_SAVE_MS     = 5000;   // in the menu: keep holding = save & exit
static const uint32_t MENU_TIMEOUT_MS  = 60000;  // menu closes without saving after this idle time
static const uint32_t STORE_WINDOW_MS  = 60000;  // last sensor must be seen within this time to be stored
static const uint32_t BURST_DEDUP_MS   = 2000;   // identical packets within this time = same burst
static const uint32_t STATUS_EVERY_MS  = 30000;  // periodic summary
static const bool     PRINT_RAW        = false;  // also print raw payload bytes
static const uint32_t CPU_MHZ          = 80;     // 80 MHz is the minimum for BLE; saves power vs 160

// Power options, selectable in the settings menu (index saved in flash)
static const int      NUM_OPTS               = 4;
static const uint8_t  SCAN_DUTY[NUM_OPTS]    = {100, 50, 30, 20};  // % of time the BLE receiver listens
static const uint8_t  BRIGHT_PCT[NUM_OPTS]   = {100, 75, 50, 25};  // backlight brightness
static const uint16_t DIM_S[NUM_OPTS]        = {0, 10, 30, 60};    // stage 1: backlight to dim level after idle (0 = never)
static const int      NUM_DIM_LVLS           = 7;
static const uint8_t  DIM_LVL_PCT[NUM_DIM_LVLS] = {1, 2, 5, 10, 15, 20, 25};  // dimmed brightness choices
static const int      DEFAULT_DIM_LVL_IDX    = 2;                  // 5 %
static const uint16_t SCREEN_OFF_S[NUM_OPTS] = {0, 30, 60, 300};   // stage 2: display sleep after idle (0 = never)
static const int      DEFAULT_SCAN_IDX = 1, DEFAULT_BRIGHT_IDX = 0, DEFAULT_DIM_IDX = 0, DEFAULT_OFF_IDX = 0;
// "Idle" = no button press, no alert and no new sensor data.

// Positions, in the order the button fills them. Add more if needed
// (e.g. {"Front Left", "Front Right", "Rear Left", "Rear Right"}).
static const char* POSITION_NAMES[] = {"Front", "Back"};
static const int   NUM_POSITIONS    = sizeof(POSITION_NAMES) / sizeof(POSITION_NAMES[0]);

static const NimBLEUUID FBB0_SERVICE_UUID("fbb0");
static const NimBLEUUID BR_SERVICE_UUID("27a5");

// ---------------- Data types ----------------
// Main-screen look = Layout (fonts + positions) x Colors (palette); any combination works.
// Status colours (green/red/blue/magenta/orange) keep their meaning in every combination.
enum Layout { LAY_CLASSIC, LAY_MINIMAL, LAY_DETAIL, LAY_RETRO };
static const char* LAYOUT_NAMES[] = {"Classic", "Minimal", "Detail", "Retro"};
static const int   NUM_LAYOUTS    = sizeof(LAYOUT_NAMES) / sizeof(LAYOUT_NAMES[0]);

struct Palette { const char* name; uint16_t bg, fg, dim, warn; };
static const Palette PALETTES[] = {
  //  name        bg      fg      dim     warn
  {"Dark",      0x0000, 0xFFFF, 0x7BEF, 0xFFE0},   // white on black
  {"Light",     0xFFFF, 0x0000, 0x6B4D, 0xC300},   // black on white (sunlight)
  {"Green LCD", 0x0000, 0x07E0, 0x03E0, 0xFFE0},   // green on black
  {"Amber",     0x0000, 0xFD20, 0x8200, 0xFFE0},   // amber on black, low glare at night
};
static const int NUM_PALETTES = sizeof(PALETTES) / sizeof(PALETTES[0]);

// Where/how the big pressure number is drawn in each layout
struct NumSpec { const GFXfont* font; int baseline; int x; bool center; };

enum PosState { PS_EMPTY, PS_WAIT, PS_OK, PS_LOW, PS_HIGH, PS_DROP, PS_HOT, PS_OLD };
enum ScreenState { SCR_ON, SCR_DIM, SCR_OFF };   // DIM = low backlight, OFF = backlight 0 + display sleep

struct Reading {
  char     addr[18];      // "4a:21:00:00:52:2b"
  uint8_t  raw[18];
  uint8_t  rawLen;
  char     type[5];       // "BR" or "FBB0"
  float    psi;
  float    tempC;
  float    volts;         // -1 if unknown
  int      batteryPct;    // -1 if unknown
  uint8_t  status;
  int      rssi;
  uint32_t ms;            // millis() when received
};

struct Position {
  String  addr;           // empty = not assigned
  Reading last;
  bool    hasReading;
  float   minBar;         // below = red
  float   maxBar;         // above = blue
};

static Position     positions[NUM_POSITIONS];
static bool         usePsi = false;   // display unit: false = bar, true = psi

// Pressure history per position, for the rapid-loss alert
struct PressHist {
  float    bar[HIST_N];
  uint32_t ms[HIST_N];
  int      count, head;
  bool     dropping;      // rapid-loss alert active
  float    dropFrom;      // highest pressure in the window when the alert started
};
static PressHist hist[NUM_POSITIONS];
static bool      lowBattWarned[NUM_POSITIONS] = {false};
static int          scanIdx = DEFAULT_SCAN_IDX, brightIdx = DEFAULT_BRIGHT_IDX,
                    dimIdx = DEFAULT_DIM_IDX, dimLvlIdx = DEFAULT_DIM_LVL_IDX, screenOffIdx = DEFAULT_OFF_IDX,
                    staleIdx = DEFAULT_STALE_IDX, tmaxIdx = DEFAULT_TMAX_IDX;
static bool         useFahrenheit = false;   // temperature display unit
static bool         rotate180     = false;   // screen mounted upside down
static int          layoutIdx     = 0;       // main-screen layout (LAYOUT_NAMES)
static int          paletteIdx    = 0;       // main-screen colours (PALETTES)
static ScreenState  scrState = SCR_ON;
static uint32_t     lastActivity = 0;  // last button press / alert, for the screen-off timer
static volatile bool scanReconfig = false;
static Reading      lastDetected;
static bool         haveLastDetected = false;
static QueueHandle_t readingQueue;
static Preferences  prefs;
static volatile uint32_t advCount = 0;

// ---------------- Decoders ----------------
static uint32_t readU32LE(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool decodeBR(const uint8_t* d, size_t len, Reading& r) {
  if (len != 7) return false;
  uint16_t raw = ((uint16_t)d[3] << 8) | d[4];
  r.status     = d[0];
  r.volts      = d[1] / 10.0f;
  r.batteryPct = -1;
  r.tempC      = (int8_t)d[2];
  r.psi        = ((int)raw - 145) / 10.0f;
  if (r.psi < 0) r.psi = 0;
  strcpy(r.type, "BR");
  return true;
}

static bool decodeFBB0(const uint8_t* d, size_t len, Reading& r) {
  if (len != 18 || d[0] != 0x00 || d[1] != 0x01) return false;
  r.psi        = readU32LE(&d[8]) / 1000.0f * 0.1450377f;
  r.tempC      = (int32_t)readU32LE(&d[12]) / 100.0f;
  r.batteryPct = d[16];
  r.volts      = -1;
  r.status     = d[17];
  strcpy(r.type, "FBB0");
  return true;
}

// ---------------- BLE scan callback (runs in the BLE task) ----------------
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    advCount++;
    if (!dev->haveManufacturerData()) return;

    bool candidate = dev->isAdvertisingService(BR_SERVICE_UUID) ||
                     dev->isAdvertisingService(FBB0_SERVICE_UUID) ||
                     (dev->haveName() && dev->getName() == "BR");
    std::string md = dev->getManufacturerData();
    if (!candidate && md.size() != 18) return;

    Reading r = {};
    const uint8_t* d = (const uint8_t*)md.data();
    if (!decodeBR(d, md.size(), r) && !decodeFBB0(d, md.size(), r)) return;

    strncpy(r.addr, dev->getAddress().toString().c_str(), sizeof(r.addr) - 1);
    r.rawLen = min(md.size(), sizeof(r.raw));
    memcpy(r.raw, d, r.rawLen);
    r.rssi = dev->getRSSI();
    r.ms   = millis();
    xQueueSend(readingQueue, &r, 0);   // hand over to loop(); drop if queue is full
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    if (!scanReconfig) NimBLEDevice::getScan()->start(0, false, true);
  }
};
static ScanCallbacks scanCallbacks;

// (Re)start scanning with the selected duty cycle: listen <duty> ms out of every 100 ms
static void applyScanSettings() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scanReconfig = true;
  if (scan->isScanning()) scan->stop();
  scan->setInterval(100);
  scan->setWindow(SCAN_DUTY[scanIdx]);
  scan->start(0, false, true);
  scanReconfig = false;
  if (TPMS_DEBUG) Serial.printf(">> BLE scan: listening %u%% of the time\n", SCAN_DUTY[scanIdx]);
}

// ---------------- Positions (stored in flash) ----------------
static String prefKey(int i) { return "pos" + String(i); }

static void loadPositions() {
  prefs.begin("tpms", true);
  for (int i = 0; i < NUM_POSITIONS; i++) {
    positions[i].addr = prefs.getString(prefKey(i).c_str(), "");
    positions[i].hasReading = false;
    positions[i].minBar = prefs.getFloat(("min" + String(i)).c_str(), DEFAULT_MIN_BAR);
    positions[i].maxBar = prefs.getFloat(("max" + String(i)).c_str(), DEFAULT_MAX_BAR);
  }
  usePsi       = prefs.getBool("psi", false);
  scanIdx      = prefs.getUChar("scan", DEFAULT_SCAN_IDX)   % NUM_OPTS;
  brightIdx    = prefs.getUChar("bri",  DEFAULT_BRIGHT_IDX) % NUM_OPTS;
  dimIdx       = prefs.getUChar("dim",  DEFAULT_DIM_IDX)    % NUM_OPTS;
  dimLvlIdx    = prefs.getUChar("diml", DEFAULT_DIM_LVL_IDX) % NUM_DIM_LVLS;
  staleIdx     = prefs.getUChar("stale", DEFAULT_STALE_IDX) % NUM_STALE;
  tmaxIdx      = prefs.getUChar("tmax", DEFAULT_TMAX_IDX)   % NUM_TMAX;
  useFahrenheit = prefs.getBool("degf", false);
  rotate180     = prefs.getBool("rot", false);
  layoutIdx     = prefs.getUChar("layout", 0) % NUM_LAYOUTS;
  paletteIdx    = prefs.getUChar("colors", 0) % NUM_PALETTES;
  screenOffIdx = prefs.getUChar("off",  DEFAULT_OFF_IDX)    % NUM_OPTS;
  prefs.end();
}

// Saves MIN/MAX limits and the power options
static void saveLimits() {
  prefs.begin("tpms", false);
  for (int i = 0; i < NUM_POSITIONS; i++) {
    prefs.putFloat(("min" + String(i)).c_str(), positions[i].minBar);
    prefs.putFloat(("max" + String(i)).c_str(), positions[i].maxBar);
  }
  prefs.putBool("psi", usePsi);
  prefs.putUChar("scan", scanIdx);
  prefs.putUChar("bri",  brightIdx);
  prefs.putUChar("dim",  dimIdx);
  prefs.putUChar("diml", dimLvlIdx);
  prefs.putUChar("stale", staleIdx);
  prefs.putUChar("tmax", tmaxIdx);
  prefs.putBool("degf", useFahrenheit);
  prefs.putBool("rot", rotate180);
  prefs.putUChar("layout", layoutIdx);
  prefs.putUChar("colors", paletteIdx);
  prefs.putUChar("off",  screenOffIdx);
  prefs.end();
}

static void savePosition(int i) {
  prefs.begin("tpms", false);
  prefs.putString(prefKey(i).c_str(), positions[i].addr);
  prefs.end();
}

static int findPosition(const char* addr) {
  for (int i = 0; i < NUM_POSITIONS; i++)
    if (positions[i].addr.length() && positions[i].addr.equalsIgnoreCase(addr)) return i;
  return -1;
}

static void printPositions() {
  Serial.println("---- Stored sensors ----");
  for (int i = 0; i < NUM_POSITIONS; i++) {
    Serial.printf("  %-6s [%.2f-%.2f bar] : ", POSITION_NAMES[i], positions[i].minBar, positions[i].maxBar);
    if (!positions[i].addr.length()) { Serial.println("(empty)"); continue; }
    Serial.print(positions[i].addr);
    if (positions[i].hasReading) {
      const Reading& r = positions[i].last;
      Serial.printf("  %.2f bar / %.1f psi  %.0f C  (%lu s ago)\n",
                    r.psi * 0.0689476f, r.psi, r.tempC, (millis() - r.ms) / 1000);
    } else {
      Serial.println("  (no reading yet)");
    }
  }
  Serial.println("------------------------");
}

// ---------------- Rapid pressure loss / battery ----------------
static void resetHistory(int i) {
  memset(&hist[i], 0, sizeof(hist[i]));
  lowBattWarned[i] = false;
}

// Called for every new (non-repeat) reading of a paired sensor
static void addPressureSample(int i, float bar, uint32_t ms) {
  PressHist& h = hist[i];

  // Highest pressure within the window (including the current reading)
  float maxBar = bar;
  for (int k = 0; k < h.count; k++) {
    int idx = (h.head - 1 - k + HIST_N) % HIST_N;
    if (ms - h.ms[idx] > RAPID_WINDOW_MS) break;     // older samples are outside the window
    if (h.bar[idx] > maxBar) maxBar = h.bar[idx];
  }

  bool drop = (maxBar - bar) >= RAPID_DROP_BAR;
  if (drop && !h.dropping) {
    h.dropFrom = maxBar;
    Serial.printf("!! %s: RAPID PRESSURE LOSS %.2f -> %.2f bar within %lu min\n",
                  POSITION_NAMES[i], maxBar, bar, (unsigned long)(RAPID_WINDOW_MS / 60000));
  } else if (!drop && h.dropping) {
    Serial.printf(">> %s: pressure drop alert cleared (%.2f bar)\n", POSITION_NAMES[i], bar);
  }
  h.dropping = drop;

  // Store a history sample at most every HIST_EVERY_MS
  int last = (h.head - 1 + HIST_N) % HIST_N;
  if (h.count == 0 || ms - h.ms[last] >= HIST_EVERY_MS) {
    h.bar[h.head] = bar;
    h.ms[h.head]  = ms;
    h.head = (h.head + 1) % HIST_N;
    if (h.count < HIST_N) h.count++;
  }
}

static bool isLowBattery(const Reading& r) {
  if (r.volts >= 0)      return r.volts < LOW_BATT_V;
  if (r.batteryPct >= 0) return r.batteryPct < LOW_BATT_PCT;
  return false;
}

// ---------------- Button actions ----------------
static void storeLastDetected() {
  if (!haveLastDetected || millis() - lastDetected.ms > STORE_WINDOW_MS) {
    Serial.println(">> Button: no sensor detected in the last 60 s - wake a sensor first.");
    return;
  }
  int existing = findPosition(lastDetected.addr);
  if (existing >= 0) {
    Serial.printf(">> Button: %s is already stored as %s.\n",
                  lastDetected.addr, POSITION_NAMES[existing]);
    return;
  }
  for (int i = 0; i < NUM_POSITIONS; i++) {
    if (!positions[i].addr.length()) {
      positions[i].addr       = lastDetected.addr;
      positions[i].last       = lastDetected;
      positions[i].hasReading = true;
      savePosition(i);
      resetHistory(i);
      Serial.printf(">> Button: stored %s as %s.\n", lastDetected.addr, POSITION_NAMES[i]);
      if (i + 1 < NUM_POSITIONS)
        Serial.printf(">> Next: wake the %s sensor and press the button.\n", POSITION_NAMES[i + 1]);
      else
        Serial.println(">> All positions stored.");
      printPositions();
      return;
    }
  }
  Serial.println(">> Button: all positions are full. Use RESET PAIRING in the menu to clear.");
}

static bool displayDirty = false;      // set to force a full screen redraw

static bool allPositionsStored() {
  for (int i = 0; i < NUM_POSITIONS; i++)
    if (!positions[i].addr.length()) return false;
  return true;
}


// Click on the normal screen: store a sensor while pairing, otherwise next layout
static void onShortPress() {
  if (!allPositionsStored()) { storeLastDetected(); return; }
  layoutIdx = (layoutIdx + 1) % NUM_LAYOUTS;
  prefs.begin("tpms", false);
  prefs.putUChar("layout", layoutIdx);
  prefs.end();
  displayDirty = true;
  Serial.printf(">> Click: layout %s\n", LAYOUT_NAMES[layoutIdx]);
}
static void showResetScreen();

static void clearPositions() {
  for (int i = 0; i < NUM_POSITIONS; i++) {
    positions[i].addr = "";
    positions[i].hasReading = false;
    savePosition(i);
    resetHistory(i);
  }
  haveLastDetected = false;            // only sensors heard AFTER the reset can be stored
  showResetScreen();
  Serial.printf(">> Pairing reset: all positions cleared. Wake the %s sensor and press the button.\n",
                POSITION_NAMES[0]);
}

// ---------------- Settings menu (MIN/MAX per position) ----------------
// Items: Pressure unit, Front MIN, Front MAX, Back MIN, Back MAX, Temp unit, ..., Scan, Brightness, Screen off,
//        RESET PAIRING, SAVE & EXIT
static const int MENU_PUNIT  = 0;                        // bar / psi (first: MIN/MAX below use this unit)
static const int MENU_LIM0   = 1;                        // first MIN/MAX item: Front MIN, Front MAX, Back MIN, Back MAX
static const int MENU_TUNIT  = MENU_LIM0 + NUM_POSITIONS * 2;   // C / F
static const int MENU_TMAX   = MENU_TUNIT + 1;
static const int MENU_STALE  = MENU_TUNIT + 2;
static const int MENU_SCAN   = MENU_TUNIT + 3;
static const int MENU_BRIGHT = MENU_TUNIT + 4;
static const int MENU_DIM    = MENU_TUNIT + 5;
static const int MENU_DIMLVL = MENU_TUNIT + 6;
static const int MENU_OFF    = MENU_TUNIT + 7;
static const int MENU_ROT    = MENU_TUNIT + 8;
static const int MENU_LAYOUT = MENU_TUNIT + 9;
static const int MENU_COLORS = MENU_TUNIT + 10;
static const int MENU_RESET  = MENU_TUNIT + 11;
static const int MENU_FACTORY= MENU_TUNIT + 12;
static const int MENU_SAVE   = MENU_TUNIT + 13;
static const int MENU_ITEMS  = MENU_TUNIT + 14;
static const int MENU_VISIBLE = 11;    // rows that fit on screen; the list scrolls
static bool      menuActive = false;
static bool      resetArmed = false;   // RESET PAIRING / FACTORY RESET need a second click to confirm
static int       menuSel = 0;
static float     editMin[NUM_POSITIONS], editMax[NUM_POSITIONS];
static int       editScan, editBright, editDim, editDimLvl, editOff, editStale, editTmax;
static bool      editFahrenheit, editRotate;
static int       editLayout, editPalette;
static bool      savedPsi;            // unit before the menu opened (restored if not saved)
static uint32_t  menuLastInput = 0;

static void drawMenu();
static void showMessage(const char* line1, const char* line2, uint16_t bg);
static void setBacklightPct(int pct);
static void applyRotation(bool flip);
static void drawSplash();
static void setScreenState(ScreenState target);

// Values are edited in the unit shown on screen: 0.1 bar or 1 psi steps
static float unitStep()          { return usePsi ? 1.0f : 0.1f; }
static float toUnit(float bar)   { return usePsi ? bar * 14.5038f : bar; }
static float fromUnit(float u)   { return usePsi ? u / 14.5038f : u; }
static float snap(float u)       { return roundf(u / unitStep()) * unitStep(); }

static void openMenu() {
  for (int i = 0; i < NUM_POSITIONS; i++) {
    editMin[i] = positions[i].minBar;
    editMax[i] = positions[i].maxBar;
  }
  editScan   = scanIdx;
  editBright = brightIdx;
  editDim    = dimIdx;
  editDimLvl = dimLvlIdx;
  editOff    = screenOffIdx;
  editStale  = staleIdx;
  editTmax   = tmaxIdx;
  editFahrenheit = useFahrenheit;
  savedPsi   = usePsi;
  editRotate = rotate180;
  editLayout = layoutIdx;
  editPalette = paletteIdx;
  menuSel = 0;
  resetArmed = false;
  menuActive = true;
  menuLastInput = millis();
  Serial.println(">> Settings menu opened (click = +, hold 1 s = next item, hold 5 s = save & exit)");
  drawMenu();
}

static void closeMenu(bool save) {
  if (save) {
    for (int i = 0; i < NUM_POSITIONS; i++) {
      positions[i].minBar = editMin[i];
      positions[i].maxBar = editMax[i];
    }
    bool scanChanged = editScan != scanIdx;
    scanIdx      = editScan;
    brightIdx    = editBright;
    dimIdx       = editDim;
    dimLvlIdx    = editDimLvl;
    screenOffIdx = editOff;
    staleIdx     = editStale;
    tmaxIdx      = editTmax;
    useFahrenheit = editFahrenheit;
    rotate180    = editRotate;
    layoutIdx    = editLayout;
    paletteIdx   = editPalette;
    saveLimits();
    setBacklightPct(BRIGHT_PCT[brightIdx]);
    if (scanChanged) applyScanSettings();
    Serial.printf(">> Settings saved (scan %u%%, brightness %u%%, dim %us to %u%%, screen off %us)\n",
                  SCAN_DUTY[scanIdx], BRIGHT_PCT[brightIdx], DIM_S[dimIdx], DIM_LVL_PCT[dimLvlIdx],
                  SCREEN_OFF_S[screenOffIdx]);
    Serial.printf(">> No signal after %u min, max temp %u C (0 = off)\n", STALE_MIN[staleIdx], TMAX_C[tmaxIdx]);
    printPositions();
    showMessage("SAVED", "", ST77XX_GREEN);
  } else {
    Serial.println(">> Settings menu closed without saving");
    setBacklightPct(BRIGHT_PCT[brightIdx]);          // undo brightness preview
    applyRotation(rotate180);                        // undo rotation preview
    usePsi = savedPsi;                               // undo unit preview
    showMessage("NOT SAVED", "(timeout)", 0x7BEF);
  }
  lastActivity = millis();
  menuActive = false;
  displayDirty = true;
}

// Click on a value: increase by one step, wrap to the lowest allowed value at the top.
// MIN can go up to MAX - 1 step; MAX can go down to MIN + 1 step.
static void menuIncrement() {
  int  pos   = (menuSel - MENU_LIM0) / 2;
  bool isMax = (menuSel - MENU_LIM0) % 2;
  float step = unitStep();
  float lo, hi;
  if (!isMax) { lo = ceilf(toUnit(LIMIT_RANGE_MIN) / step) * step; hi = snap(toUnit(editMax[pos])) - step; }
  else        { lo = snap(toUnit(editMin[pos])) + step;              hi = floorf(toUnit(LIMIT_RANGE_MAX) / step) * step; }

  float cur = snap(toUnit(isMax ? editMax[pos] : editMin[pos]));
  float nv  = cur + step;
  if (nv > hi + step * 0.01f || nv < lo - step * 0.01f) nv = lo;
  if (isMax) editMax[pos] = fromUnit(nv);
  else       editMin[pos] = fromUnit(nv);
}

// Erase everything stored in flash and restart with defaults
static void factoryReset() {
  Serial.println(">> FACTORY RESET: erasing all settings and pairing, restarting...");
  prefs.begin("tpms", false);
  prefs.clear();
  prefs.end();
  setBacklightPct(100);
  showMessage("FACTORY", "reset - restarting", ST77XX_RED);
  ESP.restart();
}

static void menuClick() {
  menuLastInput = millis();
  if (menuSel == MENU_SAVE) { closeMenu(true); return; }
  if (menuSel == MENU_FACTORY) {
    if (!resetArmed) {                 // first click: ask for confirmation
      resetArmed = true;
      Serial.println(">> Factory reset? Click again to confirm, hold to cancel");
      drawMenu();
      return;
    }
    factoryReset();                    // does not return
  }
  if (menuSel == MENU_RESET) {
    if (!resetArmed) {                 // first click: ask for confirmation
      resetArmed = true;
      Serial.println(">> Reset pairing? Click again to confirm, hold to cancel");
      drawMenu();
      return;
    }
    resetArmed = false;                // second click: reset and leave the menu
    menuActive = false;                // (other edits in this session are not saved)
    setBacklightPct(BRIGHT_PCT[brightIdx]);
    applyRotation(rotate180);
    usePsi = savedPsi;
    lastActivity = millis();
    clearPositions();
    return;
  }
  if      (menuSel == MENU_SCAN)   editScan = (editScan + 1) % NUM_OPTS;
  else if (menuSel == MENU_BRIGHT) { editBright = (editBright + 1) % NUM_OPTS; setBacklightPct(BRIGHT_PCT[editBright]); }
  else if (menuSel == MENU_DIM)    editDim = (editDim + 1) % NUM_OPTS;
  else if (menuSel == MENU_DIMLVL) { editDimLvl = (editDimLvl + 1) % NUM_DIM_LVLS; setBacklightPct(DIM_LVL_PCT[editDimLvl]); }  // preview
  else if (menuSel == MENU_OFF)    editOff = (editOff + 1) % NUM_OPTS;
  else if (menuSel == MENU_STALE)  editStale = (editStale + 1) % NUM_STALE;
  else if (menuSel == MENU_TMAX)   editTmax = (editTmax + 1) % NUM_TMAX;
  else if (menuSel == MENU_TUNIT)  editFahrenheit = !editFahrenheit;
  else if (menuSel == MENU_PUNIT)  usePsi = !usePsi;   // preview: menu values switch unit
  else if (menuSel == MENU_LAYOUT) editLayout = (editLayout + 1) % NUM_LAYOUTS;
  else if (menuSel == MENU_COLORS) editPalette = (editPalette + 1) % NUM_PALETTES;
  else if (menuSel == MENU_ROT)    { editRotate = !editRotate; applyRotation(editRotate); }  // preview
  else                             menuIncrement();
  drawMenu();
}

static void menuNext() {
  menuLastInput = millis();
  resetArmed = false;
  menuSel = (menuSel + 1) % MENU_ITEMS;
  setBacklightPct(menuSel == MENU_DIMLVL ? DIM_LVL_PCT[editDimLvl]     // preview the dim level
                                         : BRIGHT_PCT[editBright]);
  drawMenu();
}


// Debounced button:
//   normal mode: click (unit / pairing) / long press 1.5 s (open menu)
//   menu mode:   click (+ / reset / save) / hold 1 s (next item)
static void handleButton() {
  static int      lastRead = HIGH, stable = HIGH;
  static uint32_t lastChange = 0, pressStart = 0;
  static bool     holdDone = false;
  static bool     menuHold = false;   // current press started inside the menu

  int now = digitalRead(BUTTON_PIN);
  if (now != lastRead) { lastRead = now; lastChange = millis(); }

  if (millis() - lastChange > 30 && now != stable) {
    stable = now;
    if (stable == LOW) {
      pressStart = millis();
      holdDone = false;
      menuHold = menuActive;
      lastActivity = millis();
      if (splashActive) {                             // boot splash: this press only skips it
        splashStart = millis() - SPLASH_MS;
        holdDone = true;
      } else if (scrState != SCR_ON) {                // dimmed / off: this press only wakes it
        setScreenState(SCR_ON);
        holdDone = true;
      }
    } else if (!holdDone) {                           // released before any hold action
      if (menuActive) menuClick();
      else            onShortPress();
    }
  }

  if (stable == LOW) {
    uint32_t held = millis() - pressStart;
    if (!holdDone && menuActive && held >= MENU_HOLD_MS) {          // menu: hold 1 s = next item
      holdDone = true;
      menuNext();
    } else if (!holdDone && !menuActive && held >= MENU_OPEN_MS) {  // normal: long press = open menu
      holdDone = true;
      openMenu();
    }
    // Menu: keep holding to 5 s = save & exit (only for a press that started in the menu)
    if (menuHold && menuActive && held >= MENU_SAVE_MS) {
      menuHold = false;
      Serial.println(">> Menu: long hold -> save & exit");
      closeMenu(true);
    }
  }

  if (menuActive && millis() - menuLastInput > MENU_TIMEOUT_MS) closeMenu(false);
}

// ---------------- Reading handling ----------------
static void printReading(const Reading& r, int pos) {
  Serial.printf("[%8lu ms] ", r.ms);
  if (pos >= 0) Serial.printf("%-6s ", POSITION_NAMES[pos]);
  else          Serial.print("NEW    ");
  Serial.printf("%s  %.2f bar / %.1f psi  %.0f C", r.addr, r.psi * 0.0689476f, r.psi, r.tempC);
  if (r.volts >= 0)      Serial.printf("  %.1f V", r.volts);
  if (r.batteryPct >= 0) Serial.printf("  %d%%", r.batteryPct);
  Serial.printf("  st 0x%02X  RSSI %d  [%s]\n", r.status, r.rssi, r.type);

  if (PRINT_RAW) {
    Serial.print("           raw:");
    for (int i = 0; i < r.rawLen; i++) Serial.printf(" %02X", r.raw[i]);
    Serial.println();
  }
  if (pos < 0) Serial.println("           -> press the button to store this sensor");
}

static void processReadings() {
  Reading r;
  while (xQueueReceive(readingQueue, &r, 0) == pdTRUE) {
    // Same sensor + same bytes shortly after the previous one = repeat inside a burst
    bool repeat = haveLastDetected &&
                  strcmp(r.addr, lastDetected.addr) == 0 &&
                  r.rawLen == lastDetected.rawLen &&
                  memcmp(r.raw, lastDetected.raw, r.rawLen) == 0 &&
                  r.ms - lastDetected.ms < BURST_DEDUP_MS;

    lastDetected = r;
    haveLastDetected = true;

    int pos = findPosition(r.addr);
    if (pos >= 0) { positions[pos].last = r; positions[pos].hasReading = true; }

    if (TPMS_DEBUG && !repeat) printReading(r, pos);

    if (pos >= 0 && !repeat) {
      addPressureSample(pos, r.psi * 0.0689476f, r.ms);
      if (isLowBattery(r) && !lowBattWarned[pos]) {
        lowBattWarned[pos] = true;
        Serial.printf("!! %s sensor battery LOW (%.1f V)\n", POSITION_NAMES[pos], r.volts);
      }
    }

    // New data from a paired sensor (or a sensor that can be paired) resets the dim/off timers
    // (other people's sensors nearby are ignored unless we are still pairing)
    if (pos >= 0 || nextFreePosition() >= 0) lastActivity = millis();
  }
}

// ---------------- Display ----------------
static Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

static const uint16_t COL_GREY   = 0x7BEF;
static const uint16_t COL_ORANGE = 0xFD20;

static PosState positionState(int i) {
  const Position& p = positions[i];
  if (!p.addr.length()) return PS_EMPTY;
  if (!p.hasReading)    return PS_WAIT;
  if (millis() - p.last.ms > (uint32_t)STALE_MIN[staleIdx] * 60000UL) return PS_OLD;
  if (hist[i].dropping) return PS_DROP;        // rapid loss has priority over everything
  if (TMAX_C[tmaxIdx] && p.last.tempC > TMAX_C[tmaxIdx]) return PS_HOT;   // then high temperature
  float bar = p.last.psi * 0.0689476f;
  if (bar < p.minBar) return PS_LOW;
  if (bar > p.maxBar) return PS_HIGH;
  return PS_OK;
}

static const uint16_t COL_BLUE = 0x3B9F;   // light blue, readable on black

static uint16_t stateColor(PosState s) {
  switch (s) {
    case PS_OK:   return ST77XX_GREEN;
    case PS_LOW:  return ST77XX_RED;
    case PS_DROP: return ST77XX_RED;
    case PS_HOT:  return ST77XX_MAGENTA;
    case PS_HIGH: return COL_BLUE;
    case PS_OLD:  return COL_ORANGE;
    default:      return COL_GREY;
  }
}

static bool isAlertState(PosState s) { return s == PS_LOW || s == PS_HIGH || s == PS_DROP || s == PS_HOT; }

static const char* stateText(PosState s) {
  switch (s) {
    case PS_EMPTY: return "NOT PAIRED";
    case PS_WAIT:  return "WAITING";
    case PS_OK:    return "OK";
    case PS_LOW:   return "LOW";
    case PS_HIGH:  return "HIGH";
    case PS_DROP:  return "DROPPING!";
    case PS_HOT:   return "HOT!";
    case PS_OLD:   return "NO SIGNAL";
  }
  return "";
}

// Bottom line of a section: temperature, battery, age (redrawn every second)
// ---- Look / formatting helpers ----
static const Palette& T() { return PALETTES[paletteIdx]; }   // current colours
static Layout         L() { return (Layout)layoutIdx; }       // current layout

static void fmtAge(uint32_t ms, char* out, size_t n) {
  uint32_t age = (millis() - ms) / 1000;
  if (age < 60)        snprintf(out, n, "%lus", (unsigned long)age);
  else if (age < 3600) snprintf(out, n, "%lum", (unsigned long)(age / 60));
  else                 snprintf(out, n, "%luh", (unsigned long)(age / 3600));
}

static void fmtBattery(const Reading& r, char* out, size_t n) {
  if (r.volts >= 0)           snprintf(out, n, "%.1fV", r.volts);
  else if (r.batteryPct >= 0) snprintf(out, n, "%d%%", r.batteryPct);
  else                        snprintf(out, n, "?");
}

static float dispTemp(float c) { return useFahrenheit ? c * 1.8f + 32 : c; }
static char  tempUnit()        { return useFahrenheit ? 'F' : 'C'; }

static void fmtPressure(float bar, char* out, size_t n) {
  if (usePsi) snprintf(out, n, "%.1f", bar * 14.5038f);
  else        snprintf(out, n, "%.2f", bar);
}

// The text line that contains the age ("5s ago"); redrawn every second.
static void drawInfoLine(int i) {
  const Position& p = positions[i];
  if (!p.hasReading || !p.addr.length()) return;
  const Reading& r = p.last;
  const int y = i * 80;
  char age[8], bat[8], line[32];
  fmtAge(r.ms, age, sizeof(age));
  fmtBattery(r, bat, sizeof(bat));
  bool  lowBat = isLowBattery(r);
  float t = dispTemp(r.tempC);
  char  tu = tempUnit();
  int x = 4, yy = y + 69, width = 20;
  uint16_t color = lowBat ? T().warn : T().fg;

  switch (L()) {
    case LAY_MINIMAL:
      if (lowBat) snprintf(line, sizeof(line), "%.0f%c  %s LOW  %s", t, tu, bat, age);
      else        snprintf(line, sizeof(line), "%.0f%c  %s  %s ago", t, tu, bat, age);
      x = 8; yy = y + 62; width = 19;
      if (!lowBat) color = T().dim;
      break;
    case LAY_DETAIL:
      snprintf(line, sizeof(line), "RSSI %d   %s ago", r.rssi, age);
      yy = y + 68; color = T().dim;
      break;
    case LAY_RETRO:
      if (lowBat) snprintf(line, sizeof(line), "%.0f%c %s LOW %s", t, tu, bat, age);
      else        snprintf(line, sizeof(line), "%.0f%c  %s  %s", t, tu, bat, age);
      x = 6; yy = y + 64; width = 19;
      break;
    default:   // Classic: battery is in the header
      if (lowBat) snprintf(line, sizeof(line), "%.0f%c  LOW BAT  %s", t, tu, age);
      else        snprintf(line, sizeof(line), "%.0f%c  %s ago", t, tu, age);
      break;
  }
  char padded[32];
  snprintf(padded, sizeof(padded), "%-*s", width, line);   // pad so old text is overwritten
  padded[width] = 0;
  tft.setTextSize(1);
  tft.setTextColor(color, T().bg);
  tft.setCursor(x, yy);
  tft.print(padded);
}

static int nextFreePosition() {
  for (int i = 0; i < NUM_POSITIONS; i++)
    if (!positions[i].addr.length()) return i;
  return -1;
}

// An unpaired sensor heard recently enough to be stored by a click
static bool haveCandidate() {
  return haveLastDetected &&
         millis() - lastDetected.ms <= STORE_WINDOW_MS &&
         findPosition(lastDetected.addr) < 0;
}

// Big pressure number of a section; visible=false erases it (used for blinking).
// Returns the number of characters (to place the unit after it).
// ---- Alert tracking: when did each wheel go out of range (0 = not in alert) ----
static uint32_t alertSince[NUM_POSITIONS]   = {0};
static bool     alertStarted[NUM_POSITIONS] = {false};   // edge flag, consumed by manageScreen()

static void updateAlerts() {
  static PosState alertKind[NUM_POSITIONS];
  for (int i = 0; i < NUM_POSITIONS; i++) {
    PosState st = positionState(i);
    bool alert = isAlertState(st);
    // A new alert, or a change of kind (e.g. LOW -> DROPPING), starts a new blink period
    if (alert && (!alertSince[i] || st != alertKind[i])) {
      alertSince[i] = millis() | 1;
      alertStarted[i] = true;
      alertKind[i] = st;
    }
    if (!alert) alertSince[i] = 0;
  }
}

// Number blinks during the first ALERT_BLINK_MS of an alert
static bool isBlinking(int i) {
  return alertSince[i] && millis() - alertSince[i] < ALERT_BLINK_MS;
}

// A reading older than AGED_MS (sensor quiet, e.g. standing still)
static bool isAged(int i) {
  return positions[i].hasReading && millis() - positions[i].last.ms > AGED_MS;
}

// Colour of the big number + unit: alerts keep their colour, an old OK value turns grey
static uint16_t numberColor(int i, PosState st) {
  if (st != PS_OK) return stateColor(st);
  return isAged(i) ? T().dim : T().fg;
}

static NumSpec numSpec() {
  switch (L()) {
    case LAY_MINIMAL: return { &FreeSansBold24pt7b, 50, 0, true  };
    case LAY_DETAIL:  return { &FreeSansBold18pt7b, 42, 4, false };
    case LAY_RETRO:   return { &FreeMonoBold18pt7b, 46, 6, false };
    default:          return { &FreeSansBold24pt7b, 52, 2, false };
  }
}

// Big pressure number of a section; visible=false erases it (blinking).
// Returns the x of its right edge (the unit is drawn after it).
static int drawPressureNumber(int i, bool visible) {
  const int y = i * 80;
  PosState st = positionState(i);
  NumSpec ns = numSpec();
  char num[8];
  fmtPressure(positions[i].last.psi * 0.0689476f, num, sizeof(num));

  tft.setFont(ns.font);
  tft.setTextSize(1);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(num, 0, 0, &bx, &by, &bw, &bh);
  int x = ns.center ? (128 - (bw + 3 + 18)) / 2 - bx : ns.x;   // centre number + unit
  if (visible) {
    tft.setTextColor(numberColor(i, st));
    tft.setCursor(x, y + ns.baseline);
    tft.print(num);
  } else {
    tft.fillRect(x + bx, y + ns.baseline + by, bw, bh, T().bg);
  }
  tft.setFont();                       // back to the built-in font
  return x + bx + bw;
}

// Small unit text after the number, bottom-aligned with it
static void drawUnit(int i, int numRight, PosState st) {
  tft.setTextSize(1);
  tft.setTextColor(numberColor(i, st));
  tft.setCursor(numRight + 3, i * 80 + numSpec().baseline - 7);
  tft.print(usePsi ? "psi" : "bar");
}

// One section per position: 128 x 80 px, stacked vertically
// Coloured header bar: name + either status, or MIN-MAX and battery
static void drawHeaderBar(int i, PosState st, bool withLimits) {
  const int y = i * 80;
  const Position& p = positions[i];
  uint16_t col = stateColor(st);
  tft.fillRect(0, y, 128, 16, col);
  tft.setTextSize(1);
  tft.setTextColor(isAlertState(st) ? ST77XX_WHITE : ST77XX_BLACK);
  tft.setCursor(4, y + 4);
  tft.print(POSITION_NAMES[i]);
  if (!withLimits) {
    const char* s = stateText(st);
    tft.setCursor(124 - strlen(s) * 6, y + 4);
    tft.print(s);
    return;
  }
  if (usePsi) tft.printf(" %.0f-%.0f", toUnit(p.minBar), toUnit(p.maxBar));
  else        tft.printf(" %.2f-%.2f", p.minBar, p.maxBar);
  char bat[8];
  fmtBattery(p.last, bat, sizeof(bat));
  int bx = 124 - strlen(bat) * 6;
  if (isLowBattery(p.last)) {                         // low battery: black on yellow box
    tft.fillRect(bx - 2, y + 2, strlen(bat) * 6 + 4, 12, ST77XX_YELLOW);
    tft.setTextColor(ST77XX_BLACK);
  }
  tft.setCursor(bx, y + 4);
  tft.print(bat);
}

// Pairing / waiting screens (same in every theme)
static void drawNoData(int i, PosState st) {
  const int y = i * 80;
  const Position& p = positions[i];
  drawHeaderBar(i, st, false);
  tft.setTextColor(T().fg);
  if (st == PS_EMPTY) {
    int next = nextFreePosition();
    if (i != next) {                                  // a later position: pair the earlier one first
      tft.setCursor(4, y + 30); tft.printf("Pair %s first", POSITION_NAMES[next]);
      return;
    }
    if (!haveCandidate()) {
      tft.setCursor(4, y + 26); tft.print("Waiting for sensor...");
      tft.setCursor(4, y + 40); tft.print("Wake it up (pressure");
      tft.setCursor(4, y + 52); tft.print("or shake it)");
      return;
    }
    // Unpaired sensor heard recently: show it, a click will store it here
    const Reading& c = lastDetected;
    tft.fillRect(0, y, 128, 16, ST77XX_YELLOW);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(4, y + 4);   tft.print(POSITION_NAMES[i]);
    tft.setCursor(124 - 12 * 6, y + 4); tft.print("SENSOR FOUND");
    tft.setTextColor(T().fg);
    tft.setCursor(4, y + 24);  tft.print(c.addr);
    tft.setCursor(4, y + 38);
    char num[8];
    fmtPressure(c.psi * 0.0689476f, num, sizeof(num));
    tft.printf("%s %s  %.0f%c", num, usePsi ? "psi" : "bar", dispTemp(c.tempC), tempUnit());
    tft.setTextColor(T().warn);
    tft.setCursor(4, y + 56);  tft.print("Click to store");
    return;
  }
  // PS_WAIT: paired, no reading yet
  tft.setCursor(4, y + 30); tft.print("Waiting for data...");
  tft.setCursor(4, y + 44); tft.print(p.addr);
}

// "DROPPING! from 2.45" / "LOW" ... in the state colour
static void printStatus(int i, PosState st) {
  tft.setTextColor(stateColor(st));
  tft.print(stateText(st));
  if (st == PS_DROP) {
    char from[8];
    fmtPressure(hist[i].dropFrom, from, sizeof(from));
    tft.printf(" from %s", from);
  }
}

// Classic / Light: header bar with limits + battery, big number, status, info
static void drawClassic(int i, PosState st) {
  const int y = i * 80;
  drawHeaderBar(i, st, true);
  drawUnit(i, drawPressureNumber(i, true), st);
  tft.setTextSize(1);
  tft.setCursor(4, y + 58);
  printStatus(i, st);
  drawInfoLine(i);
}

// Minimal / Night: colour stripe, small name + status, centred number, one info line
static void drawMinimal(int i, PosState st) {
  const int y = i * 80;
  tft.fillRect(0, y + 2, 3, 76, stateColor(st));
  tft.setTextSize(1);
  tft.setTextColor(T().dim);
  tft.setCursor(8, y + 4);
  tft.print(POSITION_NAMES[i]);
  const char* s = stateText(st);
  tft.setTextColor(stateColor(st));
  tft.setCursor(124 - strlen(s) * 6, y + 4);
  tft.print(s);
  drawUnit(i, drawPressureNumber(i, true), st);
  if (i < NUM_POSITIONS - 1) tft.drawFastHLine(8, y + 79, 112, T().dim);
  drawInfoLine(i);
}

// Detail: header bar with status, medium number, temperature/battery, limits, RSSI + age
static void drawDetail(int i, PosState st) {
  const int y = i * 80;
  const Position& p = positions[i];
  drawHeaderBar(i, st, false);
  drawUnit(i, drawPressureNumber(i, true), st);
  char bat[8];
  fmtBattery(p.last, bat, sizeof(bat));
  tft.setTextSize(1);
  tft.setTextColor(isLowBattery(p.last) ? T().warn : T().fg);
  tft.setCursor(4, y + 48);
  tft.printf("Temp %.0f%c  Bat %s%s", dispTemp(p.last.tempC), tempUnit(), bat,
             isLowBattery(p.last) ? " LOW" : "");
  tft.setCursor(4, y + 58);
  if (st == PS_DROP) {
    printStatus(i, st);
  } else {
    char mn[8], mx[8];
    fmtPressure(p.minBar, mn, sizeof(mn));
    fmtPressure(p.maxBar, mx, sizeof(mx));
    tft.setTextColor(T().dim);
    tft.printf("Min %s  Max %s", mn, mx);
  }
  drawInfoLine(i);
}

// Retro: double frame in the state colour, mono font
static void drawRetro(int i, PosState st) {
  const int y = i * 80;
  uint16_t col = stateColor(st);
  tft.drawRect(1, y + 1, 126, 78, col);
  tft.drawRect(2, y + 2, 124, 76, col);
  tft.setTextSize(1);
  tft.setTextColor(T().fg);
  tft.setCursor(6, y + 6);
  tft.print(POSITION_NAMES[i]);
  const char* s = stateText(st);
  tft.setTextColor(col);
  tft.setCursor(122 - strlen(s) * 6, y + 6);
  tft.print(s);
  drawUnit(i, drawPressureNumber(i, true), st);
  drawInfoLine(i);
}

// One section per position: 128 x 80 px, stacked vertically
static void drawPosition(int i) {
  const Position& p = positions[i];
  PosState st = positionState(i);
  tft.fillRect(0, i * 80, 128, 80, T().bg);
  if (st == PS_EMPTY || st == PS_WAIT || !p.hasReading) { drawNoData(i, st); return; }
  switch (L()) {
    case LAY_MINIMAL: drawMinimal(i, st); break;
    case LAY_DETAIL:  drawDetail(i, st);  break;
    case LAY_RETRO:   drawRetro(i, st);   break;
    default:          drawClassic(i, st); break;
  }
}

// Redraw a section only when something changed; refresh the age line every second
static void updateDisplay() {
  static PosState drawnState[NUM_POSITIONS];
  static uint32_t drawnMs[NUM_POSITIONS];
  static String   drawnAddr[NUM_POSITIONS];
  static bool     drawnAged[NUM_POSITIONS];
  static bool     first = true;
  static uint32_t lastAgeUpdate = 0;

  if (splashActive) {                              // boot splash still showing
    if (millis() - splashStart < SPLASH_MS) return;
    splashActive = false;
    displayDirty = true;
    lastActivity = millis();                       // dim/off timers start after the splash
  }
  if (menuActive || scrState == SCR_OFF) return;  // menu owns the screen / display asleep
                                                   // (when dimmed the screen is still visible)
  bool ageTick = millis() - lastAgeUpdate >= 1000;
  if (ageTick) lastAgeUpdate = millis();
  if (displayDirty) { first = true; displayDirty = false; }

  // Blink phase for alert numbers: toggles every BLINK_MS
  static uint32_t lastBlink = 0;
  static bool     blinkOn = true;
  static bool     numHidden[NUM_POSITIONS] = {false};   // number currently erased by blinking
  bool blinkTick = millis() - lastBlink >= BLINK_MS;
  if (blinkTick) { lastBlink = millis(); blinkOn = !blinkOn; }

  for (int i = 0; i < NUM_POSITIONS; i++) {
    PosState st = positionState(i);
    uint32_t ms;
    bool     msChanged;
    if (positions[i].hasReading) {
      ms = positions[i].last.ms;
      msChanged = ms != drawnMs[i];
    } else {
      // Empty position: follow the unpaired sensor shown in it (once per burst, not per packet)
      ms = (st == PS_EMPTY && i == nextFreePosition() && haveCandidate()) ? lastDetected.ms : 0;
      msChanged = (ms == 0) != (drawnMs[i] == 0) || (ms && ms - drawnMs[i] > BURST_DEDUP_MS);
    }
    bool aged = isAged(i);
    if (first || st != drawnState[i] || msChanged || positions[i].addr != drawnAddr[i] || aged != drawnAged[i]) {
      drawPosition(i);
      numHidden[i]  = false;
      drawnState[i] = st;
      drawnMs[i]    = ms;
      drawnAddr[i]  = positions[i].addr;
      drawnAged[i]  = aged;
    } else {
      if (ageTick) drawInfoLine(i);
      // Out of range (red / blue): blink the pressure number
      if (blinkTick && isBlinking(i)) {
        drawPressureNumber(i, blinkOn);
        numHidden[i] = !blinkOn;
      } else if (numHidden[i] && !isBlinking(i)) {   // blinking just ended: make sure it is shown
        drawPressureNumber(i, true);
        numHidden[i] = false;
      }
    }
  }
  first = false;
}

// Settings menu screen
static void drawMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 128, 16, ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(4, 4);
  tft.print("SETTINGS");
  tft.setCursor(58, 4);
  tft.print("v" FW_VERSION);
  tft.setCursor(124 - 3 * 6, 4);
  tft.print(usePsi ? "psi" : "bar");

  // Scroll so the selected item is always visible
  int top = menuSel - (MENU_VISIBLE - 1);
  if (top < 0) top = 0;
  if (MENU_ITEMS > MENU_VISIBLE) {     // scrollbar on the right edge
    int trackH = MENU_VISIBLE * 10;
    int thumbH = trackH * MENU_VISIBLE / MENU_ITEMS;
    int thumbY = 19 + (trackH - thumbH) * top / (MENU_ITEMS - MENU_VISIBLE);
    tft.fillRect(126, 19, 2, trackH, 0x39E7);
    tft.fillRect(126, thumbY, 2, thumbH, ST77XX_WHITE);
  }

  for (int k = top; k < MENU_ITEMS && k < top + MENU_VISIBLE; k++) {
    int y = 20 + (k - top) * 10;
    bool sel = (k == menuSel);
    bool armed = sel && (k == MENU_RESET || k == MENU_FACTORY) && resetArmed;
    if (sel) tft.fillRect(0, y - 1, 125, 10, armed ? ST77XX_RED : ST77XX_YELLOW);
    tft.setTextColor(armed ? ST77XX_WHITE : (sel ? ST77XX_BLACK : ST77XX_WHITE));
    tft.setCursor(4, y);
    if (k == MENU_SAVE)  { tft.print("SAVE & EXIT"); continue; }
    if (k == MENU_RESET)   { tft.print(armed ? "CONFIRM RESET?" : "RESET PAIRING"); continue; }
    if (k == MENU_FACTORY) { tft.print(armed ? "ERASE ALL?" : "FACTORY RESET"); continue; }

    char val[10];
    if (k == MENU_SCAN) {
      tft.print("Scan");
      snprintf(val, sizeof(val), "%u%%", SCAN_DUTY[editScan]);
    } else if (k == MENU_BRIGHT) {
      tft.print("Brightness");
      snprintf(val, sizeof(val), "%u%%", BRIGHT_PCT[editBright]);
    } else if (k == MENU_DIMLVL) {
      tft.print("Dim level");
      snprintf(val, sizeof(val), "%u%%", DIM_LVL_PCT[editDimLvl]);
    } else if (k == MENU_STALE) {
      tft.print("No signal");
      snprintf(val, sizeof(val), "%um", STALE_MIN[editStale]);
    } else if (k == MENU_TMAX) {
      tft.print("Max temp");
      if (!TMAX_C[editTmax])  snprintf(val, sizeof(val), "Off");
      else if (editFahrenheit) snprintf(val, sizeof(val), "%.0fF", TMAX_C[editTmax] * 1.8f + 32);
      else                    snprintf(val, sizeof(val), "%uC", TMAX_C[editTmax]);
    } else if (k == MENU_PUNIT) {
      tft.print("Pressure unit");
      snprintf(val, sizeof(val), usePsi ? "psi" : "bar");
    } else if (k == MENU_TUNIT) {
      tft.print("Temp unit");
      snprintf(val, sizeof(val), editFahrenheit ? "F" : "C");
    } else if (k == MENU_LAYOUT) {
      tft.print("Layout");
      snprintf(val, sizeof(val), "%s", LAYOUT_NAMES[editLayout]);
    } else if (k == MENU_COLORS) {
      tft.print("Colors");
      snprintf(val, sizeof(val), "%s", PALETTES[editPalette].name);
    } else if (k == MENU_ROT) {
      tft.print("Rotate");
      snprintf(val, sizeof(val), editRotate ? "180" : "0");
    } else if (k == MENU_DIM || k == MENU_OFF) {
      tft.print(k == MENU_DIM ? "Dim after" : "Screen off");
      uint16_t s = (k == MENU_DIM) ? DIM_S[editDim] : SCREEN_OFF_S[editOff];
      if (s == 0)       snprintf(val, sizeof(val), "Never");
      else if (s < 60)  snprintf(val, sizeof(val), "%us", s);
      else              snprintf(val, sizeof(val), "%um", s / 60);
    } else {
      int  pos   = (k - MENU_LIM0) / 2;
      bool isMax = (k - MENU_LIM0) % 2;
      tft.printf("%s %s", POSITION_NAMES[pos], isMax ? "MAX" : "MIN");
      float v = toUnit(isMax ? editMax[pos] : editMin[pos]);
      if (usePsi) snprintf(val, sizeof(val), "%.0f", v);
      else        snprintf(val, sizeof(val), "%.2f", v);
    }
    tft.setCursor(122 - strlen(val) * 6, y);
    tft.print(val);
  }

  tft.setTextColor(COL_GREY);
  tft.setCursor(4, 139);
  const char* help1 = "Click: +  (wraps)";
  const char* help2 = "Hold 1s next/5s SAVE";
  if (menuSel == MENU_PUNIT || (menuSel >= MENU_TUNIT && menuSel <= MENU_COLORS)) help1 = "Click: change";
  if (menuSel == MENU_SAVE)  help1 = "Click: save";
  if (menuSel == MENU_RESET) {
    help1 = resetArmed ? "Click: CONFIRM reset" : "Click: reset pairing";
    if (resetArmed) help2 = "Hold 1s: cancel";
  }
  if (menuSel == MENU_FACTORY) {
    help1 = resetArmed ? "Click: ERASE ALL" : "Click: erase all";
    if (resetArmed) help2 = "Hold 1s: cancel";
  }
  tft.print(help1);
  tft.setCursor(4, 150);
  tft.print(help2);
}

// Short full-screen message (1.5 s)
static void showMessage(const char* line1, const char* line2, uint16_t bg) {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 55, 128, 50, bg);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor((128 - strlen(line1) * 12) / 2, 64);
  tft.print(line1);
  tft.setTextSize(1);
  tft.setCursor((128 - strlen(line2) * 6) / 2, 88);
  tft.print(line2);
  delay(1500);
}


// Full-screen confirmation, then back to the normal view
static void showResetScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 50, 128, 60, ST77XX_YELLOW);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(22, 58);
  tft.print("PAIRING");
  tft.setCursor(34, 78);
  tft.print("RESET");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 124);
  tft.printf("Wake %s sensor", POSITION_NAMES[0]);
  tft.setCursor(10, 136);
  tft.print("and press button");
  delay(2000);
  displayDirty = true;
}

// Portrait 0 or 180 degrees (both 128 x 160)
static void applyRotation(bool flip) {
  tft.setRotation(flip ? 2 : 0);
}

// ---------------- Backlight / screen sleep ----------------
static void setBacklightPct(int pct) {
  if (TFT_BL < 0) return;
  ledcWrite(TFT_BL, (uint32_t)pct * 255 / 100);
}

// ON  : backlight at the selected brightness
// DIM : backlight at the dim level (1-25 %), still readable
// OFF : backlight 0 + display controller asleep
static void setScreenState(ScreenState target) {
  if (target == scrState) return;
  if (scrState == SCR_OFF) {           // leaving sleep: wake the controller and redraw
    tft.enableSleep(false);
    delay(120);
    displayDirty = true;
  }
  if (target == SCR_OFF) tft.enableSleep(true);
  int pct = 0;
  if (target == SCR_ON)  pct = BRIGHT_PCT[brightIdx];
  if (target == SCR_DIM) pct = min((int)DIM_LVL_PCT[dimLvlIdx], (int)BRIGHT_PCT[brightIdx]);
  setBacklightPct(pct);
  scrState = target;
  if (TPMS_DEBUG) Serial.printf(">> Screen %s\n", target == SCR_ON ? "on" : target == SCR_DIM ? "dimmed" : "off");
}

// Two-stage idle timer: "Dim after" -> backlight to "Dim level", "Screen off" -> backlight 0 + display sleep.
// Idle time restarts on a button press, new sensor data (see processReadings) and
// while there is an alert (tyre below MIN / above MAX, or an unpaired sensor found
// while pairing) or the menu is open.
static void manageScreen() {
  if (TFT_BL < 0) return;              // no backlight control: always on

  // Splash, menu open or an unpaired sensor waiting to be stored: keep the screen on
  bool attention = splashActive || menuActive || (nextFreePosition() >= 0 && haveCandidate());

  // Alerts (a wheel red/blue):
  //  - wake the screen when they start,
  //  - while the number blinks (first 5 min) the dim stage is skipped,
  //  - after that dimming works again; "Screen off" always applies.
  bool anyBlinking = false;
  for (int i = 0; i < NUM_POSITIONS; i++) {
    if (alertStarted[i]) { attention = true; alertStarted[i] = false; }
    anyBlinking |= isBlinking(i);
  }
  if (attention) lastActivity = millis();

  uint32_t idle   = millis() - lastActivity;
  uint16_t dimS   = DIM_S[dimIdx];
  uint16_t offS   = SCREEN_OFF_S[screenOffIdx];
  ScreenState target = SCR_ON;
  if (dimS && !anyBlinking && idle > dimS * 1000UL) target = SCR_DIM;
  if (offS && idle > offS * 1000UL) target = SCR_OFF;
  setScreenState(target);
}

static void initDisplay() {
  if (TFT_BL >= 0) {
    ledcAttach(TFT_BL, 5000, 8);       // 5 kHz, 8-bit PWM
    setBacklightPct(100);
  }
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.initR(TFT_TAB);
  tft.setSPISpeed(27000000);
  tft.setRotation(0);              // portrait, 128 x 160 (rotate180 applied after loading settings)
  drawSplash();
}

// Boot splash: logo (white on black), title and version.
// Stays SPLASH_MS while BLE already starts scanning; a click skips it.
static void drawSplash() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawBitmap((128 - LOGO_W) / 2, 40, LOGO_BITMAP, LOGO_W, LOGO_H, ST77XX_WHITE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor((128 - 4 * 12) / 2, 100);
  tft.print("TPMS");
  tft.setTextSize(1);
  tft.setTextColor(COL_GREY);
  tft.setCursor((128 - (strlen(FW_VERSION) + 1) * 6) / 2, 140);
  tft.print("v" FW_VERSION);
  esp_reset_reason_t rr = esp_reset_reason();
  if (isAbnormalReset(rr)) {                     // e.g. "restarted: watchdog"
    char msg[32];
    snprintf(msg, sizeof(msg), "restarted: %s", resetReasonText(rr));
    tft.setTextColor(ST77XX_RED);
    tft.setCursor((128 - strlen(msg) * 6) / 2, 124);
    tft.print(msg);
  }
  splashActive = true;
  splashStart  = millis();
}

// ---------------- Watchdog / reset reason ----------------
// Why did the board start? Abnormal causes are logged and shown on the splash.
static const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "low voltage";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_USB:       return "USB reset";
    case ESP_RST_EXT:       return "reset pin";
    default:                return "unknown";
  }
}

static bool isAbnormalReset(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT   || r == ESP_RST_BROWNOUT;
}

static void startWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;          // only watch loop(), not the idle task
  cfg.trigger_panic  = true;       // restart when it fires
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
  esp_task_wdt_add(NULL);          // NULL = this task (the Arduino loop task)
}

// ---------------- Setup / loop ----------------
void setup() {
  setCpuFrequencyMhz(CPU_MHZ);
  Serial.begin(115200);
  // Never block on serial output: if USB is connected but no serial monitor is
  // reading, prints would otherwise stall loop() (button + screen freeze).
  Serial.setTxTimeoutMs(0);
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) delay(10);
  delay(500);
  Serial.println();
  Serial.println("=== ESP-TPMS (ESP32-C3) v" FW_VERSION " ===");
  Serial.printf("Last reset: %s%s\n", resetReasonText(esp_reset_reason()),
                isAbnormalReset(esp_reset_reason()) ? "  <-- ABNORMAL" : "");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  readingQueue = xQueueCreate(20, sizeof(Reading));
  initDisplay();

  loadPositions();
  setBacklightPct(BRIGHT_PCT[brightIdx]);
  if (rotate180) { applyRotation(true); drawSplash(); }   // redraw splash upside down
  lastActivity = millis();
  Serial.printf("CPU %lu MHz, scan %u%%, brightness %u%%, dim %us to %u%%, screen off %us\n",
                (unsigned long)getCpuFrequencyMhz(), SCAN_DUTY[scanIdx], BRIGHT_PCT[brightIdx],
                DIM_S[dimIdx], DIM_LVL_PCT[dimLvlIdx], SCREEN_OFF_S[screenOffIdx]);
  printPositions();
  for (int i = 0; i < NUM_POSITIONS; i++) {
    if (!positions[i].addr.length()) {
      Serial.printf("Wake the %s sensor, then press the button (GPIO%d) to store it.\n",
                    POSITION_NAMES[i], BUTTON_PIN);
      break;
    }
  }

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);  // report duplicates
  scan->setActiveScan(true);
  applyScanSettings();                           // interval 100 ms, window = duty %

  startWatchdog();
  Serial.printf("Watchdog: %lu s\n", (unsigned long)(WDT_TIMEOUT_MS / 1000));
}

void loop() {
  // Diagnostics: longest single loop() pass since the last status print
  static uint32_t loopStart = 0, maxLoopMs = 0;
  uint32_t nowMs = millis();
  if (loopStart && nowMs - loopStart > maxLoopMs) maxLoopMs = nowMs - loopStart;
  loopStart = nowMs;

  esp_task_wdt_reset();                // feed the watchdog: "loop is still running"
#ifdef WDT_TEST
  if (millis() > 20000) { Serial.println("WDT_TEST: hanging on purpose..."); while (true) {} }
#endif
  processReadings();
  handleButton();
  updateAlerts();
  manageScreen();
  updateDisplay();

  static uint32_t lastStatus = 0;
  if (TPMS_DEBUG && millis() - lastStatus >= STATUS_EVERY_MS) {
    lastStatus = millis();
    Serial.printf("[%8lu ms] alive - %lu BLE adverts received, slowest loop %lu ms\n",
                  millis(), (unsigned long)advCount, (unsigned long)maxLoopMs);
    maxLoopMs = 0;
    printPositions();
  }
  delay(10);
}

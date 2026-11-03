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

// ---------------- Current look (used by the display tab) ----------------
// Note: the main file must contain a function after the data types above;
// Arduino inserts the generated prototypes of ALL tabs before it.
static const Palette& T() { return PALETTES[paletteIdx]; }   // current colours
static Layout         L() { return (Layout)layoutIdx; }       // current layout

// Code is split into tabs (compiled in this order after this file):
//   10_ble.ino         Decoders
//   20_storage.ino     Positions (stored in flash)
//   30_alerts.ino      Rapid pressure loss / battery
//   40_pairing.ino     Button actions
//   50_menu.ino        Settings menu (MIN/MAX per position)
//   60_readings.ino    Reading handling
//   70_display.ino     Display
//   80_screen.ino      Backlight / screen sleep
//   90_system.ino      Watchdog / reset reason
//   99_setup_loop.ino  Setup / loop


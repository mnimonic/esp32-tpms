// ---------------- SD card measurement log ----------------
// One CSV file per ignition-on ("ride"): /logs/rideNNNN.csv, numbered upwards.
// Columns:
//   t_s       seconds since power-on (there is no clock on the board)
//   pos       Front / Back (empty for board events)
//   event     data | state | lowbat | boot | sensor
//   bar, temp_c, volts, batt_pct, status, rssi   last reading of that wheel
//   state     OK / LOW / HIGH / DROPPING! / HOT! / NO SIGNAL / WAITING / NOT PAIRED
//   note      extra text (boot info, sensor address, limits)
// "data" rows: one per wheel every LOG_EVERY_MS, plus immediately when the sensor's
// status byte changes. "state" rows: whenever a wheel's state changes (alerts, no signal).
// Power is cut without warning (ignition), so rows are written in small blocks
// and flushed: at most LOG_FLUSH_MS of data is lost.

#include "sd_diskio.h"      // sdcard_init / sdcard_uninit (for formatting)
#include "ff.h"             // f_mkfs

static const char* LOG_DIR = "/logs";

static int rideNumberOf(const char* name) {      // "ride0012.csv" -> 12, otherwise -1
  if (strncmp(name, "ride", 4) != 0 || !strstr(name, ".csv")) return -1;
  return atoi(name + 4);
}

// Highest ride number on the card (lowest one in 'lowest'; 0 = none)
static int scanRides(int& lowest) {
  int highest = 0;
  lowest = 0;
  File dir = SD.open(LOG_DIR);
  if (!dir) return 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    int n = rideNumberOf(f.name());
    if (n > 0) {
      if (n > highest) highest = n;
      if (!lowest || n < lowest) lowest = n;
    }
    f.close();
  }
  dir.close();
  return highest;
}

// Card nearly full: delete the oldest rides (keeps at least the newest one)
static void freeSpace() {
  for (int guard = 0; guard < 50; guard++) {
    if (SD.totalBytes() - SD.usedBytes() >= LOG_MIN_FREE) return;
    int lowest, highest = scanRides(lowest);
    if (!lowest || lowest >= highest) return;
    char path[32];
    snprintf(path, sizeof(path), "%s/ride%04d.csv", LOG_DIR, lowest);
    SD.remove(path);
    Serial.printf("SD: card full, deleted %s\n", path);
  }
}

static void logRow(uint32_t ms, int pos, const char* event, const Reading* r,
                   const char* state, const char* note) {
  if (!sdOk) return;
  char line[160];
  int n = snprintf(line, sizeof(line), "%lu.%lu,%s,%s,", (unsigned long)(ms / 1000),
                   (unsigned long)(ms % 1000 / 100), pos >= 0 ? POSITION_NAMES[pos] : "", event);
  if (r) {
    n += snprintf(line + n, sizeof(line) - n, "%.3f,%.0f,", r->psi * 0.0689476f, r->tempC);
    if (r->volts >= 0) n += snprintf(line + n, sizeof(line) - n, "%.1f,", r->volts);
    else               n += snprintf(line + n, sizeof(line) - n, ",");
    if (r->batteryPct >= 0) n += snprintf(line + n, sizeof(line) - n, "%d,", r->batteryPct);
    else                    n += snprintf(line + n, sizeof(line) - n, ",");
    n += snprintf(line + n, sizeof(line) - n, "0x%02X,%d,", r->status, r->rssi);
  } else {
    n += snprintf(line + n, sizeof(line) - n, ",,,,,,");
  }
  snprintf(line + n, sizeof(line) - n, "%s,%s\n", state ? state : "", note ? note : "");
  logBuf += line;
}

// Writes the buffered rows to the card
static void logFlush() {
  if (!sdOk || !logBuf.length()) return;
  size_t len = logBuf.length();
  size_t written = logFile.print(logBuf);
  logFile.flush();
  logBuf = "";
  if (written != len) {
    sdOk = false;
    logFile.close();
    Serial.println("!! SD: write failed (card removed or full) - logging stopped until restart");
  }
}

// Called once in setup(), after the display (shared SPI bus) and the settings are ready
static void sdInit() {
  sdChecked = true;
  if (SD_CS < 0) return;
  if (!SD.begin(SD_CS, SPI, SD_SPI_HZ, "/sd", 2)) {
    Serial.println("SD: no card - not logging");
    return;
  }
  if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
  freeSpace();
  int lowest;
  rideNo = scanRides(lowest) + 1;
  if (rideNo > 9999) rideNo = 1;                 // wrap (old file is overwritten)
  char path[32];
  snprintf(path, sizeof(path), "%s/ride%04d.csv", LOG_DIR, rideNo);
  logFile = SD.open(path, FILE_WRITE);
  if (!logFile) {
    Serial.printf("!! SD: cannot create %s - not logging\n", path);
    return;
  }
  sdOk = true;
  logBuf.reserve(2048);
  logFile.print("t_s,pos,event,bar,temp_c,volts,batt_pct,status,rssi,state,note\n");

  char note[96];
  snprintf(note, sizeof(note), "v" FW_VERSION " reset=%s", resetReasonText(esp_reset_reason()));
  logRow(millis(), -1, "boot", nullptr, nullptr, note);
  for (int i = 0; i < NUM_POSITIONS; i++) {
    snprintf(note, sizeof(note), "%s min=%.2f max=%.2f",
             positions[i].addr.length() ? positions[i].addr.c_str() : "none",
             positions[i].minBar, positions[i].maxBar);
    logRow(millis(), i, "sensor", nullptr, nullptr, note);
  }
  logFlush();
  Serial.printf("SD: %llu MB card, %llu MB free, logging to %s\n",
                SD.cardSize() / (1024 * 1024), (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024), path);
}

// Every new (non-repeat) reading of a paired sensor
static void logReading(int pos, const Reading& r) {
  static uint32_t lastLog[NUM_POSITIONS] = {0};
  static int      lastStatus[NUM_POSITIONS];
  static bool     init = false;
  if (!init) { for (int i = 0; i < NUM_POSITIONS; i++) lastStatus[i] = -1; init = true; }
  if (!sdOk) return;
  bool statusChanged = r.status != lastStatus[pos];
  if (!statusChanged && lastLog[pos] && r.ms - lastLog[pos] < LOG_EVERY_MS) return;
  lastLog[pos] = r.ms | 1;
  lastStatus[pos] = r.status;
  logRow(r.ms, pos, "data", &r, stateText(positionState(pos)), statusChanged ? "status changed" : nullptr);
}

static void logLowBattery(int pos, const Reading& r) {
  logRow(r.ms, pos, "lowbat", &r, stateText(positionState(pos)), nullptr);
  logFlush();
}

// Called from loop(): state changes are logged (and written) immediately
static void sdLogLoop() {
  if (!sdOk) return;
  static int      shown[NUM_POSITIONS];
  static bool     init = false;
  static uint32_t lastFlush = 0;
  if (!init) { for (int i = 0; i < NUM_POSITIONS; i++) shown[i] = -1; init = true; }

  bool changed = false;
  for (int i = 0; i < NUM_POSITIONS; i++) {
    PosState st = positionState(i);
    if ((int)st == shown[i]) continue;
    char note[40] = "";
    if (st == PS_DROP) snprintf(note, sizeof(note), "from %.2f bar", hist[i].dropFrom);
    if (st == PS_WAIT && shown[i] == PS_EMPTY) snprintf(note, sizeof(note), "paired %s", positions[i].addr.c_str());
    shown[i] = st;
    logRow(millis(), i, "state", positions[i].hasReading ? &positions[i].last : nullptr,
           stateText(st), note[0] ? note : nullptr);
    changed = true;
  }
  if (changed || logBuf.length() > 1500 || millis() - lastFlush >= LOG_FLUSH_MS) {
    lastFlush = millis();
    logFlush();
  }
}

// ---- Card maintenance (SD card submenu) ----

// Close everything, remount and start a new ride file (also picks up a card inserted later)
static void sdRestart() {
  if (sdOk) logFlush();
  if (logFile) logFile.close();
  sdOk = false;
  logBuf = "";
  SD.end();
  sdInit();
}

// Deletes every ride file, then starts a new log. Returns the number deleted, -1 = no card.
static int sdDeleteLogs() {
  if (!sdOk) sdRestart();
  if (!sdOk) return -1;
  logFlush();
  logFile.close();
  sdOk = false;
  static String names[64];
  int n = 0, total = 0;
  do {                                 // collect first, then delete (max 64 per pass)
    n = 0;
    File dir = SD.open(LOG_DIR);
    for (File f = dir ? dir.openNextFile() : File(); f && n < 64; f = dir.openNextFile()) {
      if (rideNumberOf(f.name()) > 0) names[n++] = String(LOG_DIR) + "/" + f.name();
      f.close();
    }
    if (dir) dir.close();
    int removed = 0;
    for (int i = 0; i < n; i++) {
      if (SD.remove(names[i])) removed++;
      esp_task_wdt_reset();
    }
    total += removed;
    if (!removed) break;               // nothing could be removed: stop
  } while (n == 64);
  Serial.printf(">> SD: deleted %d log file(s)\n", total);
  sdRestart();                         // new log starts at ride0001
  return total;
}

// Quick format (FatFs picks FAT/FAT32/exFAT for the size). 0 = ok, 1 = no card, 2 = failed.
static int sdFormat() {
  if (SD_CS < 0) return 1;
  if (sdOk) logFlush();
  if (logFile) logFile.close();
  sdOk = false;
  logBuf = "";
  SD.end();
  uint8_t pdrv = sdcard_init(SD_CS, &SPI, SD_SPI_HZ);
  if (pdrv == 0xFF) { sdInit(); return 1; }
  char drv[3] = {(char)('0' + pdrv), ':', 0};
  const size_t WORK = 16 * 1024;       // bigger buffer = multi-sector writes = faster
  BYTE* work = (BYTE*)malloc(WORK);
  FRESULT res = FR_NOT_ENOUGH_CORE;
  if (work) {
    Serial.println(">> SD: formatting...");
    uint32_t t0 = millis();
    esp_task_wdt_delete(NULL);         // a big card can take longer than the 10 s watchdog
    const MKFS_PARM opt = {(BYTE)FM_ANY, 0, 0, 0, 0};
    res = f_mkfs(drv, &opt, work, WORK);
    esp_task_wdt_add(NULL);
    free(work);
    Serial.printf(">> SD: format %s (%d) in %lu ms\n", res == FR_OK ? "done" : "FAILED", (int)res,
                  (unsigned long)(millis() - t0));
  }
  sdcard_uninit(pdrv);
  sdInit();                            // mount the fresh card, new log ride0001
  if (res == FR_NOT_READY) return 1;
  return (res == FR_OK && sdOk) ? 0 : 2;
}

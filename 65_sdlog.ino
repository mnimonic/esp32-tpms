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

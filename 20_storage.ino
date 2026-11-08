// ---------------- Positions (stored in flash) ----------------
static String prefKey(int i) { return "pos" + String(i); }

static void loadPositions() {
  prefs.begin("tpms", true);
  for (int i = 0; i < NUM_POSITIONS; i++) {
    positions[i].addr = prefs.getString(prefKey(i).c_str(), "");
    positions[i].hasReading = false;
    positions[i].restored   = false;
    if (positions[i].addr.length()) loadLastReading(i);
    positions[i].minBar = prefs.getFloat(("min" + String(i)).c_str(), DEFAULT_MIN_BAR);
    positions[i].maxBar = prefs.getFloat(("max" + String(i)).c_str(), DEFAULT_MAX_BAR);
  }
  usePsi       = prefs.getBool("psi", false);
  scanIdx      = prefs.getUChar("scan", DEFAULT_SCAN_IDX)   % NUM_OPTS;
  brightIdx    = prefs.getUChar("bri",  DEFAULT_BRIGHT_IDX) % NUM_OPTS;
  dimIdx       = prefs.getUChar("dim",  DEFAULT_DIM_IDX)    % NUM_DIM;
  dimLvlIdx    = prefs.getUChar("diml", DEFAULT_DIM_LVL_IDX) % NUM_DIM_LVLS;
  staleIdx     = prefs.getUChar("stale", DEFAULT_STALE_IDX) % NUM_STALE;
  tmaxIdx      = prefs.getUChar("tmax", DEFAULT_TMAX_IDX)   % NUM_TMAX;
  buzzIdx      = prefs.getUChar("buzz", BUZZ_ALERTS)        % NUM_BUZZ;
  useFahrenheit = prefs.getBool("degf", false);
  rotate180     = prefs.getBool("rot", false);
  layoutIdx     = prefs.getUChar("layout", 0) % NUM_LAYOUTS;
  paletteIdx    = prefs.getUChar("colors", 0) % NUM_PALETTES;
  screenOffIdx = prefs.getUChar("off",  DEFAULT_OFF_IDX)    % NUM_OFF;
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
  prefs.putUChar("buzz", buzzIdx);
  prefs.putBool("degf", useFahrenheit);
  prefs.putBool("rot", rotate180);
  prefs.putUChar("layout", layoutIdx);
  prefs.putUChar("colors", paletteIdx);
  prefs.putUChar("off",  screenOffIdx);
  prefs.end();
}

// ---- Last reading per wheel (shown after power-on until the sensor sends again) ----
struct StoredReading { float psi, tempC, volts; int16_t batteryPct; uint8_t status; char type[5]; };
static String lastKey(int i) { return "last" + String(i); }

// Called inside loadPositions (prefs already open)
static void loadLastReading(int i) {
  StoredReading s;
  if (prefs.getBytesLength(lastKey(i).c_str()) != sizeof(s)) return;
  prefs.getBytes(lastKey(i).c_str(), &s, sizeof(s));
  Reading& r = positions[i].last;
  memset(&r, 0, sizeof(r));
  strlcpy(r.addr, positions[i].addr.c_str(), sizeof(r.addr));
  strlcpy(r.type, s.type, sizeof(r.type));
  r.psi = s.psi; r.tempC = s.tempC; r.volts = s.volts; r.batteryPct = s.batteryPct;
  r.status = s.status; r.rssi = 0; r.ms = 0;
  positions[i].hasReading = true;
  positions[i].restored   = true;
}

// Called for every fresh (non-repeat) reading of a paired sensor
static void maybeSaveLastReading(int i, const Reading& r) {
  static uint32_t savedMs[NUM_POSITIONS] = {0};
  static float    savedBar[NUM_POSITIONS], savedC[NUM_POSITIONS];
  float bar = r.psi * 0.0689476f;
  if (savedMs[i]) {
    if (millis() - savedMs[i] < LAST_SAVE_MS) return;
    if (fabsf(bar - savedBar[i]) < LAST_SAVE_BAR && fabsf(r.tempC - savedC[i]) < LAST_SAVE_C) return;
  }
  StoredReading s = { r.psi, r.tempC, r.volts, (int16_t)r.batteryPct, r.status, {0} };
  strlcpy(s.type, r.type, sizeof(s.type));
  prefs.begin("tpms", false);
  prefs.putBytes(lastKey(i).c_str(), &s, sizeof(s));
  prefs.end();
  savedMs[i] = millis() | 1; savedBar[i] = bar; savedC[i] = r.tempC;
  if (TPMS_DEBUG) Serial.printf(">> %s: last reading saved (%.2f bar, %.0f C)\n", POSITION_NAMES[i], bar, r.tempC);
}

static void savePosition(int i) {
  prefs.begin("tpms", false);
  prefs.putString(prefKey(i).c_str(), positions[i].addr);
  if (!positions[i].addr.length()) prefs.remove(lastKey(i).c_str());   // pairing cleared
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
      if (positions[i].restored)
        Serial.printf("  %.2f bar / %.1f psi  %.0f C  (last ride)\n", r.psi * 0.0689476f, r.psi, r.tempC);
      else
        Serial.printf("  %.2f bar / %.1f psi  %.0f C  (%lu s ago)\n",
                      r.psi * 0.0689476f, r.psi, r.tempC, (millis() - r.ms) / 1000);
    } else {
      Serial.println("  (no reading yet)");
    }
  }
  Serial.println("------------------------");
}


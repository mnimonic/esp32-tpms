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


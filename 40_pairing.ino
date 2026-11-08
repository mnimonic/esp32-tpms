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
      positions[i].restored   = false;
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
    positions[i].restored   = false;
    savePosition(i);
    resetHistory(i);
  }
  haveLastDetected = false;            // only sensors heard AFTER the reset can be stored
  showResetScreen();
  Serial.printf(">> Pairing reset: all positions cleared. Wake the %s sensor and press the button.\n",
                POSITION_NAMES[0]);
}


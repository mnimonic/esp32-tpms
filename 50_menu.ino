// ---------------- Settings menu (MIN/MAX per position) ----------------
// Items: WiFi setup, Pressure unit, Front MIN, Front MAX, Back MIN, Back MAX, Temp unit, ..., Scan, Brightness,
//        Screen off, RESET PAIRING, SAVE & EXIT
static const int MENU_WIFI   = 0;                        // first (easy to reach): save menu + start WiFi settings page
static const int MENU_PUNIT  = 1;                        // bar / psi (before MIN/MAX: they use this unit)
static const int MENU_LIM0   = 2;                        // first MIN/MAX item: Front MIN, Front MAX, Back MIN, Back MAX
static const int MENU_TUNIT  = MENU_LIM0 + NUM_POSITIONS * 2;   // C / F
static const int MENU_TMAX   = MENU_TUNIT + 1;
static const int MENU_STALE  = MENU_TUNIT + 2;
static const int MENU_BUZZ   = MENU_TUNIT + 3;
static const int MENU_SCAN   = MENU_TUNIT + 4;
static const int MENU_BRIGHT = MENU_TUNIT + 5;
static const int MENU_DIM    = MENU_TUNIT + 6;
static const int MENU_DIMLVL = MENU_TUNIT + 7;
static const int MENU_OFF    = MENU_TUNIT + 8;
static const int MENU_ROT    = MENU_TUNIT + 9;
static const int MENU_LAYOUT = MENU_TUNIT + 10;
static const int MENU_COLORS = MENU_TUNIT + 11;
static const int MENU_RESET  = MENU_TUNIT + 12;
static const int MENU_FACTORY= MENU_TUNIT + 13;
static const int MENU_SAVE   = MENU_TUNIT + 14;
static const int MENU_ITEMS  = MENU_TUNIT + 15;
static const int MENU_VISIBLE = 11;    // rows that fit on screen; the list scrolls
static bool      menuActive = false;
static bool      resetArmed = false;   // RESET PAIRING / FACTORY RESET need a second click to confirm
static int       menuSel = 0;
static float     editMin[NUM_POSITIONS], editMax[NUM_POSITIONS];
static int       editScan, editBright, editDim, editDimLvl, editOff, editStale, editTmax, editBuzz;
static bool      editFahrenheit, editRotate;
static int       editLayout, editPalette;
static bool      savedPsi;            // unit before the menu opened (restored if not saved)
static uint32_t  menuLastInput = 0;

static void drawMenu();
static void showMessage(const char* line1, const char* line2, uint16_t bg);
static void drawMessage(const char* line1, const char* line2, uint16_t bg);
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
  editBuzz   = buzzIdx;
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

// quiet: no SAVED / NOT SAVED screen (WiFi setup shows its own)
static void closeMenu(bool save, bool quiet) {
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
    buzzIdx      = editBuzz;
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
    if (!quiet) showMessage("SAVED", "", ST77XX_GREEN);
  } else {
    Serial.println(">> Settings menu closed without saving");
    setBacklightPct(BRIGHT_PCT[brightIdx]);          // undo brightness preview
    applyRotation(rotate180);                        // undo rotation preview
    usePsi = savedPsi;                               // undo unit preview
    if (!quiet) showMessage("NOT SAVED", "(timeout)", 0x7BEF);
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
  if (menuSel == MENU_SAVE) { closeMenu(true, false); return; }
  if (menuSel == MENU_WIFI) {               // keeps the menu changes, no SAVED screen
    drawMessage("WiFi", "starting...", 0x3B9F);
    closeMenu(true, true);
    wifiStart();                             // replaces it with the connect screen
    return;
  }
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
  else if (menuSel == MENU_BUZZ)   { editBuzz = (editBuzz + 1) % NUM_BUZZ;          // test beep
                                     if (editBuzz != BUZZ_OFF) buzzerPlay(PAT_TEST, 3, true); }
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
      buzzerClick();
      if (wifiActive)      wifiStop();                 // WiFi screen: click = stop WiFi
      else if (menuActive) menuClick();
      else                 onShortPress();
    }
  }

  if (stable == LOW) {
    uint32_t held = millis() - pressStart;
    if (!holdDone && menuActive && held >= MENU_HOLD_MS) {          // menu: hold 1 s = next item
      holdDone = true;
      menuNext();
    } else if (!holdDone && !menuActive && !wifiActive && held >= MENU_OPEN_MS) {  // normal: long press = open menu
      holdDone = true;
      openMenu();
    }
    // Menu: keep holding to 5 s = save & exit (only for a press that started in the menu)
    if (menuHold && menuActive && held >= MENU_SAVE_MS) {
      menuHold = false;
      Serial.println(">> Menu: long hold -> save & exit");
      closeMenu(true, false);
    }
  }

  if (menuActive && millis() - menuLastInput > MENU_TIMEOUT_MS) closeMenu(false, false);
}


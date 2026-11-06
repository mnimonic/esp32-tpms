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
      buzzerAlert(st);                 // sound the new alert
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
  if (menuActive || wifiActive || scrState == SCR_OFF) return;  // menu / WiFi screen / display asleep
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
    } else if (k == MENU_WIFI) {
      tft.print("WiFi setup");
      snprintf(val, sizeof(val), "Start");
    } else if (k == MENU_BUZZ) {
      tft.print("Buzzer");
      snprintf(val, sizeof(val), "%s", BUZZ_NAMES[editBuzz]);
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
  if (menuSel == MENU_WIFI)  help1 = "Click: save + WiFi";
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

// Full-screen message box (stays until something else is drawn)
static void drawMessage(const char* line1, const char* line2, uint16_t bg) {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 55, 128, 50, bg);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor((128 - strlen(line1) * 12) / 2, 64);
  tft.print(line1);
  tft.setTextSize(1);
  tft.setCursor((128 - strlen(line2) * 6) / 2, 88);
  tft.print(line2);
}

// Short full-screen message (1.5 s)
static void showMessage(const char* line1, const char* line2, uint16_t bg) {
  drawMessage(line1, line2, bg);
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


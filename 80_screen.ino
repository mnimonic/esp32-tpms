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
  bool attention = splashActive || menuActive || wifiActive || (nextFreePosition() >= 0 && haveCandidate());

  // After power-on: stay on until every paired wheel sent fresh data (max WAIT_KEEP_ON_MS)
  if (millis() < WAIT_KEEP_ON_MS) {
    for (int i = 0; i < NUM_POSITIONS; i++)
      if (positions[i].addr.length() && (!positions[i].hasReading || positions[i].restored)) attention = true;
  }

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
  if (SD_CS >= 0) { pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH); }   // keep the SD card off the bus
  SPI.begin(TFT_SCK, SD_CS >= 0 ? SD_MISO : -1, TFT_MOSI, -1);  // MISO only needed by the SD card
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
  drawSplashSd();
  splashActive = true;
  splashStart  = millis();
}

// SD status line on the splash ("log ride 12" / "no SD card")
static void drawSplashSd() {
  if (!sdChecked || SD_CS < 0) return;
  char msg[24];
  if (sdOk) snprintf(msg, sizeof(msg), "SD log: ride %d", rideNo);
  else      snprintf(msg, sizeof(msg), "no SD card");
  tft.setTextSize(1);
  tft.setTextColor(sdOk ? COL_GREY : COL_ORANGE, ST77XX_BLACK);
  tft.setCursor((128 - strlen(msg) * 6) / 2, 150);
  tft.print(msg);
}


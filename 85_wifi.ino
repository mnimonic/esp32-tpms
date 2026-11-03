// ---------------- WiFi settings page ----------------
// Menu -> "WiFi setup": the board opens a WiFi hotspot (WIFI_AP_SSID / WIFI_AP_PASS)
// and serves a settings page at http://192.168.4.1 (web_page.h). A click on the
// button, or WIFI_IDLE_OFF_MS without page activity, switches WiFi off again.
// BLE keeps scanning while WiFi is on.

static void wifiTouch() { wifiLastUse = millis(); }

// ---- JSON helpers ----
static void jsonArr(String& j, const char* key, const uint8_t* v, int n) {
  j += '"'; j += key; j += "\":[";
  for (int i = 0; i < n; i++) { if (i) j += ','; j += v[i]; }
  j += "],";
}
static void jsonArr16(String& j, const char* key, const uint16_t* v, int n) {
  j += '"'; j += key; j += "\":[";
  for (int i = 0; i < n; i++) { if (i) j += ','; j += v[i]; }
  j += "],";
}
static void jsonNames(String& j, const char* key, const char* const* v, int n) {
  j += '"'; j += key; j += "\":[";
  for (int i = 0; i < n; i++) { if (i) j += ','; j += '"'; j += v[i]; j += '"'; }
  j += "],";
}
static void jsonInt(String& j, const char* key, int v)   { j += '"'; j += key; j += "\":"; j += v; j += ','; }
static void jsonBool(String& j, const char* key, bool v) { j += '"'; j += key; j += "\":"; j += v ? "true" : "false"; j += ','; }

// ---- HTTP handlers ----
static void webRoot() {
  wifiTouch();
  web.send_P(200, "text/html", WEB_PAGE);
}

static void webConfig() {
  wifiTouch();
  String j; j.reserve(1200);
  j = "{\"version\":\"" FW_VERSION "\",";
  jsonBool(j, "psi", usePsi);
  jsonBool(j, "degf", useFahrenheit);
  jsonBool(j, "rot", rotate180);
  j += "\"positions\":[";
  for (int i = 0; i < NUM_POSITIONS; i++) {
    if (i) j += ',';
    j += "{\"name\":\""; j += POSITION_NAMES[i];
    j += "\",\"min\":";  j += String(positions[i].minBar, 3);
    j += ",\"max\":";    j += String(positions[i].maxBar, 3);
    j += '}';
  }
  j += "],";
  jsonInt(j, "tmax", tmaxIdx);       jsonArr(j, "tmaxOpts", TMAX_C, NUM_TMAX);
  jsonInt(j, "stale", staleIdx);     jsonArr(j, "staleOpts", STALE_MIN, NUM_STALE);
  jsonInt(j, "buzz", buzzIdx);       jsonNames(j, "buzzOpts", BUZZ_NAMES, NUM_BUZZ);
  jsonInt(j, "layout", layoutIdx);   jsonNames(j, "layoutOpts", LAYOUT_NAMES, NUM_LAYOUTS);
  jsonInt(j, "colors", paletteIdx);
  j += "\"colorOpts\":[";
  for (int i = 0; i < NUM_PALETTES; i++) { if (i) j += ','; j += '"'; j += PALETTES[i].name; j += '"'; }
  j += "],";
  jsonInt(j, "bright", brightIdx);   jsonArr(j, "brightOpts", BRIGHT_PCT, NUM_OPTS);
  jsonInt(j, "dim", dimIdx);         jsonArr16(j, "dimOpts", DIM_S, NUM_OPTS);
  jsonInt(j, "diml", dimLvlIdx);     jsonArr(j, "dimlOpts", DIM_LVL_PCT, NUM_DIM_LVLS);
  jsonInt(j, "off", screenOffIdx);   jsonArr16(j, "offOpts", SCREEN_OFF_S, NUM_OPTS);
  jsonInt(j, "scan", scanIdx);       jsonArr(j, "scanOpts", SCAN_DUTY, NUM_OPTS);
  j.remove(j.length() - 1);          // trailing comma
  j += '}';
  web.send(200, "application/json", j);
}

static void webStatus() {
  wifiTouch();
  String j; j.reserve(600);
  j = "{";
  jsonBool(j, "psi", usePsi);
  jsonBool(j, "degf", useFahrenheit);
  j += "\"positions\":[";
  for (int i = 0; i < NUM_POSITIONS; i++) {
    const Position& p = positions[i];
    if (i) j += ',';
    j += "{\"name\":\""; j += POSITION_NAMES[i]; j += "\",";
    jsonBool(j, "paired", p.addr.length() > 0);
    jsonBool(j, "has", p.hasReading);
    if (p.hasReading) {
      j += "\"state\":\""; j += stateText(positionState(i)); j += "\",";
      j += "\"bar\":";   j += String(p.last.psi * 0.0689476f, 3); j += ',';
      j += "\"temp\":";  j += String(p.last.tempC, 1); j += ',';
      j += "\"volts\":"; j += String(p.last.volts, 2); j += ',';
      jsonInt(j, "batt", p.last.batteryPct);
      jsonInt(j, "rssi", p.last.rssi);
      jsonInt(j, "age", (millis() - p.last.ms) / 1000);
    }
    j.remove(j.length() - 1);
    j += '}';
  }
  j += "]}";
  web.send(200, "application/json", j);
}

// Index argument: keep the current value if missing or out of range
static int argIdx(const char* name, int n, int cur) {
  if (!web.hasArg(name)) return cur;
  int v = web.arg(name).toInt();
  return (v >= 0 && v < n) ? v : cur;
}

static void webSettings() {
  wifiTouch();
  // Validate all MIN/MAX first, so nothing is changed on an error
  float mn[NUM_POSITIONS], mx[NUM_POSITIONS];
  for (int i = 0; i < NUM_POSITIONS; i++) {
    mn[i] = positions[i].minBar;
    mx[i] = positions[i].maxBar;
    String kMin = "min" + String(i), kMax = "max" + String(i);
    if (web.hasArg(kMin)) mn[i] = web.arg(kMin).toFloat();
    if (web.hasArg(kMax)) mx[i] = web.arg(kMax).toFloat();
    if (mn[i] < LIMIT_RANGE_MIN - 0.001f || mx[i] > LIMIT_RANGE_MAX + 0.001f || mn[i] >= mx[i]) {
      char err[96];
      snprintf(err, sizeof(err), "%s: MIN must be below MAX, within %.1f-%.1f bar",
               POSITION_NAMES[i], LIMIT_RANGE_MIN, LIMIT_RANGE_MAX);
      web.send(400, "text/plain", err);
      return;
    }
  }
  for (int i = 0; i < NUM_POSITIONS; i++) { positions[i].minBar = mn[i]; positions[i].maxBar = mx[i]; }

  int oldScan = scanIdx;
  usePsi        = argIdx("psi", 2, usePsi) == 1;
  useFahrenheit = argIdx("degf", 2, useFahrenheit) == 1;
  rotate180     = argIdx("rot", 2, rotate180) == 1;
  tmaxIdx       = argIdx("tmax", NUM_TMAX, tmaxIdx);
  staleIdx      = argIdx("stale", NUM_STALE, staleIdx);
  buzzIdx       = argIdx("buzz", NUM_BUZZ, buzzIdx);
  layoutIdx     = argIdx("layout", NUM_LAYOUTS, layoutIdx);
  paletteIdx    = argIdx("colors", NUM_PALETTES, paletteIdx);
  brightIdx     = argIdx("bright", NUM_OPTS, brightIdx);
  dimIdx        = argIdx("dim", NUM_OPTS, dimIdx);
  dimLvlIdx     = argIdx("diml", NUM_DIM_LVLS, dimLvlIdx);
  screenOffIdx  = argIdx("off", NUM_OPTS, screenOffIdx);
  scanIdx       = argIdx("scan", NUM_OPTS, scanIdx);

  saveLimits();
  setBacklightPct(BRIGHT_PCT[brightIdx]);
  applyRotation(rotate180);
  if (scanIdx != oldScan) applyScanSettings();
  displayDirty = true;
  drawWifiScreen();
  Serial.println(">> WiFi: settings saved from the web page");
  web.send(200, "text/plain", "ok");
}

static void webResetPairing() {
  wifiTouch();
  web.send(200, "text/plain", "ok");
  clearPositions();                    // shows the PAIRING RESET screen
  drawWifiScreen();
}

static void webFactoryReset() {
  web.send(200, "text/plain", "ok");
  delay(300);                          // let the response go out
  factoryReset();                      // restarts the board
}

static void webNotFound() {            // any other URL -> settings page
  web.sendHeader("Location", "/");
  web.send(302, "text/plain", "");
}

// ---- Screen shown while WiFi is on ----
static void drawWifiScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 128, 16, 0x3B9F);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(4, 4);
  tft.print("WiFi SETUP");
  tft.setTextColor(0x7BEF);
  tft.setCursor(4, 24);  tft.print("Network:");
  tft.setCursor(4, 50);  tft.print("Password:");
  tft.setCursor(4, 76);  tft.print("Open in browser:");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 35);  tft.print(WIFI_AP_SSID);
  tft.setCursor(4, 61);  tft.print(WIFI_AP_PASS);
  tft.setCursor(4, 87);  tft.print(WiFi.softAPIP());
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 128); tft.print("Click = stop WiFi");
  tft.setTextColor(0x7BEF);
  tft.setCursor(4, 140); tft.printf("Off after %lu min idle", (unsigned long)(WIFI_IDLE_OFF_MS / 60000));
  drawWifiClients(true);
}

static void drawWifiClients(bool force) {
  static int shown = -1;
  int n = WiFi.softAPgetStationNum();
  if (!force && n == shown) return;
  shown = n;
  tft.setTextSize(1);
  tft.setTextColor(n ? ST77XX_GREEN : ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(4, 106);
  tft.printf("Phones connected: %d ", n);
}

// ---- Start / stop / loop ----
static void wifiStart() {
  if (wifiActive) return;
  static bool routes = false;
  if (!routes) {
    web.on("/", HTTP_GET, webRoot);
    web.on("/api/config", HTTP_GET, webConfig);
    web.on("/api/status", HTTP_GET, webStatus);
    web.on("/api/settings", HTTP_POST, webSettings);
    web.on("/api/reset_pairing", HTTP_POST, webResetPairing);
    web.on("/api/factory_reset", HTTP_POST, webFactoryReset);
    web.onNotFound(webNotFound);
    routes = true;
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  web.begin();
  wifiActive = true;
  wifiTouch();
  setScreenState(SCR_ON);
  drawWifiScreen();
  Serial.printf(">> WiFi on: network \"%s\", open http://%s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void wifiStop() {
  if (!wifiActive) return;
  web.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiActive = false;
  displayDirty = true;
  lastActivity = millis();
  Serial.println(">> WiFi off");
}

// Called from loop()
static void wifiLoop() {
  if (!wifiActive) return;
  web.handleClient();
  drawWifiClients(false);
  if (WiFi.softAPgetStationNum() > 0) {
    static uint32_t lastSeen = 0;      // a connected phone also counts as activity (once a minute)
    if (millis() - lastSeen > 60000) { lastSeen = millis(); wifiTouch(); }
  }
  if (millis() - wifiLastUse > WIFI_IDLE_OFF_MS) wifiStop();
}


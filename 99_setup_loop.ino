// ---------------- Setup / loop ----------------
void setup() {
  setCpuFrequencyMhz(CPU_MHZ);
  Serial.begin(115200);
  // Never block on serial output: if USB is connected but no serial monitor is
  // reading, prints would otherwise stall loop() (button + screen freeze).
  Serial.setTxTimeoutMs(0);
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) delay(10);
  delay(500);
  Serial.println();
  Serial.println("=== ESP-TPMS (ESP32-C3) v" FW_VERSION " ===");
  Serial.printf("Last reset: %s%s\n", resetReasonText(esp_reset_reason()),
                isAbnormalReset(esp_reset_reason()) ? "  <-- ABNORMAL" : "");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  readingQueue = xQueueCreate(20, sizeof(Reading));
  initDisplay();

  loadPositions();
  setBacklightPct(BRIGHT_PCT[brightIdx]);
  buzzerInit();
  BUZZ(PAT_CHIRP);                               // short start-up chirp = buzzer works
  if (rotate180) { applyRotation(true); drawSplash(); }   // redraw splash upside down
  lastActivity = millis();
  Serial.printf("CPU %lu MHz, scan %u%%, brightness %u%%, dim %us to %u%%, screen off %us\n",
                (unsigned long)getCpuFrequencyMhz(), SCAN_DUTY[scanIdx], BRIGHT_PCT[brightIdx],
                DIM_S[dimIdx], DIM_LVL_PCT[dimLvlIdx], SCREEN_OFF_S[screenOffIdx]);
  printPositions();
  for (int i = 0; i < NUM_POSITIONS; i++) {
    if (!positions[i].addr.length()) {
      Serial.printf("Wake the %s sensor, then press the button (GPIO%d) to store it.\n",
                    POSITION_NAMES[i], BUTTON_PIN);
      break;
    }
  }

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);  // report duplicates
  scan->setActiveScan(true);
  applyScanSettings();                           // interval 100 ms, window = duty %

  startWatchdog();
  Serial.printf("Watchdog: %lu s\n", (unsigned long)(WDT_TIMEOUT_MS / 1000));
}

void loop() {
  // Diagnostics: longest single loop() pass since the last status print
  static uint32_t loopStart = 0, maxLoopMs = 0;
  uint32_t nowMs = millis();
  if (loopStart && nowMs - loopStart > maxLoopMs) maxLoopMs = nowMs - loopStart;
  loopStart = nowMs;

  esp_task_wdt_reset();                // feed the watchdog: "loop is still running"
#ifdef WDT_TEST
  if (millis() > 20000) { Serial.println("WDT_TEST: hanging on purpose..."); while (true) {} }
#endif
  processReadings();
  handleButton();
  buzzerUpdate();
  buzzerReminder();
  updateAlerts();
  manageScreen();
  updateDisplay();

  static uint32_t lastStatus = 0;
  if (TPMS_DEBUG && millis() - lastStatus >= STATUS_EVERY_MS) {
    lastStatus = millis();
    Serial.printf("[%8lu ms] alive - %lu BLE adverts received, slowest loop %lu ms\n",
                  millis(), (unsigned long)advCount, (unsigned long)maxLoopMs);
    maxLoopMs = 0;
    printPositions();
  }
  delay(10);
}

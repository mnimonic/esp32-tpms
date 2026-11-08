// ---------------- Reading handling ----------------
static void printReading(const Reading& r, int pos) {
  Serial.printf("[%8lu ms] ", r.ms);
  if (pos >= 0) Serial.printf("%-6s ", POSITION_NAMES[pos]);
  else          Serial.print("NEW    ");
  Serial.printf("%s  %.2f bar / %.1f psi  %.0f C", r.addr, r.psi * 0.0689476f, r.psi, r.tempC);
  if (r.volts >= 0)      Serial.printf("  %.1f V", r.volts);
  if (r.batteryPct >= 0) Serial.printf("  %d%%", r.batteryPct);
  Serial.printf("  st 0x%02X  RSSI %d  [%s]\n", r.status, r.rssi, r.type);

  if (PRINT_RAW) {
    Serial.print("           raw:");
    for (int i = 0; i < r.rawLen; i++) Serial.printf(" %02X", r.raw[i]);
    Serial.println();
  }
  if (pos < 0) Serial.println("           -> press the button to store this sensor");
}

static void processReadings() {
  Reading r;
  while (xQueueReceive(readingQueue, &r, 0) == pdTRUE) {
    // Same sensor + same bytes shortly after the previous one = repeat inside a burst
    bool repeat = haveLastDetected &&
                  strcmp(r.addr, lastDetected.addr) == 0 &&
                  r.rawLen == lastDetected.rawLen &&
                  memcmp(r.raw, lastDetected.raw, r.rawLen) == 0 &&
                  r.ms - lastDetected.ms < BURST_DEDUP_MS;

    lastDetected = r;
    haveLastDetected = true;

    int pos = findPosition(r.addr);
    if (pos >= 0) {
      if (positions[pos].restored)
        Serial.printf(">> %s: first fresh reading this ride\n", POSITION_NAMES[pos]);
      positions[pos].last = r; positions[pos].hasReading = true; positions[pos].restored = false;
    }

    if (TPMS_DEBUG && !repeat) printReading(r, pos);

    if (pos >= 0 && !repeat) {
      addPressureSample(pos, r.psi * 0.0689476f, r.ms);
      logReading(pos, r);
      maybeSaveLastReading(pos, r);
      if (isLowBattery(r) && !lowBattWarned[pos]) {
        lowBattWarned[pos] = true;
        Serial.printf("!! %s sensor battery LOW (%.1f V)\n", POSITION_NAMES[pos], r.volts);
        logLowBattery(pos, r);
      }
    }

    // New data from a paired sensor (or a sensor that can be paired) resets the dim/off timers
    // (other people's sensors nearby are ignored unless we are still pairing)
    if (pos >= 0 || nextFreePosition() >= 0) lastActivity = millis();
  }
}


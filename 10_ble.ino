// ---------------- Decoders ----------------
static uint32_t readU32LE(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool decodeBR(const uint8_t* d, size_t len, Reading& r) {
  if (len != 7) return false;
  uint16_t raw = ((uint16_t)d[3] << 8) | d[4];
  r.status     = d[0];
  r.volts      = d[1] / 10.0f;
  r.batteryPct = -1;
  r.tempC      = (int8_t)d[2];
  r.psi        = ((int)raw - 145) / 10.0f;
  if (r.psi < 0) r.psi = 0;
  strcpy(r.type, "BR");
  return true;
}

static bool decodeFBB0(const uint8_t* d, size_t len, Reading& r) {
  if (len != 18 || d[0] != 0x00 || d[1] != 0x01) return false;
  r.psi        = readU32LE(&d[8]) / 1000.0f * 0.1450377f;
  r.tempC      = (int32_t)readU32LE(&d[12]) / 100.0f;
  r.batteryPct = d[16];
  r.volts      = -1;
  r.status     = d[17];
  strcpy(r.type, "FBB0");
  return true;
}

// ---------------- BLE scan callback (runs in the BLE task) ----------------
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    advCount++;
    if (!dev->haveManufacturerData()) return;

    bool candidate = dev->isAdvertisingService(BR_SERVICE_UUID) ||
                     dev->isAdvertisingService(FBB0_SERVICE_UUID) ||
                     (dev->haveName() && dev->getName() == "BR");
    std::string md = dev->getManufacturerData();
    if (!candidate && md.size() != 18) return;

    Reading r = {};
    const uint8_t* d = (const uint8_t*)md.data();
    if (!decodeBR(d, md.size(), r) && !decodeFBB0(d, md.size(), r)) return;

    strncpy(r.addr, dev->getAddress().toString().c_str(), sizeof(r.addr) - 1);
    r.rawLen = min(md.size(), sizeof(r.raw));
    memcpy(r.raw, d, r.rawLen);
    r.rssi = dev->getRSSI();
    r.ms   = millis();
    xQueueSend(readingQueue, &r, 0);   // hand over to loop(); drop if queue is full
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    if (!scanReconfig) NimBLEDevice::getScan()->start(0, false, true);
  }
};
static ScanCallbacks scanCallbacks;

// (Re)start scanning with the selected duty cycle: listen <duty> ms out of every 100 ms
static void applyScanSettings() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scanReconfig = true;
  if (scan->isScanning()) scan->stop();
  scan->setInterval(100);
  uint8_t duty = SCAN_DUTY[scanIdx];
  if (wifiActive && duty > WIFI_BLE_DUTY) duty = WIFI_BLE_DUTY;   // leave airtime for WiFi
  scan->setWindow(duty);
  scan->start(0, false, true);
  scanReconfig = false;
  if (TPMS_DEBUG) Serial.printf(">> BLE scan: listening %u%% of the time\n", duty);
}


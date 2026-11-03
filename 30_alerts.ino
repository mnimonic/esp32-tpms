// ---------------- Rapid pressure loss / battery ----------------
static void resetHistory(int i) {
  memset(&hist[i], 0, sizeof(hist[i]));
  lowBattWarned[i] = false;
}

// Called for every new (non-repeat) reading of a paired sensor
static void addPressureSample(int i, float bar, uint32_t ms) {
  PressHist& h = hist[i];

  // Highest pressure within the window (including the current reading)
  float maxBar = bar;
  for (int k = 0; k < h.count; k++) {
    int idx = (h.head - 1 - k + HIST_N) % HIST_N;
    if (ms - h.ms[idx] > RAPID_WINDOW_MS) break;     // older samples are outside the window
    if (h.bar[idx] > maxBar) maxBar = h.bar[idx];
  }

  bool drop = (maxBar - bar) >= RAPID_DROP_BAR;
  if (drop && !h.dropping) {
    h.dropFrom = maxBar;
    Serial.printf("!! %s: RAPID PRESSURE LOSS %.2f -> %.2f bar within %lu min\n",
                  POSITION_NAMES[i], maxBar, bar, (unsigned long)(RAPID_WINDOW_MS / 60000));
  } else if (!drop && h.dropping) {
    Serial.printf(">> %s: pressure drop alert cleared (%.2f bar)\n", POSITION_NAMES[i], bar);
  }
  h.dropping = drop;

  // Store a history sample at most every HIST_EVERY_MS
  int last = (h.head - 1 + HIST_N) % HIST_N;
  if (h.count == 0 || ms - h.ms[last] >= HIST_EVERY_MS) {
    h.bar[h.head] = bar;
    h.ms[h.head]  = ms;
    h.head = (h.head + 1) % HIST_N;
    if (h.count < HIST_N) h.count++;
  }
}

static bool isLowBattery(const Reading& r) {
  if (r.volts >= 0)      return r.volts < LOW_BATT_V;
  if (r.batteryPct >= 0) return r.batteryPct < LOW_BATT_PCT;
  return false;
}


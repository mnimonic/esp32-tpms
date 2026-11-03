// ---------------- Buzzer ----------------
// Non-blocking beep patterns on BUZZER_PIN. A pattern is a list of durations
// in ms: on, off, on, off, ... (an odd count ends with "on").

static const uint16_t PAT_CHIRP[]  = {60};                              // start-up check
static const uint16_t PAT_CLICK[]  = {25};                              // button tick
static const uint16_t PAT_TEST[]   = {120, 100, 120};                   // menu test beep
static const uint16_t PAT_WARN[]   = {150, 150, 150};                   // LOW / HIGH: 2 short
static const uint16_t PAT_DANGER[] = {400, 200, 400, 200, 400};         // DROPPING / HOT: 3 long

static const uint16_t* buzPattern = nullptr;
static int      buzLen = 0, buzStep = 0;
static uint32_t buzStepStart = 0;

static void buzzerOutput(bool on) {
  if (BUZZER_PIN < 0) return;
  if (BUZZER_PASSIVE) ledcWriteTone(BUZZER_PIN, on ? BUZZER_TONE_HZ : 0);
  else                digitalWrite(BUZZER_PIN, (on != BUZZER_ACTIVE_LOW) ? HIGH : LOW);
}

static void buzzerInit() {
  if (BUZZER_PIN < 0) return;
  if (BUZZER_PASSIVE) ledcAttach(BUZZER_PIN, BUZZER_TONE_HZ, 8);
  else                pinMode(BUZZER_PIN, OUTPUT);
  buzzerOutput(false);
}

// Start a pattern (replaces one that is playing). force = play even when the buzzer is Off (menu test)
static void buzzerPlay(const uint16_t* pat, int len, bool force) {
  if (BUZZER_PIN < 0 || (!force && buzzIdx == BUZZ_OFF)) return;
  buzPattern = pat;
  buzLen = len;
  buzStep = 0;
  buzStepStart = millis();
  buzzerOutput(true);
}
#define BUZZ(p) buzzerPlay(p, sizeof(p) / sizeof(p[0]), false)

// Called from loop(): advances the pattern
static void buzzerUpdate() {
  if (!buzPattern) return;
  if (millis() - buzStepStart < buzPattern[buzStep]) return;
  buzStep++;
  buzStepStart = millis();
  if (buzStep >= buzLen) { buzPattern = nullptr; buzzerOutput(false); return; }
  buzzerOutput(buzStep % 2 == 0);           // even steps = on, odd = off
}

// Sound for an alert kind
static void buzzerAlert(PosState st) {
  if (st == PS_DROP || st == PS_HOT) BUZZ(PAT_DANGER);
  else                               BUZZ(PAT_WARN);
}

// Button feedback (only in "Alerts+click" mode)
static void buzzerClick() {
  if (buzzIdx == BUZZ_ALERTS_CLICK) BUZZ(PAT_CLICK);
}

// Repeat the alert sound every BUZZ_REPEAT_MS while an alert number is still blinking
static void buzzerReminder() {
  static uint32_t lastReminder = 0;
  int worst = -1;
  for (int i = 0; i < NUM_POSITIONS; i++) {
    if (!isBlinking(i)) continue;
    PosState st = positionState(i);
    if (worst < 0 || st == PS_DROP || st == PS_HOT) worst = i;
  }
  if (worst < 0) { lastReminder = millis(); return; }
  if (millis() - lastReminder >= BUZZ_REPEAT_MS) {
    lastReminder = millis();
    buzzerAlert(positionState(worst));
  }
}


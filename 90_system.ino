// ---------------- Watchdog / reset reason ----------------
// Why did the board start? Abnormal causes are logged and shown on the splash.
static const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "low voltage";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_USB:       return "USB reset";
    case ESP_RST_EXT:       return "reset pin";
    default:                return "unknown";
  }
}

static bool isAbnormalReset(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT   || r == ESP_RST_BROWNOUT;
}

static void startWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;          // only watch loop(), not the idle task
  cfg.trigger_panic  = true;       // restart when it fires
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
  esp_task_wdt_add(NULL);          // NULL = this task (the Arduino loop task)
}


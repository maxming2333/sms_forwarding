#include "Scheduler.h"
#include "config/AppConfig.h"
#include "sim/SimManager.h"
#include <esp_task_wdt.h>
#include <time.h>

void checkScheduledReboot() {
  if (!config.autoRebootEnabled) return;

  struct tm ti;
  if (!getLocalTime(&ti) || ti.tm_year < (2020 - 1900)) {
    // Time not synced yet — attempt re-sync periodically
    static unsigned long lastSyncTry = 0;
    if (millis() - lastSyncTry > 300000UL) {
      lastSyncTry = millis();
      Serial.println("[Scheduler] 时间未同步，重试NTP...");
      configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");
    }
    return;
  }

  // Parse "HH:MM" target time
  int colonIdx = config.autoRebootTime.indexOf(':');
  if (colonIdx == -1) return;
  int targetHour = config.autoRebootTime.substring(0, colonIdx).toInt();
  int targetMin  = config.autoRebootTime.substring(colonIdx + 1).toInt();

  // Trigger once per minute window (within first 5 seconds of the target minute)
  if (ti.tm_hour == targetHour && ti.tm_min == targetMin && ti.tm_sec < 5) {
    Serial.println("[Scheduler] ⏰ 触发定时重启 (" + config.autoRebootTime + ")");
    // Feed watchdog before the potentially-slow resetModule()
    esp_task_wdt_reset();
    resetModule();
    Serial.println("[Scheduler] 重启ESP32...");
    delay(1000);
    ESP.restart();
  }
}


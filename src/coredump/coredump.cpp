#include "coredump.h"
#include "common/nvs_helper.h"
#include "../logger/logger.h"
#include "ota/ota_manager.h"
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>

// RTC memory: preserved across panic reboot, cleared on power loss.
RTC_DATA_ATTR static time_t s_rtcLastKnownTime = 0;

time_t Coredump::s_crashTime = 0;
String Coredump::s_crashVersion = "";

size_t Coredump::scanUsed(const esp_partition_t* part) {
  uint8_t buf[256];
  for (size_t offset = part->size; offset > 0;) {
    size_t chunk = (offset >= sizeof(buf)) ? sizeof(buf) : offset;
    offset -= chunk;
    if (esp_partition_read(part, offset, buf, chunk) != ESP_OK) {
      return 0;
    }
    for (int i = static_cast<int>(chunk) - 1; i >= 0; --i) {
      if (buf[i] != 0xFF) {
        return offset + static_cast<size_t>(i) + 1;
      }
    }
  }
  return 0;
}

const esp_partition_t* Coredump::partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

size_t Coredump::usedSize(const esp_partition_t* part) {
  return scanUsed(part);
}

void Coredump::updateLastKnownTime(time_t t) {
  s_rtcLastKnownTime = t;
  static time_t s_lastNvsWrite = 0;
  if (t - s_lastNvsWrite >= 60) {
    Nvs::putLong("sms_config", "cdLastTs", (long)t);
    s_lastNvsWrite = t;
  }
}

void Coredump::init() {
  if (s_rtcLastKnownTime == 0) {
    long saved = Nvs::getLong("sms_config", "cdLastTs", 0);
    if (saved > 0) {
      s_rtcLastKnownTime = (time_t)saved;
    }
  }

  if (time(nullptr) < 1577836800L && s_rtcLastKnownTime > 1577836800L) {
    struct timeval tv = { s_rtcLastKnownTime, 0 };
    settimeofday(&tv, nullptr);
    LOG("Time", "从 NVS 恢复系统时间（近似）: %ld", (long)s_rtcLastKnownTime);
  }

  long savedCrash = Nvs::getLong("sms_config", "cdCrashTs", 0);
  if (savedCrash > 0) {
    s_crashTime = (time_t)savedCrash;
  }
  s_crashVersion = Nvs::getStr("sms_config", "cdCrashVer", "");

  if (esp_reset_reason() == ESP_RST_PANIC && s_rtcLastKnownTime > 0) {
    s_crashTime    = s_rtcLastKnownTime;
    s_crashVersion = Ota::version();
    NvsScope p("sms_config", false);
    if (p.ok()) {
      p->putLong("cdCrashTs", (long)s_crashTime);
      p->putString("cdCrashVer", s_crashVersion);
    }
    LOG("Coredump", "检测到 panic 重启，崩溃时间: %ld，版本: %s", (long)s_crashTime, s_crashVersion.c_str());
  }
}

bool Coredump::hasData() {
  const esp_partition_t* part = partition();
  if (!part) {
    return false;
  }
  return scanUsed(part) > 0;
}

time_t Coredump::crashTime() { return s_crashTime; }
String Coredump::crashVersion() { return s_crashVersion; }

String Coredump::elfUrl() {
  if (s_crashVersion.length() == 0) {
    return "";
  }
  return String(OTA_RELEASES_BASE_URL) + "/download/" + s_crashVersion + "/firmware-" + s_crashVersion + ".elf";
}

#include "time_sync.h"
#include "../logger/logger.h"
#include "sim/sim_dispatcher.h"
#include <time.h>
#include <sys/time.h>

bool          TimeSync::s_synced = false;
unsigned long TimeSync::s_simRetryNext = 0;

void TimeSync::init() {
  setenv("TZ", "UTC-8", 1);
  tzset();
  LOG("Time", "时间模块已初始化（默认时区 UTC+8）");
}

void TimeSync::syncFromSIM() {
  String resp;
  bool ok = SimDispatcher::sendCommand("AT+CCLK?", 3000, &resp, false);
  if (!ok && resp.indexOf("+CCLK:") < 0) {
    LOG("Time", "AT+CCLK? 无响应，跳过 SIM 时间同步");
    return;
  }

  int cclkIdx = resp.indexOf("+CCLK:");
  int q1 = resp.indexOf('"', cclkIdx);
  int q2 = resp.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 <= q1) {
    LOG("Time", "CCLK 格式解析失败");
    return;
  }
  String ts = resp.substring(q1 + 1, q2);

  int yy, mo, dd, hh, mi, ss, tz = 0;
  int parsed = sscanf(ts.c_str(), "%d/%d/%d,%d:%d:%d%d", &yy, &mo, &dd, &hh, &mi, &ss, &tz);
  if (parsed < 6) {
    LOG("Time", "CCLK 时间字段解析失败，原始: %s", ts.c_str());
    return;
  }

  if (yy < 0 || yy > 99 || mo < 1 || mo > 12 || dd < 1 || dd > 31 ||
      hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) {
    LOG("Time", "CCLK 时间字段越界，原始: %s", ts.c_str());
    return;
  }
  if (tz < -56 || tz > 56) {
    LOG("Time", "CCLK 时区字段越界 (%d)，按 0 处理", tz);
    tz = 0;
  }

  struct tm t = {};
  t.tm_year = yy + 100;
  t.tm_mon  = mo - 1;
  t.tm_mday = dd;
  t.tm_hour = hh;
  t.tm_min  = mi;
  t.tm_sec  = ss;

  time_t localTime = mktime(&t);
  time_t utcTime   = localTime - (time_t)(tz * 15 * 60);

  struct timeval tv = { utcTime, 0 };
  settimeofday(&tv, nullptr);

  int tzHours = tz / 4;
  char tzStr[16];
  snprintf(tzStr, sizeof(tzStr), "UTC%+d", -tzHours);
  setenv("TZ", tzStr, 1);
  tzset();

  s_synced = true;
  LOG("Time", "SIM时间同步成功: %s, 时区偏移: %d (UTC%+d)", ts.c_str(), tz, tzHours);
}

void TimeSync::syncNTP() {
  configTime(8 * 3600, 0, "pool.ntp.org", "ntp.ntsc.ac.cn", "ntp.aliyun.com");
  setenv("TZ", "UTC-8", 1);
  tzset();

  int retries = 0;
  while (time(nullptr) < 1000000 && retries < 100) {
    delay(100);
    retries++;
  }
  if (time(nullptr) >= 1000000) {
    s_synced = true;
    LOG("Time", "NTP时间同步成功，UTC+8: %ld", (long)time(nullptr));
  } else {
    LOG("Time", "NTP时间同步超时");
  }
}

bool TimeSync::isSynced() { return s_synced; }

void TimeSync::tick() {
  if (s_synced) {
    return;
  }
  if (millis() < s_simRetryNext) {
    return;
  }
  LOG("Time", "SIM 时间重试...");
  syncFromSIM();
  if (time(nullptr) >= 1000000) {
    s_synced = true;
    LOG("Time", "SIM 时间重试成功，停止重试");
  } else {
    s_simRetryNext = millis() + 10000;
  }
}

String TimeSync::dateStr() {
  time_t now = time(nullptr);
  if (now < 1000000) {
    return "未知";
  }
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S%z", &t);
  return String(buf);
}

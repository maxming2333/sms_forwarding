#include "config.h"
#include <Preferences.h>
#include "logger.h"
#include <time.h>

#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

Config config;
RebootSchedule rebootSchedule;

static Preferences preferences;

void loadConfig() {
  preferences.begin("sms_config", false);
  config.smtpServer = preferences.isKey("smtpServer") ? preferences.getString("smtpServer", "") : "";
  config.smtpPort   = preferences.isKey("smtpPort")   ? preferences.getInt("smtpPort", 465)     : 465;
  config.smtpUser   = preferences.isKey("smtpUser")   ? preferences.getString("smtpUser", "")   : "";
  config.smtpPass   = preferences.isKey("smtpPass")   ? preferences.getString("smtpPass", "")   : "";
  config.smtpSendTo = preferences.isKey("smtpSendTo") ? preferences.getString("smtpSendTo", "") : "";
  config.adminPhone = preferences.isKey("adminPhone") ? preferences.getString("adminPhone", "") : "";
  config.webUser    = preferences.isKey("webUser")    ? preferences.getString("webUser", DEFAULT_WEB_USER) : DEFAULT_WEB_USER;
  config.webPass    = preferences.isKey("webPass")    ? preferences.getString("webPass", DEFAULT_WEB_PASS) : DEFAULT_WEB_PASS;

  config.pushCount = preferences.isKey("pushCount") ? (int)preferences.getUChar("pushCount", 5) : 5;
  if (config.pushCount < 1) config.pushCount = 1;
  if (config.pushCount > MAX_PUSH_CHANNELS) config.pushCount = MAX_PUSH_CHANNELS;

  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    config.pushChannels[i].enabled    = preferences.getBool((prefix + "en").c_str(), false);
    config.pushChannels[i].type       = (PushType)preferences.getUChar((prefix + "type").c_str(), PUSH_TYPE_POST_JSON);
    config.pushChannels[i].url        = preferences.isKey((prefix + "url").c_str())  ? preferences.getString((prefix + "url").c_str(),  "")                    : "";
    config.pushChannels[i].name       = preferences.isKey((prefix + "name").c_str()) ? preferences.getString((prefix + "name").c_str(), "通道" + String(i + 1)) : "通道" + String(i + 1);
    config.pushChannels[i].key1       = preferences.isKey((prefix + "k1").c_str())   ? preferences.getString((prefix + "k1").c_str(),   "")                    : "";
    config.pushChannels[i].key2       = preferences.isKey((prefix + "k2").c_str())   ? preferences.getString((prefix + "k2").c_str(),   "")                    : "";
    config.pushChannels[i].customBody = preferences.isKey((prefix + "body").c_str()) ? preferences.getString((prefix + "body").c_str(), "")                    : "";
    config.pushChannels[i].retryOnFail = preferences.getBool((prefix + "retry").c_str(), false);
  }

  // 兼容旧配置：迁移旧 httpUrl 到第一个通道
  String oldHttpUrl = preferences.isKey("httpUrl") ? preferences.getString("httpUrl", "") : "";
  if (oldHttpUrl.length() > 0 && !config.pushChannels[0].enabled) {
    config.pushChannels[0].enabled = true;
    config.pushChannels[0].url     = oldHttpUrl;
    config.pushChannels[0].type    = preferences.getUChar("barkMode", 0) != 0 ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
    config.pushChannels[0].name    = "迁移通道";
    LOG("Config", "已迁移旧HTTP配置到推送通道1");
  }

  config.simNotifyEnabled = preferences.isKey("simNotify") ? preferences.getBool("simNotify", false) : false;
  config.dataTraffic       = preferences.getBool("dataTraffic", false);

  // Multi-WiFi: 读取 wifiCount，如不存在则迁移旧单 WiFi 键（FR-017）
  if (preferences.isKey("wifiCount")) {
    config.wifiCount = (int)preferences.getUChar("wifiCount", 0);
    if (config.wifiCount < 0) config.wifiCount = 0;
    if (config.wifiCount > MAX_WIFI_ENTRIES) config.wifiCount = MAX_WIFI_ENTRIES;
    for (int i = 0; i < config.wifiCount; i++) {
      String ks = "wifi" + String(i) + "ssid";
      String kp = "wifi" + String(i) + "pass";
      config.wifiList[i].ssid     = preferences.isKey(ks.c_str()) ? preferences.getString(ks.c_str(), "") : "";
      config.wifiList[i].password = preferences.isKey(kp.c_str()) ? preferences.getString(kp.c_str(), "") : "";
    }
  } else {
    // 迁移旧单 WiFi 键
    String legacySsid = preferences.isKey("wifiSsid") ? preferences.getString("wifiSsid", "") : "";
    String legacyPass = preferences.isKey("wifiPass")  ? preferences.getString("wifiPass",  "") : "";
    if (legacySsid.length() > 0) {
      config.wifiList[0].ssid     = legacySsid;
      config.wifiList[0].password = legacyPass;
      config.wifiCount = 1;
      LOG("Config", "已迁移旧单WiFi配置到wifiList[0]");
    } else {
      config.wifiCount = 0;
    }
  }

  config.pushStrategy = (PushStrategy)(preferences.isKey("pushStrategy") ? preferences.getUChar("pushStrategy", 0) : 0);

  config.blacklistCount = preferences.getInt("blCount", 0);
  if (config.blacklistCount > MAX_BLACKLIST_ENTRIES) config.blacklistCount = MAX_BLACKLIST_ENTRIES;
  for (int i = 0; i < config.blacklistCount; i++) {
    String key = "bl" + String(i);
    config.blacklist[i] = preferences.getString(key.c_str(), "");
  }

  preferences.end();
  LOG("Config", "配置已加载");
}

void saveConfig() {
  preferences.begin("sms_config", false);
  preferences.putString("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort",      config.smtpPort);
  preferences.putString("smtpUser",   config.smtpUser);
  preferences.putString("smtpPass",   config.smtpPass);
  preferences.putString("smtpSendTo", config.smtpSendTo);
  preferences.putString("adminPhone", config.adminPhone);
  preferences.putString("webUser",    config.webUser);
  preferences.putString("webPass",    config.webPass);

  preferences.putUChar("pushCount", (uint8_t)config.pushCount);
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    preferences.putBool((prefix + "en").c_str(),   config.pushChannels[i].enabled);
    preferences.putUChar((prefix + "type").c_str(), (uint8_t)config.pushChannels[i].type);
    preferences.putString((prefix + "url").c_str(), config.pushChannels[i].url);
    preferences.putString((prefix + "name").c_str(), config.pushChannels[i].name);
    preferences.putString((prefix + "k1").c_str(),  config.pushChannels[i].key1);
    preferences.putString((prefix + "k2").c_str(),  config.pushChannels[i].key2);
    preferences.putString((prefix + "body").c_str(), config.pushChannels[i].customBody);
    preferences.putBool((prefix + "retry").c_str(), config.pushChannels[i].retryOnFail);
  }

  preferences.putBool("simNotify",    config.simNotifyEnabled);
  preferences.putBool("dataTraffic",  config.dataTraffic);

  preferences.putUChar("wifiCount", (uint8_t)config.wifiCount);
  for (int i = 0; i < config.wifiCount; i++) {
    String ks = "wifi" + String(i) + "ssid";
    String kp = "wifi" + String(i) + "pass";
    preferences.putString(ks.c_str(), config.wifiList[i].ssid);
    preferences.putString(kp.c_str(), config.wifiList[i].password);
  }

  preferences.putUChar("pushStrategy", (uint8_t)config.pushStrategy);

  preferences.putInt("blCount", config.blacklistCount);
  for (int i = 0; i < config.blacklistCount; i++) {
    String key = "bl" + String(i);
    preferences.putString(key.c_str(), config.blacklist[i]);
  }

  preferences.end();
  LOG("Config", "配置已保存");
}

void loadRebootSchedule(RebootSchedule& sched) {
  preferences.begin("reboot_cfg", false);
  sched.enabled   = preferences.getBool("rb_enabled", false);
  sched.mode      = preferences.getUChar("rb_mode", 0);
  sched.hour      = preferences.getUChar("rb_hour", 3);
  sched.minute    = preferences.getUChar("rb_minute", 0);
  sched.intervalH = preferences.getUShort("rb_interval", 24);
  preferences.end();
}

void saveRebootSchedule(const RebootSchedule& sched) {
  preferences.begin("reboot_cfg", false);
  preferences.putBool("rb_enabled",    sched.enabled);
  preferences.putUChar("rb_mode",      sched.mode);
  preferences.putUChar("rb_hour",      sched.hour);
  preferences.putUChar("rb_minute",    sched.minute);
  preferences.putUShort("rb_interval", sched.intervalH);
  preferences.end();
}

void rebootTick() {
  if (!rebootSchedule.enabled) return;

  if (rebootSchedule.mode == 1) {
    // 按间隔模式：millis() / 3600000 >= intervalH
    if ((millis() / 3600000UL) >= rebootSchedule.intervalH) {
      LOG("Config", "定时重启触发（间隔模式）");
      ESP.restart();
    }
  } else {
    // 每日定时模式：依赖 NTP
    time_t now = time(nullptr);
    if (now < 100000) {
      // NTP 未同步，跳过
      static unsigned long lastWarn = 0;
      if (millis() - lastWarn >= 60000) {
        lastWarn = millis();
        LOG("Config", "定时重启：NTP未同步，跳过检查");
      }
      return;
    }

    // 从 NVS 加载上次重启的 epoch（仅首次调用时读取，避免重启后 static 变量丢失）
    static uint32_t lastRebootEpoch = 0;
    static bool lastRebootLoaded = false;
    if (!lastRebootLoaded) {
      Preferences p;
      p.begin("reboot_cfg", true);
      lastRebootEpoch = p.getUInt("rb_last_epoch", 0);
      p.end();
      lastRebootLoaded = true;
    }

    struct tm t;
    localtime_r(&now, &t);

    // 计算今天目标时刻的 epoch
    struct tm trigger = t;
    trigger.tm_hour = rebootSchedule.hour;
    trigger.tm_min  = rebootSchedule.minute;
    trigger.tm_sec  = 0;
    time_t triggerEpoch = mktime(&trigger);

    // 当前时间在目标分钟窗口内，且本次触发窗口尚未重启过
    if (now >= triggerEpoch && now < triggerEpoch + 60 && lastRebootEpoch < (uint32_t)triggerEpoch) {
      // 重启前先持久化，防止重启后重复触发
      Preferences p;
      p.begin("reboot_cfg", false);
      p.putUInt("rb_last_epoch", (uint32_t)now);
      p.end();
      LOG("Config", "定时重启触发（每日模式）");
      ESP.restart();
    }
  }
}

void resetConfig() {
  Preferences p;
  p.begin("sms_config", false);
  p.clear();
  p.end();
  p.begin("reboot_cfg", false);
  p.clear();
  p.end();

  config = Config{};
  config.smtpPort  = 465;
  config.webUser   = DEFAULT_WEB_USER;
  config.webPass   = DEFAULT_WEB_PASS;
  config.pushCount = 5;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    config.pushChannels[i].name = "通道" + String(i + 1);
    config.pushChannels[i].type = PUSH_TYPE_POST_JSON;
  }
  config.wifiCount = 1;
  config.wifiList[0] = WifiEntry{"", ""};

  rebootSchedule = RebootSchedule{};
  rebootSchedule.hour      = 3;
  rebootSchedule.intervalH = 24;

  LOG("Config", "配置已重置为出厂默认值");
}

bool isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_DINGTALK:
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_CUSTOM:
      return ch.url.length() > 0;
    case PUSH_TYPE_PUSHPLUS:
    case PUSH_TYPE_SERVERCHAN:
      return ch.key1.length() > 0;
    case PUSH_TYPE_GOTIFY:
      return ch.url.length() > 0 && ch.key1.length() > 0;
    case PUSH_TYPE_TELEGRAM:
      return ch.key1.length() > 0 && ch.key2.length() > 0;
    case PUSH_TYPE_WECHAT_WORK:
      return ch.url.length() > 0;
    case PUSH_TYPE_SMS:
      return ch.url.length() > 0;
    default:
      return false;
  }
}

bool isConfigValid() {
  bool emailValid = config.smtpServer.length() > 0 &&
                    config.smtpUser.length() > 0   &&
                    config.smtpPass.length() > 0   &&
                    config.smtpSendTo.length() > 0;
  bool pushValid = false;
  for (int i = 0; i < config.pushCount; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushValid = true;
      break;
    }
  }
  return emailValid || pushValid;
}

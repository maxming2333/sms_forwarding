#include "config.h"
#include "common/nvs_helper.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include "../logger/logger.h"
#include <time.h>

static constexpr char kNvsSmsConfig[] = "sms_config";
static constexpr char kNvsRebootCfg[] = "reboot_cfg";

static String trimStr(const String& s) {
  String r = s;
  r.trim();
  return r;
}

Config config;
RebootSchedule rebootSchedule;

void ConfigStore::load() {
  NvsScope p(kNvsSmsConfig, false);
  if (!p.ok()) {
    LOG("CFG", "NVS 打开失败");
    return;
  }
  auto& prefs = p.p();

  config.adminPhone = prefs.isKey("adminPhone") ? prefs.getString("adminPhone", "") : "";
  config.webUser    = prefs.isKey("webUser")    ? prefs.getString("webUser", DEFAULT_WEB_USER) : DEFAULT_WEB_USER;
  config.webPass    = prefs.isKey("webPass")    ? prefs.getString("webPass", DEFAULT_WEB_PASS) : DEFAULT_WEB_PASS;

  config.pushCount = prefs.isKey("pushCount") ? (int)prefs.getUChar("pushCount", 5) : 5;
  config.pushCount = constrain(config.pushCount, 1, MAX_PUSH_CHANNELS);

  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    auto& ch = config.pushChannels[i];
    ch.enabled    = prefs.getBool((prefix + "en").c_str(), false);
    ch.type       = (PushType)prefs.getUChar((prefix + "type").c_str(), PUSH_TYPE_POST_JSON);
    ch.url        = prefs.isKey((prefix + "url").c_str())  ? prefs.getString((prefix + "url").c_str(),  "")                    : "";
    ch.name       = prefs.isKey((prefix + "name").c_str()) ? prefs.getString((prefix + "name").c_str(), "通道" + String(i + 1)) : "通道" + String(i + 1);
    ch.key1       = prefs.isKey((prefix + "k1").c_str())   ? prefs.getString((prefix + "k1").c_str(),   "")                    : "";
    ch.key2       = prefs.isKey((prefix + "k2").c_str())   ? prefs.getString((prefix + "k2").c_str(),   "")                    : "";
    ch.customBody = prefs.isKey((prefix + "body").c_str()) ? prefs.getString((prefix + "body").c_str(), "")                    : "";
    ch.retryOnFail = prefs.getBool((prefix + "retry").c_str(), false);
  }

  // Migrate legacy httpUrl into channel 0
  String oldHttpUrl = prefs.isKey("httpUrl") ? prefs.getString("httpUrl", "") : "";
  if (oldHttpUrl.length() > 0 && !config.pushChannels[0].enabled) {
    config.pushChannels[0].enabled = true;
    config.pushChannels[0].url     = oldHttpUrl;
    config.pushChannels[0].type    = prefs.getUChar("barkMode", 0) != 0 ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
    config.pushChannels[0].name    = "迁移通道";
    LOG("CFG", "已迁移旧HTTP配置到推送通道1");
  }

  config.simNotifyEnabled = prefs.isKey("simNotify") ? prefs.getBool("simNotify", false) : false;
  config.dataTraffic      = prefs.getBool("dataTraffic", false);
  config.logFileEnabled   = prefs.isKey("logFile") ? prefs.getBool("logFile", false) : false;

  if (prefs.isKey("wifiCount")) {
    config.wifiCount = constrain((int)prefs.getUChar("wifiCount", 0), 0, MAX_WIFI_ENTRIES);
    for (int i = 0; i < config.wifiCount; i++) {
      String ks = "wifi" + String(i) + "ssid";
      String kp = "wifi" + String(i) + "pass";
      config.wifiList[i].ssid     = prefs.isKey(ks.c_str()) ? prefs.getString(ks.c_str(), "") : "";
      config.wifiList[i].password = prefs.isKey(kp.c_str()) ? prefs.getString(kp.c_str(), "") : "";
    }
  } else {
    String legacySsid = prefs.isKey("wifiSsid") ? prefs.getString("wifiSsid", "") : "";
    String legacyPass = prefs.isKey("wifiPass") ? prefs.getString("wifiPass",  "") : "";
    if (legacySsid.length() > 0) {
      config.wifiList[0].ssid     = legacySsid;
      config.wifiList[0].password = legacyPass;
      config.wifiCount = 1;
      LOG("CFG", "已迁移旧单WiFi配置到wifiList[0]");
    } else {
      config.wifiCount = 0;
    }
  }

  config.pushStrategy = (PushStrategy)(prefs.isKey("pushStrategy") ? prefs.getUChar("pushStrategy", 0) : 0);
  config.remark       = prefs.isKey("remark") ? prefs.getString("remark", "") : "";

  config.blacklistCount = constrain(prefs.getInt("blCount", 0), 0, MAX_BLACKLIST_ENTRIES);
  for (int i = 0; i < config.blacklistCount; i++) {
    config.blacklist[i] = prefs.getString(("bl" + String(i)).c_str(), "");
  }

  LOG("CFG", "配置已加载");
}

void ConfigStore::save() {
  if (config.webUser.length() == 0) {
    config.webUser = DEFAULT_WEB_USER;
  }
  if (config.webPass.length() == 0) {
    config.webPass = DEFAULT_WEB_PASS;
  }
  config.pushCount      = constrain(config.pushCount, 1, MAX_PUSH_CHANNELS);
  config.wifiCount      = constrain(config.wifiCount, 0, MAX_WIFI_ENTRIES);
  config.blacklistCount = constrain(config.blacklistCount, 0, MAX_BLACKLIST_ENTRIES);
  if (config.pushStrategy != PUSH_STRATEGY_BROADCAST && config.pushStrategy != PUSH_STRATEGY_FAILOVER) {
    config.pushStrategy = PUSH_STRATEGY_BROADCAST;
  }

  NvsScope p(kNvsSmsConfig, false);
  if (!p.ok()) {
    return;
  }
  auto& prefs = p.p();

  prefs.putString("adminPhone", trimStr(config.adminPhone));
  prefs.putString("webUser",    trimStr(config.webUser));
  prefs.putString("webPass",    trimStr(config.webPass));

  prefs.putUChar("pushCount", (uint8_t)config.pushCount);
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    auto& ch = config.pushChannels[i];
    prefs.putBool((prefix + "en").c_str(),    ch.enabled);
    prefs.putUChar((prefix + "type").c_str(), (uint8_t)ch.type);
    prefs.putString((prefix + "url").c_str(),  trimStr(ch.url));
    prefs.putString((prefix + "name").c_str(), trimStr(ch.name));
    prefs.putString((prefix + "k1").c_str(),   trimStr(ch.key1));
    prefs.putString((prefix + "k2").c_str(),   trimStr(ch.key2));
    prefs.putString((prefix + "body").c_str(), ch.customBody);
    prefs.putBool((prefix + "retry").c_str(),  ch.retryOnFail);
  }

  prefs.putBool("simNotify",   config.simNotifyEnabled);
  prefs.putBool("dataTraffic", config.dataTraffic);
  prefs.putBool("logFile",     config.logFileEnabled);

  prefs.putUChar("wifiCount", (uint8_t)config.wifiCount);
  for (int i = 0; i < config.wifiCount; i++) {
    prefs.putString(("wifi" + String(i) + "ssid").c_str(), trimStr(config.wifiList[i].ssid));
    prefs.putString(("wifi" + String(i) + "pass").c_str(), trimStr(config.wifiList[i].password));
  }

  prefs.putUChar("pushStrategy", (uint8_t)config.pushStrategy);
  prefs.putString("remark", trimStr(config.remark).substring(0, 64));

  prefs.putInt("blCount", config.blacklistCount);
  for (int i = 0; i < config.blacklistCount; i++) {
    prefs.putString(("bl" + String(i)).c_str(), trimStr(config.blacklist[i]));
  }

  LOG("CFG", "配置已保存");
}

void ConfigStore::loadReboot(RebootSchedule& sched) {
  NvsScope p(kNvsRebootCfg, true);
  if (!p.ok()) {
    return;
  }
  sched.enabled   = p->getBool("rb_enabled", false);
  sched.mode      = p->getUChar("rb_mode", 0);
  sched.hour      = p->getUChar("rb_hour", 3);
  sched.minute    = p->getUChar("rb_minute", 0);
  sched.intervalH = p->getUShort("rb_interval", 24);
}

void ConfigStore::saveReboot(const RebootSchedule& sched) {
  RebootSchedule n = sched;
  if (n.mode > 1)    { n.mode = 0; }
  if (n.hour > 23)   { n.hour = 23; }
  if (n.minute > 59) { n.minute = 59; }
  n.intervalH = constrain((int)n.intervalH, 1, 168);

  NvsScope p(kNvsRebootCfg, false);
  if (!p.ok()) {
    return;
  }
  p->putBool("rb_enabled",    n.enabled);
  p->putUChar("rb_mode",      n.mode);
  p->putUChar("rb_hour",      n.hour);
  p->putUChar("rb_minute",    n.minute);
  p->putUShort("rb_interval", n.intervalH);
}

void ConfigStore::rebootTick() {
  if (!rebootSchedule.enabled) {
    return;
  }

  if (rebootSchedule.mode == 1) {
    if ((millis() / 3600000UL) >= rebootSchedule.intervalH) {
      LOG("CFG", "定时重启触发（间隔模式）");
      ESP.restart();
    }
    return;
  }

  time_t now = time(nullptr);
  if (now < 100000) {
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn >= 60000) {
      lastWarn = millis();
      LOG("CFG", "定时重启：NTP未同步，跳过检查");
    }
    return;
  }

  static uint32_t lastRebootEpoch = 0;
  static bool lastRebootLoaded = false;
  if (!lastRebootLoaded) {
    lastRebootEpoch = Nvs::getUInt(kNvsRebootCfg, "rb_last_epoch", 0);
    lastRebootLoaded = true;
  }

  struct tm t;
  localtime_r(&now, &t);
  struct tm trigger = t;
  trigger.tm_hour = rebootSchedule.hour;
  trigger.tm_min  = rebootSchedule.minute;
  trigger.tm_sec  = 0;
  time_t triggerEpoch = mktime(&trigger);

  if (now >= triggerEpoch && now < triggerEpoch + 60 && lastRebootEpoch < (uint32_t)triggerEpoch) {
    Nvs::putUInt(kNvsRebootCfg, "rb_last_epoch", (uint32_t)now);
    LOG("CFG", "定时重启触发（每日模式）");
    ESP.restart();
  }
}

void ConfigStore::reset() {
  Nvs::clearNamespace(kNvsSmsConfig);
  Nvs::clearNamespace(kNvsRebootCfg);

  config = Config{};
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

  LOG("CFG", "配置已重置为出厂默认值");
}

bool ConfigStore::isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) {
    return false;
  }
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_DINGTALK:
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_CUSTOM:
    case PUSH_TYPE_WECHAT_WORK:
    case PUSH_TYPE_SMS:
      return ch.url.length() > 0;
    case PUSH_TYPE_PUSHPLUS:
    case PUSH_TYPE_SERVERCHAN:
      return ch.key1.length() > 0;
    case PUSH_TYPE_GOTIFY:
      return ch.url.length() > 0 && ch.key1.length() > 0;
    case PUSH_TYPE_TELEGRAM:
      return ch.key1.length() > 0 && ch.key2.length() > 0;
    default:
      return false;
  }
}

bool ConfigStore::isValid() {
  for (int i = 0; i < config.pushCount; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      return true;
    }
  }
  return false;
}

void ConfigStore::toJson(JsonDocument& doc) {
  JsonObject general          = doc["general"].to<JsonObject>();
  general["adminPhone"]       = config.adminPhone;
  general["webUser"]          = config.webUser;
  general["webPass"]          = config.webPass;
  general["simNotifyEnabled"] = config.simNotifyEnabled;
  general["dataTraffic"]      = config.dataTraffic;
  general["logFileEnabled"]   = config.logFileEnabled;
  general["pushStrategy"]     = (int)config.pushStrategy;
  general["remark"]           = config.remark;

  JsonArray wifiArr = doc["wifiList"].to<JsonArray>();
  for (int i = 0; i < config.wifiCount; i++) {
    JsonObject we = wifiArr.add<JsonObject>();
    we["ssid"] = config.wifiList[i].ssid;
    we["pass"] = config.wifiList[i].password;
  }

  JsonArray channels = doc["pushChannels"].to<JsonArray>();
  for (int i = 0; i < config.pushCount; i++) {
    JsonObject ch     = channels.add<JsonObject>();
    auto& src         = config.pushChannels[i];
    ch["enabled"]     = src.enabled;
    ch["type"]        = (int)src.type;
    ch["name"]        = src.name;
    ch["url"]         = src.url;
    ch["key1"]        = src.key1;
    ch["key2"]        = src.key2;
    ch["customBody"]  = src.customBody;
    ch["retryOnFail"] = src.retryOnFail;
  }

  JsonArray bl = doc["blacklist"].to<JsonArray>();
  for (int i = 0; i < config.blacklistCount; i++) {
    bl.add(config.blacklist[i]);
  }

  JsonObject reboot   = doc["reboot"].to<JsonObject>();
  reboot["enabled"]   = rebootSchedule.enabled;
  reboot["mode"]      = (int)rebootSchedule.mode;
  reboot["hour"]      = (int)rebootSchedule.hour;
  reboot["minute"]    = (int)rebootSchedule.minute;
  reboot["intervalH"] = (int)rebootSchedule.intervalH;
}

void ConfigStore::fromJson(JsonDocument& doc) {
  if (doc["general"].is<JsonObject>()) {
    JsonObject g            = doc["general"].as<JsonObject>();
    config.adminPhone       = g["adminPhone"]       | config.adminPhone;
    config.webUser          = g["webUser"]          | config.webUser;
    config.webPass          = g["webPass"]          | config.webPass;
    config.simNotifyEnabled = g["simNotifyEnabled"] | config.simNotifyEnabled;
    config.dataTraffic      = g["dataTraffic"]      | config.dataTraffic;
    config.logFileEnabled   = g["logFileEnabled"]   | config.logFileEnabled;
    config.pushStrategy     = (PushStrategy)(g["pushStrategy"] | (int)config.pushStrategy);
    if (g["remark"].is<const char*>()) {
      config.remark = String(g["remark"].as<const char*>()).substring(0, 64);
    }
  }

  if (doc["wifiList"].is<JsonArray>()) {
    int wCount = 0;
    for (JsonVariant v : doc["wifiList"].as<JsonArray>()) {
      if (!v.is<JsonObject>() || wCount >= MAX_WIFI_ENTRIES) {
        continue;
      }
      JsonObject we = v.as<JsonObject>();
      String ssid = we["ssid"] | String("");
      if (ssid.length() == 0) {
        continue;
      }
      config.wifiList[wCount].ssid     = ssid;
      config.wifiList[wCount].password = we["pass"] | we["password"] | String("");
      wCount++;
    }
    config.wifiCount = wCount;
  }

  if (doc["pushChannels"].is<JsonArray>()) {
    int i = 0;
    for (JsonObject ch : doc["pushChannels"].as<JsonArray>()) {
      if (i >= MAX_PUSH_CHANNELS) {
        break;
      }
      auto& dst = config.pushChannels[i];
      dst.enabled     = ch["enabled"]     | dst.enabled;
      dst.type        = (PushType)(ch["type"] | (int)dst.type);
      dst.name        = ch["name"]        | dst.name;
      dst.url         = ch["url"]         | dst.url;
      dst.key1        = ch["key1"]        | dst.key1;
      dst.key2        = ch["key2"]        | dst.key2;
      dst.retryOnFail = ch["retryOnFail"] | dst.retryOnFail;
      if (ch["customBody"].is<const char*>()) {
        dst.customBody = ch["customBody"].as<String>();
      } else if (ch["customFormat"].is<const char*>()) {
        dst.customBody = ch["customFormat"].as<String>();
      }
      i++;
    }
    config.pushCount = i;
  }

  if (doc["blacklist"].is<JsonArray>()) {
    int count = 0;
    for (JsonVariant v : doc["blacklist"].as<JsonArray>()) {
      if (count >= MAX_BLACKLIST_ENTRIES) {
        break;
      }
      config.blacklist[count++] = v.as<String>();
    }
    config.blacklistCount = count;
  }

  if (doc["reboot"].is<JsonObject>()) {
    JsonObject r = doc["reboot"].as<JsonObject>();
    rebootSchedule.enabled   = r["enabled"]   | rebootSchedule.enabled;
    rebootSchedule.mode      = r["mode"]      | rebootSchedule.mode;
    rebootSchedule.hour      = r["hour"]      | rebootSchedule.hour;
    rebootSchedule.minute    = r["minute"]    | rebootSchedule.minute;
    rebootSchedule.intervalH = r["intervalH"] | rebootSchedule.intervalH;
  }
}

void ConfigStore::insertWifiFirst(const String& ssid, const String& pass) {
  String s = trimStr(ssid);
  String p = trimStr(pass);
  if (s.length() == 0) {
    return;
  }

  int dupIdx = -1;
  for (int i = 0; i < config.wifiCount; i++) {
    if (config.wifiList[i].ssid == s) {
      dupIdx = i;
      break;
    }
  }
  if (dupIdx >= 0) {
    for (int i = dupIdx; i < config.wifiCount - 1; i++) {
      config.wifiList[i] = config.wifiList[i + 1];
    }
    config.wifiCount--;
  }

  int newCount = min(config.wifiCount + 1, MAX_WIFI_ENTRIES);
  for (int i = newCount - 1; i > 0; i--) {
    config.wifiList[i] = config.wifiList[i - 1];
  }
  config.wifiList[0].ssid     = s;
  config.wifiList[0].password = p;
  config.wifiCount = newCount;
  save();
  LOG("CFG", "WiFi已插入首位: %s，列表共 %d 条", s.c_str(), config.wifiCount);
}

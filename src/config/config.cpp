#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include "logger.h"
#include <time.h>

// NVS 命名空间常量（仅在本文件内使用）
static constexpr char kNvsSmsConfig[] = "sms_config";
static constexpr char kNvsRebootCfg[] = "reboot_cfg";

// 去除首尾空白（Arduino String::trim() 原地修改，此函数返回副本）
static String trimStr(const String& s) {
  String r = s;
  r.trim();
  return r;
}

Config config;
RebootSchedule rebootSchedule;

static Preferences preferences;

void loadConfig() {
  preferences.begin(kNvsSmsConfig, false);
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
  config.remark        = preferences.isKey("remark") ? preferences.getString("remark", "") : "";

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
  preferences.begin(kNvsSmsConfig, false);
  preferences.putString("adminPhone", trimStr(config.adminPhone));
  preferences.putString("webUser",    trimStr(config.webUser));
  preferences.putString("webPass",    trimStr(config.webPass));

  preferences.putUChar("pushCount", (uint8_t)config.pushCount);
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    preferences.putBool((prefix + "en").c_str(),    config.pushChannels[i].enabled);
    preferences.putUChar((prefix + "type").c_str(), (uint8_t)config.pushChannels[i].type);
    preferences.putString((prefix + "url").c_str(),  trimStr(config.pushChannels[i].url));
    preferences.putString((prefix + "name").c_str(), trimStr(config.pushChannels[i].name));
    preferences.putString((prefix + "k1").c_str(),   trimStr(config.pushChannels[i].key1));
    preferences.putString((prefix + "k2").c_str(),   trimStr(config.pushChannels[i].key2));
    preferences.putString((prefix + "body").c_str(), config.pushChannels[i].customBody);  // body 为模板，不 trim
    preferences.putBool((prefix + "retry").c_str(),  config.pushChannels[i].retryOnFail);
  }

  preferences.putBool("simNotify",    config.simNotifyEnabled);
  preferences.putBool("dataTraffic",  config.dataTraffic);

  preferences.putUChar("wifiCount", (uint8_t)config.wifiCount);
  for (int i = 0; i < config.wifiCount; i++) {
    String ks = "wifi" + String(i) + "ssid";
    String kp = "wifi" + String(i) + "pass";
    preferences.putString(ks.c_str(), trimStr(config.wifiList[i].ssid));
    preferences.putString(kp.c_str(), trimStr(config.wifiList[i].password));
  }

  preferences.putUChar("pushStrategy", (uint8_t)config.pushStrategy);
  preferences.putString("remark", trimStr(config.remark).substring(0, 64));

  preferences.putInt("blCount", config.blacklistCount);
  for (int i = 0; i < config.blacklistCount; i++) {
    String key = "bl" + String(i);
    preferences.putString(key.c_str(), trimStr(config.blacklist[i]));
  }

  preferences.end();
  LOG("Config", "配置已保存");
}

void loadRebootSchedule(RebootSchedule& sched) {
  preferences.begin(kNvsRebootCfg, false);
  sched.enabled   = preferences.getBool("rb_enabled", false);
  sched.mode      = preferences.getUChar("rb_mode", 0);
  sched.hour      = preferences.getUChar("rb_hour", 3);
  sched.minute    = preferences.getUChar("rb_minute", 0);
  sched.intervalH = preferences.getUShort("rb_interval", 24);
  preferences.end();
}

void saveRebootSchedule(const RebootSchedule& sched) {
  preferences.begin(kNvsRebootCfg, false);
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
      p.begin(kNvsRebootCfg, true);
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
      p.begin(kNvsRebootCfg, false);
      p.putUInt("rb_last_epoch", (uint32_t)now);
      p.end();
      LOG("Config", "定时重启触发（每日模式）");
      ESP.restart();
    }
  }
}

void resetConfig() {
  Preferences p;
  p.begin(kNvsSmsConfig, false);
  p.clear();
  p.end();
  p.begin(kNvsRebootCfg, false);
  p.clear();
  p.end();

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
  for (int i = 0; i < config.pushCount; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      return true;
    }
  }
  return false;
}

// ── configToJson ─────────────────────────────────────────────
// 将全局 config / rebootSchedule 序列化为嵌套 JSON（新格式）。
// ⚡ 新增/修改 Config 或 RebootSchedule 字段时，只需在此函数和 configFromJson 中
//    同步修改，导出和导入会自动保持一致，无需分别改两个 HTTP 控制器。
void configToJson(JsonDocument& doc) {
  JsonObject general          = doc["general"].to<JsonObject>();
  general["adminPhone"]       = config.adminPhone;
  general["webUser"]          = config.webUser;
  general["webPass"]          = config.webPass;
  general["simNotifyEnabled"] = config.simNotifyEnabled;
  general["dataTraffic"]      = config.dataTraffic;
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
    ch["enabled"]     = config.pushChannels[i].enabled;
    ch["type"]        = (int)config.pushChannels[i].type;
    ch["name"]        = config.pushChannels[i].name;
    ch["url"]         = config.pushChannels[i].url;
    ch["key1"]        = config.pushChannels[i].key1;
    ch["key2"]        = config.pushChannels[i].key2;
    ch["customBody"]  = config.pushChannels[i].customBody;
    ch["retryOnFail"] = config.pushChannels[i].retryOnFail;
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

// ── configFromJson ────────────────────────────────────────────
// 将新格式 JSON 中的各节反序列化到全局 config / rebootSchedule。
// 仅处理存在的节（缺失的节保持当前值不变）。
// 注意：调用方应在调用前完成数组长度校验（返回 HTTP 4xx 后不应再调用此函数）。
void configFromJson(JsonDocument& doc) {
  if (doc["general"].is<JsonObject>()) {
    JsonObject g            = doc["general"].as<JsonObject>();
    config.adminPhone       = g["adminPhone"]       | config.adminPhone;
    config.webUser          = g["webUser"]          | config.webUser;
    config.webPass          = g["webPass"]          | config.webPass;
    config.simNotifyEnabled = g["simNotifyEnabled"] | config.simNotifyEnabled;
    config.dataTraffic      = g["dataTraffic"]      | config.dataTraffic;
    config.pushStrategy     = (PushStrategy)(g["pushStrategy"] | (int)config.pushStrategy);
    if (g["remark"].is<const char*>()) {
      config.remark = String(g["remark"].as<const char*>()).substring(0, 64);
    }
  }

  if (doc["wifiList"].is<JsonArray>()) {
    JsonArray wArr  = doc["wifiList"].as<JsonArray>();
    int wCount = 0;
    for (JsonVariant v : wArr) {
      if (!v.is<JsonObject>()) continue;
      if (wCount >= MAX_WIFI_ENTRIES) break;
      JsonObject we = v.as<JsonObject>();
      String ssid = we["ssid"] | String("");
      if (ssid.length() == 0) continue;
      config.wifiList[wCount].ssid     = ssid;
      config.wifiList[wCount].password = we["pass"] | we["password"] | String("");
      wCount++;
    }
    config.wifiCount = wCount;
  }

  if (doc["pushChannels"].is<JsonArray>()) {
    JsonArray arr = doc["pushChannels"].as<JsonArray>();
    int i = 0;
    for (JsonObject ch : arr) {
      if (i >= MAX_PUSH_CHANNELS) break;
      config.pushChannels[i].enabled    = ch["enabled"]    | config.pushChannels[i].enabled;
      config.pushChannels[i].type       = (PushType)(ch["type"] | (int)config.pushChannels[i].type);
      config.pushChannels[i].name       = ch["name"]       | config.pushChannels[i].name;
      config.pushChannels[i].url        = ch["url"]        | config.pushChannels[i].url;
      config.pushChannels[i].key1       = ch["key1"]       | config.pushChannels[i].key1;
      config.pushChannels[i].key2       = ch["key2"]       | config.pushChannels[i].key2;
      config.pushChannels[i].retryOnFail = ch["retryOnFail"] | config.pushChannels[i].retryOnFail;
      // 向后兼容：customFormat → customBody
      if (ch["customBody"].is<const char*>()) {
        config.pushChannels[i].customBody = ch["customBody"].as<String>();
      } else if (ch["customFormat"].is<const char*>()) {
        config.pushChannels[i].customBody = ch["customFormat"].as<String>();
      }
      i++;
    }
    config.pushCount = i;
  }

  if (doc["blacklist"].is<JsonArray>()) {
    JsonArray arr = doc["blacklist"].as<JsonArray>();
    int count = 0;
    for (JsonVariant v : arr) {
      if (count >= MAX_BLACKLIST_ENTRIES) break;
      config.blacklist[count++] = v.as<String>();
    }
    config.blacklistCount = count;
  }

  if (doc["reboot"].is<JsonObject>()) {
    JsonObject r         = doc["reboot"].as<JsonObject>();
    rebootSchedule.enabled   = r["enabled"]   | rebootSchedule.enabled;
    rebootSchedule.mode      = r["mode"]       | rebootSchedule.mode;
    rebootSchedule.hour      = r["hour"]       | rebootSchedule.hour;
    rebootSchedule.minute    = r["minute"]     | rebootSchedule.minute;
    rebootSchedule.intervalH = r["intervalH"]  | rebootSchedule.intervalH;
  }
}

// WiFi 优先级插入：将新 SSID 插到 wifiList 首位，已存在则先移除再插入
void insertWifiFirst(const String& ssid, const String& pass) {
  String s = trimStr(ssid);
  String p = trimStr(pass);
  if (s.length() == 0) return;

  // 查找重复 SSID 并移除
  int dupIdx = -1;
  for (int i = 0; i < config.wifiCount; i++) {
    if (config.wifiList[i].ssid == s) { dupIdx = i; break; }
  }
  if (dupIdx >= 0) {
    for (int i = dupIdx; i < config.wifiCount - 1; i++) {
      config.wifiList[i] = config.wifiList[i + 1];
    }
    config.wifiCount--;
  }

  // 前移所有现有条目，首位写入新 SSID
  int newCount = min(config.wifiCount + 1, MAX_WIFI_ENTRIES);
  for (int i = newCount - 1; i > 0; i--) {
    config.wifiList[i] = config.wifiList[i - 1];
  }
  config.wifiList[0].ssid     = s;
  config.wifiList[0].password = p;
  config.wifiCount = newCount;
  saveConfig();
  LOG("Config", "WiFi已插入首位: %s，列表共 %d 条", s.c_str(), config.wifiCount);
}

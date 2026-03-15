#include "AppConfig.h"
#include "wifi_config.h"   // WIFI_SSID / WIFI_PASS defaults (src/ is on include path)

Config config;
bool   configValid = false;

static Preferences prefs;

// ── Save ─────────────────────────────────────────────────────────────────────
void saveConfig() {
  prefs.begin("sms_config", false);
  prefs.putString("smtpServer", config.smtpServer);
  prefs.putInt   ("smtpPort",   config.smtpPort);
  prefs.putString("smtpUser",   config.smtpUser);
  prefs.putString("smtpPass",   config.smtpPass);
  prefs.putString("smtpSendTo", config.smtpSendTo);
  prefs.putString("adminPhone", config.adminPhone);
  prefs.putString("webUser",    config.webUser);
  prefs.putString("webPass",    config.webPass);
  prefs.putString("wifiSSID",   config.wifiSSID);
  prefs.putString("wifiPass",   config.wifiPass);
  prefs.putString("numBlkList", config.numberBlackList);
  prefs.putBool  ("rebootEn",   config.autoRebootEnabled);
  prefs.putString("rebootTime", config.autoRebootTime);

  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String p = "push" + String(i);
    prefs.putBool  ((p + "en"   ).c_str(), config.pushChannels[i].enabled);
    prefs.putUChar ((p + "type" ).c_str(), (uint8_t)config.pushChannels[i].type);
    prefs.putString((p + "url"  ).c_str(), config.pushChannels[i].url);
    prefs.putString((p + "name" ).c_str(), config.pushChannels[i].name);
    prefs.putString((p + "k1"   ).c_str(), config.pushChannels[i].key1);
    prefs.putString((p + "k2"   ).c_str(), config.pushChannels[i].key2);
    prefs.putString((p + "body" ).c_str(), config.pushChannels[i].customBody);
    prefs.putString((p + "cbody").c_str(), config.pushChannels[i].customCallBody);
  }
  prefs.end();
  Serial.println("配置已保存");
}

// ── Load ─────────────────────────────────────────────────────────────────────
void loadConfig() {
  prefs.begin("sms_config", true);
  config.smtpServer      = prefs.getString("smtpServer", "");
  config.smtpPort        = prefs.getInt   ("smtpPort",   465);
  config.smtpUser        = prefs.getString("smtpUser",   "");
  config.smtpPass        = prefs.getString("smtpPass",   "");
  config.smtpSendTo      = prefs.getString("smtpSendTo", "");
  config.adminPhone      = prefs.getString("adminPhone", "");
  config.webUser         = prefs.getString("webUser",    DEFAULT_WEB_USER);
  config.webPass         = prefs.getString("webPass",    DEFAULT_WEB_PASS);
  config.wifiSSID        = prefs.getString("wifiSSID",   WIFI_SSID);
  config.wifiPass        = prefs.getString("wifiPass",   WIFI_PASS);
  config.numberBlackList = prefs.getString("numBlkList", "");
  config.autoRebootEnabled = prefs.getBool  ("rebootEn",   false);
  config.autoRebootTime    = prefs.getString("rebootTime", "03:00");

  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String p = "push" + String(i);
    config.pushChannels[i].enabled        = prefs.getBool  ((p + "en"   ).c_str(), false);
    config.pushChannels[i].type           = (PushType)prefs.getUChar((p + "type").c_str(), PUSH_TYPE_POST_JSON);
    config.pushChannels[i].url            = prefs.getString((p + "url"  ).c_str(), "");
    config.pushChannels[i].name           = prefs.getString((p + "name" ).c_str(), "通道" + String(i + 1));
    config.pushChannels[i].key1           = prefs.getString((p + "k1"   ).c_str(), "");
    config.pushChannels[i].key2           = prefs.getString((p + "k2"   ).c_str(), "");
    config.pushChannels[i].customBody     = prefs.getString((p + "body" ).c_str(), "");
    config.pushChannels[i].customCallBody = prefs.getString((p + "cbody").c_str(), "");
  }

  // Migrate legacy single-channel config (httpUrl) → channel 0
  String oldUrl = prefs.getString("httpUrl", "");
  if (oldUrl.length() > 0 && !config.pushChannels[0].enabled) {
    config.pushChannels[0].enabled = true;
    config.pushChannels[0].url     = oldUrl;
    config.pushChannels[0].type    = prefs.getUChar("barkMode", 0) ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
    config.pushChannels[0].name    = "迁移通道";
    Serial.println("已迁移旧HTTP配置到推送通道1");
  }

  prefs.end();
  Serial.println("配置已加载");
}

// ── Validation ───────────────────────────────────────────────────────────────
bool isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON: case PUSH_TYPE_BARK:       case PUSH_TYPE_GET:
    case PUSH_TYPE_DINGTALK:  case PUSH_TYPE_FEISHU:     case PUSH_TYPE_WORK_WEIXIN:
    case PUSH_TYPE_SMS:       case PUSH_TYPE_CUSTOM:     return ch.url.length() > 0;
    case PUSH_TYPE_PUSHPLUS:  case PUSH_TYPE_SERVERCHAN: return ch.key1.length() > 0;
    case PUSH_TYPE_GOTIFY:    return ch.url.length() > 0 && ch.key1.length() > 0;
    case PUSH_TYPE_TELEGRAM:  return ch.key1.length() > 0 && ch.key2.length() > 0;
    default: return false;
  }
}

bool isConfigValid() {
  bool emailOk = config.smtpServer.length() > 0 && config.smtpUser.length() > 0
              && config.smtpPass.length() > 0   && config.smtpSendTo.length() > 0;
  if (emailOk) return true;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++)
    if (isPushChannelValid(config.pushChannels[i])) return true;
  return false;
}

bool isInNumberBlackList(const char* sender) {
  if (config.numberBlackList.length() == 0) return false;
  String s = String(sender);
  if (s.startsWith("+86")) s = s.substring(3);
  String list = config.numberBlackList;
  int start = 0;
  while (start <= (int)list.length()) {
    int end = list.indexOf('\n', start);
    if (end == -1) end = list.length();
    String line = list.substring(start, end);
    line.trim();
    if (line.length() > 0 && line.equals(s)) return true;
    start = end + 1;
  }
  return false;
}


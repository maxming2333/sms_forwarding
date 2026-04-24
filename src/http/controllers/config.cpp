#include "config.h"
#include "config/config.h"
#include "logger.h"
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <time.h>

void configController(AsyncWebServerRequest* request) {
  AsyncJsonResponse* resp = new AsyncJsonResponse();
  JsonObject root = resp->getRoot();

  root["adminPhone"] = config.adminPhone;
  root["webUser"]    = config.webUser;
  // smtpPass and webPass are NEVER included (OWASP A02)
  root["remark"]     = config.remark;

  root["simNotifyEnabled"] = config.simNotifyEnabled;
  root["dataTraffic"]      = config.dataTraffic;

  root["pushStrategy"] = (int)config.pushStrategy;
  root["pushCount"]    = config.pushCount;

  root["rbEnabled"]   = rebootSchedule.enabled;
  root["rbMode"]      = (int)rebootSchedule.mode;
  root["rbHour"]      = (int)rebootSchedule.hour;
  root["rbMinute"]    = (int)rebootSchedule.minute;
  root["rbIntervalH"] = (int)rebootSchedule.intervalH;

  JsonArray channels = root["pushChannels"].to<JsonArray>();
  for (int i = 0; i < config.pushCount; i++) {
    JsonObject ch = channels.add<JsonObject>();
    ch["index"]      = i;
    ch["enabled"]    = config.pushChannels[i].enabled;
    ch["type"]       = (int)config.pushChannels[i].type;
    ch["name"]       = config.pushChannels[i].name;
    ch["url"]        = config.pushChannels[i].url;
    ch["key1"]       = config.pushChannels[i].key1;
    ch["key2"]       = config.pushChannels[i].key2;
    ch["customBody"] = config.pushChannels[i].customBody;
    ch["retryOnFail"] = config.pushChannels[i].retryOnFail;
  }

  JsonArray wifiArr = root["wifiList"].to<JsonArray>();
  for (int i = 0; i < config.wifiCount; i++) {
    JsonObject we = wifiArr.add<JsonObject>();
    we["ssid"]     = config.wifiList[i].ssid;
    we["password"] = "";  // 安全起见，密码不回显
  }

  resp->setLength();
  request->send(resp);
}

// ── configExportController ────────────────────────────────────
// 将 Config struct 直接序列化为嵌套 JSON，触发浏览器文件下载。
void configExportController(AsyncWebServerRequest* request) {
  JsonDocument doc;

  // general 节
  JsonObject general          = doc["general"].to<JsonObject>();
  general["adminPhone"]       = config.adminPhone;
  general["webUser"]          = config.webUser;
  general["webPass"]          = config.webPass;
  general["simNotifyEnabled"] = config.simNotifyEnabled;
  general["dataTraffic"]      = config.dataTraffic;
  general["pushStrategy"]     = (int)config.pushStrategy;
  general["remark"]           = config.remark;

  // wifiList 数组（含密码，用于导出还原）
  JsonArray wifiArr = doc["wifiList"].to<JsonArray>();
  for (int i = 0; i < config.wifiCount; i++) {
    JsonObject we = wifiArr.add<JsonObject>();
    we["ssid"] = config.wifiList[i].ssid;
    we["pass"] = config.wifiList[i].password;
  }

  // pushChannels 数组（仅导出 pushCount 个）
  JsonArray channels = doc["pushChannels"].to<JsonArray>();
  for (int i = 0; i < config.pushCount; i++) {
    JsonObject ch    = channels.add<JsonObject>();
    ch["enabled"]     = config.pushChannels[i].enabled;
    ch["type"]        = (int)config.pushChannels[i].type;
    ch["name"]        = config.pushChannels[i].name;
    ch["url"]         = config.pushChannels[i].url;
    ch["key1"]        = config.pushChannels[i].key1;
    ch["key2"]        = config.pushChannels[i].key2;
    ch["customBody"]  = config.pushChannels[i].customBody;
    ch["retryOnFail"] = config.pushChannels[i].retryOnFail;
  }

  // blacklist 数组
  JsonArray bl = doc["blacklist"].to<JsonArray>();
  for (int i = 0; i < config.blacklistCount; i++) {
    bl.add(config.blacklist[i]);
  }

  // reboot 节
  JsonObject reboot   = doc["reboot"].to<JsonObject>();
  reboot["enabled"]   = rebootSchedule.enabled;
  reboot["mode"]      = (int)rebootSchedule.mode;
  reboot["hour"]      = (int)rebootSchedule.hour;
  reboot["minute"]    = (int)rebootSchedule.minute;
  reboot["intervalH"] = (int)rebootSchedule.intervalH;

  // 序列化
  String body;
  serializeJsonPretty(doc, body);

  char filename[32] = "sms_config.json";
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm t;
    localtime_r(&now, &t);
    snprintf(filename, sizeof(filename), "sms_config_%04d%02d%02d.json", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  }

  char disposition[80];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);

  AsyncResponseStream* resp = request->beginResponseStream("application/octet-stream");
  resp->addHeader("Content-Disposition", disposition);
  resp->addHeader("X-Content-Type-Options", "nosniff");
  resp->print(body);
  request->send(resp);

  LOG("HTTP", "配置已导出（struct格式），大小: %d 字节", body.length());
}

// ── configImportController ────────────────────────────────────
// 将 JSON 映射到 Config struct，再通过 saveConfig() 统一写入 NVS。
// 同时兼容旧 namespace-grouped 格式（sms_config/reboot_cfg 顶层键）。
void configImportController(AsyncWebServerRequest* request, uint8_t* data,
                            size_t len, size_t index, size_t total) {
  if (total > 51200) {
    if (index == 0) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"文件大小超过限制（最大50KB）\"}");
    }
    return;
  }

  // 累积所有分块到 static 缓冲区
  static String importBuf;
  if (index == 0) {
    importBuf = "";
    importBuf.reserve(total);
  }
  importBuf.concat(reinterpret_cast<const char*>(data), len);

  if (index + len < total) return;

  JsonDocument doc;
  if (deserializeJson(doc, importBuf) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"JSON解析失败，请确认文件格式正确\"}");
    return;
  }

  // 旧 namespace-grouped 格式识别
  if (doc["sms_config"].is<JsonObject>() || doc["reboot_cfg"].is<JsonObject>()) {
    // 旧格式：按原 NVS 路径通过 saveConfig/saveRebootSchedule 完成写入
    // 此分支仅做基本映射以保持向后兼容，不引入新废弃键
    if (doc["sms_config"].is<JsonObject>()) {
      JsonObject s = doc["sms_config"].as<JsonObject>();
      config.adminPhone       = s["adminPhone"]    | config.adminPhone;
      config.webUser          = s["webUser"]       | config.webUser;
      config.webPass          = s["webPass"]       | config.webPass;
      config.simNotifyEnabled = s["simNotify"]     | config.simNotifyEnabled;
      config.dataTraffic       = s["dataTraffic"]   | config.dataTraffic;
      // 旧格式：wifiSsid/wifiPass → wifiList[0] 迁移
      if (s["wifiSsid"].is<const char*>()) {
        config.wifiList[0].ssid     = s["wifiSsid"].as<String>();
        config.wifiList[0].password = s["wifiPass"] | String("");
        config.wifiCount = 1;
      }
      config.pushStrategy     = (PushStrategy)(s["pushStrategy"] | (int)config.pushStrategy);
      for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
        String prefix = "push" + String(i);
        if (s[prefix + "en"].is<bool>())     config.pushChannels[i].enabled    = s[prefix + "en"].as<bool>();
        if (s[prefix + "type"].is<int>())    config.pushChannels[i].type       = (PushType)s[prefix + "type"].as<int>();
        if (s[prefix + "url"].is<const char*>())  config.pushChannels[i].url   = s[prefix + "url"].as<String>();
        if (s[prefix + "name"].is<const char*>()) config.pushChannels[i].name  = s[prefix + "name"].as<String>();
        if (s[prefix + "k1"].is<const char*>())   config.pushChannels[i].key1  = s[prefix + "k1"].as<String>();
        if (s[prefix + "k2"].is<const char*>())   config.pushChannels[i].key2  = s[prefix + "k2"].as<String>();
        if (s[prefix + "body"].is<const char*>())  config.pushChannels[i].customBody = s[prefix + "body"].as<String>();
        if (s[prefix + "retry"].is<bool>())        config.pushChannels[i].retryOnFail = s[prefix + "retry"].as<bool>();
      }
      int blCount = s["blCount"] | 0;
      if (blCount > MAX_BLACKLIST_ENTRIES) blCount = MAX_BLACKLIST_ENTRIES;
      config.blacklistCount = 0;
      for (int i = 0; i < blCount; i++) {
        String key = "bl" + String(i);
        if (s[key].is<const char*>()) {
          config.blacklist[config.blacklistCount++] = s[key].as<String>();
        }
      }
    }
    if (doc["reboot_cfg"].is<JsonObject>()) {
      JsonObject r = doc["reboot_cfg"].as<JsonObject>();
      rebootSchedule.enabled   = r["rb_enabled"]  | rebootSchedule.enabled;
      rebootSchedule.mode      = r["rb_mode"]      | rebootSchedule.mode;
      rebootSchedule.hour      = r["rb_hour"]      | rebootSchedule.hour;
      rebootSchedule.minute    = r["rb_minute"]    | rebootSchedule.minute;
      rebootSchedule.intervalH = r["rb_interval"]  | rebootSchedule.intervalH;
    }
    saveConfig();
    saveRebootSchedule(rebootSchedule);
    request->send(200, "application/json", "{\"ok\":true,\"message\":\"配置导入成功，设备将在2秒后自动重启\"}");
    xTaskCreate([](void*) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      ESP.restart();
      vTaskDelete(nullptr);
    }, "restart", 2048, nullptr, 1, nullptr);
    return;
  }

  // 新格式：至少一个可识别节名
  bool hasRecognized = doc["general"].is<JsonObject>()
                    || doc["wifi"].is<JsonObject>()
                    || doc["wifiList"].is<JsonArray>()
                    || doc["pushChannels"].is<JsonArray>()
                    || doc["blacklist"].is<JsonArray>()
                    || doc["reboot"].is<JsonObject>();
  if (!hasRecognized) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"配置格式不兼容，未找到可识别的配置节\"}");
    return;
  }

  if (doc["general"].is<JsonObject>()) {
    JsonObject g            = doc["general"].as<JsonObject>();
    config.adminPhone       = g["adminPhone"]       | config.adminPhone;
    config.webUser          = g["webUser"]          | config.webUser;
    config.webPass          = g["webPass"]          | config.webPass;
    config.simNotifyEnabled = g["simNotifyEnabled"] | config.simNotifyEnabled;
    config.dataTraffic       = g["dataTraffic"]      | config.dataTraffic;
    config.pushStrategy     = (PushStrategy)(g["pushStrategy"] | (int)config.pushStrategy);
    if (g["remark"].is<const char*>()) {
      config.remark = String(g["remark"].as<const char*>()).substring(0, 64);
    }
  }

  // 新格式：wifiList 数组；向后兼容旧 wifi 单对象 → wifiList[0]
  if (doc["wifiList"].is<JsonArray>()) {
    JsonArray wArr = doc["wifiList"].as<JsonArray>();
    if ((int)wArr.size() > MAX_WIFI_ENTRIES) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"wifiList 超过最大限制（5条）\"}");
      return;
    }
    int wCount = 0;
    for (JsonVariant v : wArr) {
      if (!v.is<JsonObject>()) continue;
      JsonObject we = v.as<JsonObject>();
      String ssid = we["ssid"] | String("");
      if (ssid.length() == 0) continue;
      config.wifiList[wCount].ssid     = ssid;
      config.wifiList[wCount].password = we["pass"] | we["password"] | String("");
      wCount++;
    }
    config.wifiCount = wCount;
  } else if (doc["wifi"].is<JsonObject>()) {
    // 向后兼容：旧格式 wifi 单对象 → wifiList[0]
    JsonObject w = doc["wifi"].as<JsonObject>();
    String ssid  = w["ssid"] | String("");
    if (ssid.length() > 0) {
      config.wifiList[0].ssid     = ssid;
      config.wifiList[0].password = w["pass"] | String("");
      config.wifiCount = 1;
    }
  }

  if (doc["pushChannels"].is<JsonArray>()) {
    JsonArray arr = doc["pushChannels"].as<JsonArray>();
    if ((int)arr.size() > MAX_PUSH_CHANNELS) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"pushChannels 超过最大限制（10条）\"}");
      return;
    }
    int i = 0;
    for (JsonObject ch : arr) {
      if (i >= MAX_PUSH_CHANNELS) break;
      config.pushChannels[i].enabled = ch["enabled"] | config.pushChannels[i].enabled;
      config.pushChannels[i].type    = (PushType)(ch["type"] | (int)config.pushChannels[i].type);
      config.pushChannels[i].name    = ch["name"]    | config.pushChannels[i].name;
      config.pushChannels[i].url     = ch["url"]     | config.pushChannels[i].url;
      config.pushChannels[i].key1    = ch["key1"]    | config.pushChannels[i].key1;
      config.pushChannels[i].key2    = ch["key2"]    | config.pushChannels[i].key2;
      // FR-021: 向后兼容 customFormat → customBody 映射
      if (ch["customBody"].is<const char*>()) {
        config.pushChannels[i].customBody = ch["customBody"].as<String>();
      } else if (ch["customFormat"].is<const char*>()) {
        config.pushChannels[i].customBody = ch["customFormat"].as<String>();
      }
      config.pushChannels[i].retryOnFail = ch["retryOnFail"] | config.pushChannels[i].retryOnFail;
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

  saveConfig();
  saveRebootSchedule(rebootSchedule);

  request->send(200, "application/json", "{\"ok\":true,\"message\":\"配置导入成功，设备将在2秒后自动重启\"}");
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP.restart();
    vTaskDelete(nullptr);
  }, "restart", 2048, nullptr, 1, nullptr);
}


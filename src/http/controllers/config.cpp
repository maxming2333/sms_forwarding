#include "config.h"
#include "config/config.h"
#include "http/body_accumulator.h"
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
// 字段映射统一由 configToJson()（config.cpp）维护，新增字段无需改此处。
void configExportController(AsyncWebServerRequest* request) {
  JsonDocument doc;
  configToJson(doc);

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
  const char* importBuf = nullptr;
  if (!httpAccumulateBody(request, data, len, index, total, HTTP_JSON_BODY_MAX_BYTES, &importBuf)) return;
  if (importBuf == nullptr) return;

  JsonDocument doc;
  if (deserializeJson(doc, importBuf) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    httpReleaseAccumulatedBody(request);
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"JSON解析失败，请确认文件格式正确\"}");
    return;
  }
  httpReleaseAccumulatedBody(request);

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

  // 向后兼容：旧 wifi 单对象 → wifiList（转换后交给 configFromJson 统一处理）
  if (!doc["wifiList"].is<JsonArray>() && doc["wifi"].is<JsonObject>()) {
    JsonObject w = doc["wifi"].as<JsonObject>();
    String ssid  = w["ssid"] | String("");
    if (ssid.length() > 0) {
      JsonArray wArr = doc["wifiList"].to<JsonArray>();
      JsonObject we  = wArr.add<JsonObject>();
      we["ssid"] = ssid;
      we["pass"] = w["pass"] | String("");
    }
  }

  // 数组长度校验（超出限制立即返回 4xx，不调用 configFromJson）
  if (doc["wifiList"].is<JsonArray>() && (int)doc["wifiList"].as<JsonArray>().size() > MAX_WIFI_ENTRIES) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"wifiList 超过最大限制（5条）\"}");
    return;
  }
  if (doc["pushChannels"].is<JsonArray>() && (int)doc["pushChannels"].as<JsonArray>().size() > MAX_PUSH_CHANNELS) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"pushChannels 超过最大限制（10条）\"}");
    return;
  }

  // 字段映射统一由 configFromJson()（config.cpp）维护，新增字段无需改此处
  configFromJson(doc);
  saveConfig();
  saveRebootSchedule(rebootSchedule);

  request->send(200, "application/json", "{\"ok\":true,\"message\":\"配置导入成功，设备将在2秒后自动重启\"}");
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP.restart();
    vTaskDelete(nullptr);
  }, "restart", 2048, nullptr, 1, nullptr);
}


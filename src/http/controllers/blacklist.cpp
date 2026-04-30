#include "blacklist.h"
#include "config/config.h"
#include "http/body_accumulator.h"
#include "logger.h"
#include <ArduinoJson.h>

void blacklistGetController(AsyncWebServerRequest* request) {
  JsonDocument doc;
  JsonArray arr = doc["numbers"].to<JsonArray>();
  for (int i = 0; i < config.blacklistCount; i++) {
    arr.add(config.blacklist[i]);
  }
  doc["count"]    = config.blacklistCount;
  doc["maxCount"] = MAX_BLACKLIST_ENTRIES;
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void blacklistPostController(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  const char* requestBody = nullptr;
  if (!httpAccumulateBody(request, data, len, index, total, HTTP_JSON_BODY_MAX_BYTES, &requestBody)) return;
  if (requestBody == nullptr) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, requestBody);
  httpReleaseAccumulatedBody(request);
  if (err || !doc["numbers"].is<JsonArray>()) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"请求格式错误\"}");
    return;
  }

  JsonArray arr = doc["numbers"].as<JsonArray>();
  if ((int)arr.size() > MAX_BLACKLIST_ENTRIES) {
    request->send(400, "application/json",
      "{\"ok\":false,\"error\":\"黑名单最多支持20个号码\"}");
    return;
  }

  config.blacklistCount = 0;
  for (JsonVariant v : arr) {
    config.blacklist[config.blacklistCount++] = v.as<String>();
  }
  saveConfig();
  LOG("HTTP", "黑名单已更新，共 %d 条", config.blacklistCount);

  JsonDocument resp;
  resp["ok"]    = true;
  resp["count"] = config.blacklistCount;
  String body;
  serializeJson(resp, body);
  request->send(200, "application/json", body);
}

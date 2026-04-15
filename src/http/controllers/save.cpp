#include "save.h"
#include "../http_server.h"
#include "config/config.h"
#include "email/email.h"
#include "wifi/wifi_manager.h"
#include "logger.h"
#include <ArduinoJson.h>
#include <WiFi.h>

#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

void saveController(AsyncWebServerRequest* request) {
  // ── 各字段仅在参数实际出现时才覆盖，否则保留当前值（partial-update 语义）──

  // webUser：参数存在且非空才更新，否则保留
  if (request->hasParam("webUser", true)) {
    String v = request->getParam("webUser", true)->value();
    config.webUser = (v.length() > 0) ? v : DEFAULT_WEB_USER;
  }
  // webPass：只有非空时才更新（空 = 不修改密码）
  if (request->hasParam("webPass", true)) {
    String v = request->getParam("webPass", true)->value();
    if (v.length() > 0) config.webPass = v;
  }

  if (request->hasParam("smtpServer", true)) config.smtpServer = request->getParam("smtpServer", true)->value();
  if (request->hasParam("smtpPort",   true)) {
    int p = request->getParam("smtpPort", true)->value().toInt();
    config.smtpPort = (p > 0) ? p : 465;
  }
  if (request->hasParam("smtpUser",   true)) config.smtpUser   = request->getParam("smtpUser",   true)->value();
  if (request->hasParam("smtpPass",   true)) config.smtpPass   = request->getParam("smtpPass",   true)->value();
  if (request->hasParam("smtpSendTo", true)) config.smtpSendTo = request->getParam("smtpSendTo", true)->value();
  if (request->hasParam("adminPhone", true)) config.adminPhone = request->getParam("adminPhone", true)->value();

  // checkbox：只有全量表单提交时这两个字段才有意义；
  // 用 pushCount 是否存在来判断是否为全量表单提交
  bool isFullForm = request->hasParam("pushCount", true);
  if (isFullForm) {
    config.simNotifyEnabled = request->hasParam("simNotifyEnabled", true);
    config.dataTraffic       = request->hasParam("dataTraffic",      true);
  }

  if (request->hasParam("pushStrategy", true)) {
    int ps = request->getParam("pushStrategy", true)->value().toInt();
    if (ps == 0 || ps == 1) config.pushStrategy = (PushStrategy)ps;
  }

  // 推送通道：仅全量表单提交时处理
  if (isFullForm) {
    int pushCount = request->getParam("pushCount", true)->value().toInt();
    if (pushCount < 1) pushCount = 1;
    if (pushCount > MAX_PUSH_CHANNELS) {
      request->send(400, "application/json", "{\"ok\":false,\"message\":\"pushCount 超过最大限制\"}");
      return;
    }
    config.pushCount = pushCount;

    for (int i = 0; i < pushCount; i++) {
      String idx = String(i);
      config.pushChannels[i].enabled    = request->hasParam("push" + idx + "en",   true);
      config.pushChannels[i].type       = (PushType)(request->hasParam("push" + idx + "type", true) ? request->getParam("push" + idx + "type", true)->value().toInt() : 1);
      config.pushChannels[i].url        = request->hasParam("push" + idx + "url",  true) ? request->getParam("push" + idx + "url",  true)->value() : "";
      config.pushChannels[i].name       = request->hasParam("push" + idx + "name", true) ? request->getParam("push" + idx + "name", true)->value() : "";
      config.pushChannels[i].key1       = request->hasParam("push" + idx + "key1", true) ? request->getParam("push" + idx + "key1", true)->value() : "";
      config.pushChannels[i].key2       = request->hasParam("push" + idx + "key2", true) ? request->getParam("push" + idx + "key2", true)->value() : "";
      config.pushChannels[i].customBody = request->hasParam("push" + idx + "body", true) ? request->getParam("push" + idx + "body", true)->value() : "";
      if (config.pushChannels[i].name.length() == 0) {
        config.pushChannels[i].name = "通道" + String(i + 1);
      }
    }
    // 清除用户删除的通道槽位
    for (int i = pushCount; i < MAX_PUSH_CHANNELS; i++) {
      config.pushChannels[i] = PushChannel{};
    }
  }

  saveConfig();
  refreshAuthCredentials();

  LOG("HTTP", "配置已保存，发送成功响应");

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "配置保存成功";
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);

  if (isConfigValid()) {
    String subject = "短信转发器配置已更新";
    String body = "设备配置已更新\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
}

void saveRebootController(AsyncWebServerRequest* request) {
  rebootSchedule.enabled = request->hasParam("rbEnabled", true);
  if (request->hasParam("rbMode", true))
    rebootSchedule.mode = (uint8_t)request->getParam("rbMode", true)->value().toInt();
  if (request->hasParam("rbHour", true))
    rebootSchedule.hour = (uint8_t)constrain(request->getParam("rbHour", true)->value().toInt(), 0, 23);
  if (request->hasParam("rbMinute", true))
    rebootSchedule.minute = (uint8_t)constrain(request->getParam("rbMinute", true)->value().toInt(), 0, 59);
  if (request->hasParam("rbIntervalH", true))
    rebootSchedule.intervalH = (uint16_t)constrain(request->getParam("rbIntervalH", true)->value().toInt(), 1, 168);

  saveRebootSchedule(rebootSchedule);

  LOG("HTTP", "定时重启配置已保存");

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "定时重启配置保存成功";
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

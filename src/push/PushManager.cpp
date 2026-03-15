#include "PushManager.h"
#include "channels/PushChannels.h"
#include "sim/SimManager.h"
#include <WiFi.h>

// ── Single SMS channel dispatch ───────────────────────────────────────────────
int pushOne(const PushChannel& ch, const char* sender, const char* msg,
            const char* ts, const char* dev) {
  if (!ch.enabled) return -1;

  // Types that require a URL
  bool needUrl = (ch.type == PUSH_TYPE_POST_JSON || ch.type == PUSH_TYPE_BARK  ||
                  ch.type == PUSH_TYPE_GET        || ch.type == PUSH_TYPE_DINGTALK ||
                  ch.type == PUSH_TYPE_CUSTOM);
  if (needUrl && ch.url.length() == 0) return -1;

  String name = ch.name.length() > 0 ? ch.name : ("通道" + String((int)ch.type));
  Serial.println("[Push] 发送到通道: " + name);

  int code = -1;
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:   code = pushPostJson   (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_BARK:        code = pushBark        (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_GET:         code = pushGet         (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_DINGTALK:    code = pushDingtalk    (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_PUSHPLUS:    code = pushPushplus    (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_SERVERCHAN:  code = pushServerchan  (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_CUSTOM:      code = pushCustom      (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_FEISHU:      code = pushFeishu      (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_GOTIFY:      code = pushGotify      (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_TELEGRAM:    code = pushTelegram    (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_WORK_WEIXIN: code = pushWorkWeixin  (ch, sender, msg, ts, dev); break;
    case PUSH_TYPE_SMS:         code = pushSmsForward  (ch, sender, msg, ts, dev); break;
    default:
      Serial.println("[Push] 未知推送类型");
      return -1;
  }
  if (code > 0) Serial.printf("[Push] [%s] HTTP %d\n", name.c_str(), code);
  else          Serial.printf("[Push] [%s] 请求失败: %d\n", name.c_str(), code);
  return code;
}

// ── All SMS channels dispatch ─────────────────────────────────────────────────
void pushAll(const char* sender, const char* msg, const char* ts) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Push] WiFi未连接，跳过推送");
    return;
  }
  Serial.println("\n=== 开始多通道推送 ===");
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushOne(config.pushChannels[i], sender, msg, ts, devicePhoneNumber.c_str());
      delay(100);
    }
  }
  Serial.println("=== 推送完成 ===\n");
}

// ── Single call channel dispatch ──────────────────────────────────────────────
int pushCallOne(const PushChannel& ch, const char* caller,
                const char* ts, const char* dev) {
  if (!ch.enabled) return -1;
  String name = ch.name.length() > 0 ? ch.name : ("通道" + String((int)ch.type));
  Serial.println("[PushCall] 发送来电到通道: " + name);
  int code = pushCall(ch, caller, ts, dev);
  if (code > 0) Serial.printf("[PushCall] [%s] HTTP %d\n", name.c_str(), code);
  else          Serial.printf("[PushCall] [%s] 结果: %d\n", name.c_str(), code);
  return code;
}

// ── All call channels dispatch ────────────────────────────────────────────────
void pushCallAll(const char* caller, const char* ts) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[PushCall] WiFi未连接，跳过来电推送");
    return;
  }
  Serial.println("\n=== 开始来电多通道推送 ===");
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushCallOne(config.pushChannels[i], caller, ts, devicePhoneNumber.c_str());
      delay(100);
    }
  }
  Serial.println("=== 来电推送完成 ===\n");
}


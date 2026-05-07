#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config/config.h"

// 两次 HTTP 请求之间的最小冷却时间，防止密集请求导致 ESP32 TLS/TCP 栈资源
// （mbedtls 堆、lwIP socket）尚未充分释放就开始下一次 TLS 握手。
constexpr unsigned long HTTP_COOLDOWN_MS = 500;

// 各推送渠道的具体发送实现。
// 约定：`renderedBody` 是用户在配置中自定义的请求体经模板渲染后的最终内容；
//   - 非空：直接作为 HTTP 请求体发送，覆盖渠道默认 payload；
//   - 空：使用渠道的内置默认 payload（短信文本拼装）。
// 所有方法都是同步阻塞的；返回 true 表示 HTTP 状态码符合该渠道的成功语义。
//
// 重要：每个发送函数内部都使用**栈上**的 WiFiClientSecure / HTTPClient，
// 严禁共享，否则在并发推送或与 OTA 同时下载时会导致 TLS 状态损坏。
class PushChannels {
public:
  static bool sendPostJson  (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendBark      (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendGet       (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendDingtalk  (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendPushPlus  (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendServerChan(const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendCustom    (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendFeishu    (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendGotify    (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendTelegram  (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendWechatWork(const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
  static bool sendSmsPush   (const PushChannel& ch, const String& sender, const String& message, const String& timestamp, const String& renderedBody);
};

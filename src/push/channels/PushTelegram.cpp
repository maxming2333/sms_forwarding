// Telegram Bot  POST /bot<token>/sendMessage
// key1 = chat_id, key2 = bot_token
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushTelegram(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String base = ch.url.length() > 0 ? ch.url : "https://api.telegram.org";
  if (base.endsWith("/")) base.remove(base.length() - 1);
  String url = base + "/bot" + ch.key2 + "/sendMessage";

  String text = "📱短信通知\n发送者: " + formatPhoneNumber(sender)
              + "\n接收卡号: "          + String(dev)
              + "\n内容: "             + String(msg)
              + "\n时间: "             + formatTimestamp(ts);
  String body = "{\"chat_id\":\"" + ch.key1
              + "\",\"text\":\""  + jsonEscape(text) + "\"}";
  Serial.println("[PushTelegram] " + url);
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code;
}


// Gotify  POST /message?token=<token>
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushGotify(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String url = ch.url;
  if (!url.endsWith("/")) url += "/";
  url += "message?token=" + ch.key1;

  String body = "{\"title\":\"短信来自: " + jsonEscape(formatPhoneNumber(sender))
    + "\",\"message\":\"接收卡号: " + jsonEscape(dev)
    + "\\n\\n" + jsonEscape(msg)
    + "\\n\\n时间: " + jsonEscape(formatTimestamp(ts))
    + "\",\"priority\":5}";
  Serial.println("[PushGotify] " + url);
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code;
}


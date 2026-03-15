// Custom template  POST body with {sender} {sender_fmt} {message} {timestamp} {device} {receiver}
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushCustom(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  if (ch.customBody.length() == 0) {
    Serial.println("[PushCustom] 模板为空，跳过");
    return -1;
  }
  String body = ch.customBody;
  body.replace("{sender}",      jsonEscape(sender));
  body.replace("{sender_fmt}",  jsonEscape(formatPhoneNumber(sender)));
  body.replace("{message}",     jsonEscape(msg));
  body.replace("{timestamp}",   jsonEscape(formatTimestamp(ts)));
  body.replace("{device}",      jsonEscape(dev));
  body.replace("{receiver}",    jsonEscape(dev));
  Serial.println("[PushCustom] " + body);
  HTTPClient http;
  http.begin(ch.url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code;
}


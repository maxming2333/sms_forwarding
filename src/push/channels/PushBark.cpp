// Bark iOS push  POST {"title":"sender_fmt","body":"message\n接收卡号:dev"}
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushBark(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  HTTPClient http;
  http.begin(ch.url);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"title\":\"" + jsonEscape(formatPhoneNumber(sender)) +
                "\",\"body\":\"" + jsonEscape(msg)   +
                "\\n接收卡号: "  + jsonEscape(dev)   + "\"}";
  Serial.println("[PushBark] " + body);
  int code = http.POST(body);
  http.end();
  return code;
}


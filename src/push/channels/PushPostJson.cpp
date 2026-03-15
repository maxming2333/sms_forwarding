// POST JSON  {"sender":"…","sender_fmt":"…","message":"…","timestamp":"…","device":"…","receiver":"…"}
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushPostJson(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  HTTPClient http;
  http.begin(ch.url);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"sender\":\""      + jsonEscape(sender)
              + "\",\"sender_fmt\":\"" + jsonEscape(formatPhoneNumber(sender))
              + "\",\"message\":\""   + jsonEscape(msg)
              + "\",\"timestamp\":\"" + jsonEscape(formatTimestamp(ts))
              + "\",\"device\":\""    + jsonEscape(dev)
              + "\",\"receiver\":\""  + jsonEscape(dev)
              + "\"}";
  Serial.println("[PushPostJson] " + body);
  int code = http.POST(body);
  http.end();
  return code;
}


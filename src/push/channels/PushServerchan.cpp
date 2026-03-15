// Server酱  POST form-encoded title+desp
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushServerchan(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String url = ch.url.length() > 0 ? ch.url
             : ("https://sctapi.ftqq.com/" + ch.key1 + ".send");
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "title=" + urlEncode("短信来自: " + formatPhoneNumber(sender))
              + "&desp=" + urlEncode("**发送者:** " + formatPhoneNumber(sender)
                + "\n\n**接收卡号:** " + String(dev)
                + "\n\n**时间:** " + formatTimestamp(ts)
                + "\n\n**内容:**\n\n" + String(msg));
  Serial.println("[PushServerchan] " + url);
  int code = http.POST(body);
  http.end();
  return code;
}


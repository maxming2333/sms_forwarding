// PushPlus  POST JSON with token + channel
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushPushplus(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String url = ch.url.length() > 0 ? ch.url : "http://www.pushplus.plus/send";
  String channelVal = "wechat";
  if (ch.key2 == "wechat" || ch.key2 == "extension" || ch.key2 == "app")
    channelVal = ch.key2;

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"token\":\"" + ch.key1
    + "\",\"title\":\"短信来自: " + jsonEscape(formatPhoneNumber(sender))
    + "\",\"content\":\"<b>发送者:</b> " + jsonEscape(formatPhoneNumber(sender))
    + "<br><b>接收卡号:</b> " + jsonEscape(dev)
    + "<br><b>时间:</b> " + jsonEscape(formatTimestamp(ts))
    + "<br><b>内容:</b><br>" + jsonEscape(msg)
    + "\",\"channel\":\"" + channelVal + "\"}";
  Serial.println("[PushPlus] " + body);
  int code = http.POST(body);
  http.end();
  return code;
}


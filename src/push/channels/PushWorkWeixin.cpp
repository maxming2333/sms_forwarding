// Work WeiXin (企业微信) robot  POST text message
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

int pushWorkWeixin(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String content = "📱短信通知\\n发送者: " + jsonEscape(formatPhoneNumber(sender))
                 + "\\n接收卡号: "          + jsonEscape(dev)
                 + "\\n内容: "              + jsonEscape(msg)
                 + "\\n时间: "              + jsonEscape(formatTimestamp(ts));
  String body = "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + content + "\"}}";
  Serial.println("[PushWorkWeixin] " + body);
  HTTPClient http;
  http.begin(ch.url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code;
}


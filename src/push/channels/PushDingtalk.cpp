// DingTalk robot  POST text message with optional HMAC-SHA256 signature
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>

static int _dingtalkPost(const String& webhookUrl, const String& textContent) {
  HTTPClient http;
  http.begin(webhookUrl);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + textContent + "\"}}";
  Serial.println("[PushDingtalk] " + body);
  int code = http.POST(body);
  http.end();
  return code;
}

int pushDingtalk(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String webhookUrl = ch.url;
  if (ch.key1.length() > 0) {
    int64_t t  = getUtcMillis();
    String sign = dingtalkSign(ch.key1, t);
    char buf[21]; snprintf(buf, sizeof(buf), "%lld", t);
    webhookUrl += (webhookUrl.indexOf('?') == -1 ? "?" : "&");
    webhookUrl += "timestamp=" + String(buf) + "&sign=" + sign;
  }

  String content = "📱短信通知\\n发送者: " + jsonEscape(formatPhoneNumber(sender))
                 + "\\n接收卡号: "         + jsonEscape(dev)
                 + "\\n内容: "             + jsonEscape(msg)
                 + "\\n时间: "             + jsonEscape(formatTimestamp(ts));
  int code = _dingtalkPost(webhookUrl, content);

  // Auto-extract and separately push verification codes
  String msgStr = String(msg);
  if (msgStr.indexOf("验证码") != -1 || msgStr.indexOf("验证") != -1) {
    String code_str = extractVerifyCode(msg);
    if (code_str.length() > 0) {
      Serial.println("[PushDingtalk] 检测到验证码: " + code_str);
      _dingtalkPost(webhookUrl, code_str);
    }
  }
  return code;
}


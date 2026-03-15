// Feishu (Lark) robot  POST text message with optional HMAC-SHA256 signature
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <base64.h>

int pushFeishu(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String jsonBody = "{";
  if (ch.key1.length() > 0) {
    int64_t t = (int64_t)time(nullptr);
    String str2sign = String(t) + "\n" + ch.key1;
    uint8_t result[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)ch.key1.c_str(), ch.key1.length());
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)str2sign.c_str(), str2sign.length());
    mbedtls_md_hmac_finish(&ctx, result);
    mbedtls_md_free(&ctx);
    String sign = base64::encode(result, 32);
    jsonBody += "\"timestamp\":\"" + String(t) + "\",\"sign\":\"" + sign + "\",";
  }
  String content = "📱短信通知\\n发送者: " + jsonEscape(formatPhoneNumber(sender))
                 + "\\n接收卡号: "         + jsonEscape(dev)
                 + "\\n内容: "             + jsonEscape(msg)
                 + "\\n时间: "             + jsonEscape(formatTimestamp(ts));
  jsonBody += "\"msg_type\":\"text\",\"content\":{\"text\":\"" + content + "\"}}";

  Serial.println("[PushFeishu] " + jsonBody);
  HTTPClient http;
  http.begin(ch.url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonBody);
  http.end();
  return code;
}


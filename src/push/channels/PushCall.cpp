// Unified incoming-call notification dispatcher.
// Handles customCallBody template first; falls back to per-type built-in format.
// Placeholder support: {caller} {caller_fmt} {timestamp} {receiver}
#include "PushChannels.h"
#include "utils/Utils.h"
#include <HTTPClient.h>
#include <mbedtls/md.h>
#include <base64.h>
#include <time.h>

// ── Dingtalk signed URL helper (reuse from PushDingtalk.cpp logic) ─────────
static String dtSignUrl(const PushChannel& ch) {
  String url = ch.url;
  if (ch.key1.length() > 0) {
    int64_t ts = getUtcMillis();
    String  sign = dingtalkSign(ch.key1, ts);
    char buf[21]; snprintf(buf, sizeof(buf), "%lld", ts);
    url += (url.indexOf('?') == -1 ? "?" : "&");
    url += "timestamp=" + String(buf) + "&sign=" + sign;
  }
  return url;
}

// ── Simple HTTP POST helper ───────────────────────────────────────────────────
static int postJson(const String& url, const String& body) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  Serial.println("[PushCall] POST " + url + " body=" + body);
  int code = http.POST(body);
  http.end();
  return code;
}

// ── Main call notification dispatcher ────────────────────────────────────────
int pushCall(const PushChannel& ch, const char* caller, const char* ts, const char* dev) {
  if (!ch.enabled) return -1;

  String callerEsc  = jsonEscape(String(caller));
  String callerFmt  = jsonEscape(formatPhoneNumber(caller));
  String tsEsc      = jsonEscape(String(ts));
  String devEsc     = jsonEscape(String(dev));

  // ── Custom call template takes priority for all types ─────────────────────
  if (ch.customCallBody.length() > 0) {
    String url  = ch.url;
    // Dingtalk still needs signing even when using custom body
    if (ch.type == PUSH_TYPE_DINGTALK) url = dtSignUrl(ch);
    // PushPlus / Server酱 default URLs
    if (ch.type == PUSH_TYPE_PUSHPLUS  && url.length() == 0) url = "http://www.pushplus.plus/send";
    if (ch.type == PUSH_TYPE_SERVERCHAN && url.length() == 0) url = "https://sctapi.ftqq.com/" + ch.key1 + ".send";

    if (url.length() == 0) return -1;

    String body = ch.customCallBody;
    body.replace("{caller}",     callerEsc);
    body.replace("{caller_fmt}", callerFmt);
    body.replace("{sender}",     callerEsc);   // alias for convenience
    body.replace("{timestamp}",  tsEsc);
    body.replace("{receiver}",   devEsc);
    return postJson(url, body);
  }

  // ── Built-in formats per push type ────────────────────────────────────────
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON: {
      String body = "{\"type\":\"call\""
        ",\"receiver\":\""  + devEsc    + "\""
        ",\"caller\":\""    + callerEsc + "\""
        ",\"caller_fmt\":\"" + callerFmt + "\""
        ",\"message\":\"来电通知\""
        ",\"timestamp\":\"" + tsEsc    + "\"}";
      return postJson(ch.url, body);
    }
    case PUSH_TYPE_BARK: {
      String body = "{\"title\":\"📞 来电通知\""
        ",\"body\":\"来电号码: " + callerFmt + "\\n时间: " + tsEsc + "\"}";
      return postJson(ch.url, body);
    }
    case PUSH_TYPE_GET: {
      String url = ch.url + (ch.url.indexOf('?') == -1 ? "?" : "&");
      url += "type=call";
      url += "&caller=" + urlEncode(String(caller));
      url += "&timestamp=" + urlEncode(String(ts));
      url += "&receiver=" + urlEncode(String(dev));
      Serial.println("[PushCall] GET " + url);
      HTTPClient http; http.begin(url);
      int code = http.GET(); http.end();
      return code;
    }
    case PUSH_TYPE_DINGTALK: {
      String content = "SIM: " + devEsc + "\\n"
                     + "📞来电通知\\n来电号码: " + callerFmt
                     + "\\n时间: " + tsEsc;
      String body = "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + content + "\"}}";
      return postJson(dtSignUrl(ch), body);
    }
    case PUSH_TYPE_PUSHPLUS: {
      String url  = ch.url.length() > 0 ? ch.url : "http://www.pushplus.plus/send";
      String body = "{\"token\":\"" + ch.key1
        + "\",\"title\":\"📞 来电: " + callerFmt
        + "\",\"content\":\"<b>来电号码:</b> " + callerFmt
        + "<br><b>接收卡号:</b> " + devEsc
        + "<br><b>时间:</b> " + tsEsc + "\"}";
      return postJson(url, body);
    }
    case PUSH_TYPE_SERVERCHAN: {
      String url = ch.url.length() > 0 ? ch.url : ("https://sctapi.ftqq.com/" + ch.key1 + ".send");
      HTTPClient http; http.begin(url);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String postData = "title=" + urlEncode("📞 来电: " + formatPhoneNumber(caller))
                      + "&desp=" + urlEncode("**来电号码:** " + formatPhoneNumber(caller)
                        + "\n\n**接收卡号:** " + String(dev)
                        + "\n\n**时间:** " + String(ts));
      int code = http.POST(postData); http.end();
      return code;
    }
    case PUSH_TYPE_FEISHU: {
      String content = "📞来电通知\\n来电号码: " + callerFmt
                     + "\\n接收卡号: " + devEsc
                     + "\\n时间: " + tsEsc;
      String jsonBody = "{";
      if (ch.key1.length() > 0) {
        int64_t t = (int64_t)time(nullptr);
        // Feishu sign uses seconds (not ms)
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
      jsonBody += "\"msg_type\":\"text\",\"content\":{\"text\":\"" + content + "\"}}";
      return postJson(ch.url, jsonBody);
    }
    case PUSH_TYPE_GOTIFY: {
      String url = ch.url;
      if (!url.endsWith("/")) url += "/";
      url += "message?token=" + ch.key1;
      String body = "{\"title\":\"📞 来电: " + callerFmt
        + "\",\"message\":\"接收卡号: " + devEsc
        + "\\n\\n时间: " + tsEsc
        + "\",\"priority\":5}";
      return postJson(url, body);
    }
    case PUSH_TYPE_TELEGRAM: {
      String base = ch.url.length() > 0 ? ch.url : "https://api.telegram.org";
      if (base.endsWith("/")) base.remove(base.length() - 1);
      String url  = base + "/bot" + ch.key2 + "/sendMessage";
      String text = "📞来电通知\n来电号码: " + formatPhoneNumber(caller)
                  + "\n接收卡号: " + String(dev)
                  + "\n时间: " + String(ts);
      String body = "{\"chat_id\":\"" + ch.key1
                  + "\",\"text\":\""  + jsonEscape(text) + "\"}";
      return postJson(url, body);
    }
    case PUSH_TYPE_WORK_WEIXIN: {
      String content = "📞来电通知\\n来电号码: " + callerFmt
                     + "\\n接收卡号: " + devEsc
                     + "\\n时间: " + tsEsc;
      String body = "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + content + "\"}}";
      return postJson(ch.url, body);
    }
    case PUSH_TYPE_SMS: {
      // Forward call notification as an SMS to the target number
      // (reuse the SMS forward channel's target phone stored in url field)
      String phone = ch.url; phone.trim();
      if (phone.length() == 0) return -1;
      // We can't easily call smsSend() from here without a circular dep,
      // so build a minimal notification text and return -1 to skip silently.
      // Users who want SMS call notification should use customCallBody.
      Serial.println("[PushCall] SMS通道暂不支持来电转发，请使用自定义模板");
      return -1;
    }
    case PUSH_TYPE_CUSTOM:
      // No custom call body was set, skip
      Serial.println("[PushCall] 自定义通道无来电模板，跳过");
      return -1;
    default:
      return -1;
  }
}



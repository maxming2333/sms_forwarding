#include "push_channels.h"
#include "../logger/logger.h"
#include "sms/sms.h"
#include "../utils/http.h"
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <base64.h>

// ---------- helpers ----------

static String urlEncode(const String& str) {
  String encoded;
  for (unsigned int i = 0; i < str.length(); i++) {
    unsigned char c = (unsigned char)str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

static String computeHmacSha256Base64(const String& key, const String& data) {
  uint8_t hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  return base64::encode(hmacResult, 32);
}

static String dingtalkSign(const String& secret, int64_t timestamp) {
  return urlEncode(computeHmacSha256Base64(secret, String(timestamp) + "\n" + secret));
}

static int64_t getUtcMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  }
  return (int64_t)time(nullptr) * 1000LL;
}

// 上次 HTTP 请求完成的时刻（millis()）；0 表示尚未发送过任何请求。
static unsigned long s_lastHttpEndMs = 0;

// 等待冷却间隔后创建 HttpSession，给 TCP/TLS 栈足够的资源释放时间。
// 失败（连接无法建立）时返回 nullptr。
static std::unique_ptr<HttpSession> request(const String& url) {
  unsigned long now = millis();
  if (s_lastHttpEndMs > 0 && now - s_lastHttpEndMs < HTTP_COOLDOWN_MS) {
    delay(HTTP_COOLDOWN_MS - (now - s_lastHttpEndMs));
  }
  return std::unique_ptr<HttpSession>(HttpSession::request(url));
}

// 记录响应结果并更新冷却时间戳；失败时额外打印首 120 字节响应 body，便于定位原因。
static bool isResponseSuccessful(HttpSession* session, int code) {
  if (code >= 200 && code < 300) {
    LOG("PUSHCH", "响应码: %d 成功", code);
  } else {
    String resp = session->http()->getString();
    if (resp.length() > 120) resp = resp.substring(0, 120) + "...";
    LOG("PUSHCH", "响应码: %d 失败，响应体: %s", code, resp.c_str());
  }
  s_lastHttpEndMs = millis();
  return code >= 200 && code < 300;
}

// ---------- channel implementations ----------

bool PushChannels::sendPostJson(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  auto session = request(ch.url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  String body;
  if (message.type == PUSH_BODY_CUSTOM) {
    body = message.content;
  } else {
    JsonDocument doc;
    doc["sender"]    = sender;
    doc["message"]   = message.content;
    doc["timestamp"] = timestamp;
    serializeJson(doc, body);
  }

  LOG("PUSHCH", "POST JSON to %s: %s", ch.url.c_str(), body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendBark(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  auto session = request(ch.url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  JsonDocument doc;
  // key1 已由调用方渲染，非空时作为自定义标题，否则回退到发件人号码
  String title = ch.key1.length() > 0 ? ch.key1 : sender;
  doc["title"] = title;
  doc["body"]  = message.content;
  String body;
  serializeJson(doc, body);

  LOG("PUSHCH", "Bark to %s: %s", ch.url.c_str(), body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendGet(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url;
  url += (url.indexOf('?') == -1) ? "?" : "&";
  url += "sender=" + urlEncode(sender);
  url += "&message=" + urlEncode(message.content);
  url += "&timestamp=" + urlEncode(timestamp);

  LOG("PUSHCH", "GET %s", url.c_str());
  auto session = request(url);
  if (!session) return false;
  int code = session->http()->GET();
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendDingtalk(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String webhookUrl = ch.url;

  if (ch.key1.length() > 0) {
    int64_t ts = getUtcMillis();
    String sign = dingtalkSign(ch.key1, ts);
    webhookUrl += (webhookUrl.indexOf('?') == -1) ? "?" : "&";
    char tsBuf[21];
    snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
    webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + sign;
  }

  auto session = request(webhookUrl);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  String content = message.type == PUSH_BODY_CUSTOM ? message.content : ("📱短信通知\n发送者: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);

  JsonDocument doc;
  doc["msgtype"] = "text";
  doc["text"]["content"] = content;
  String body;
  serializeJson(doc, body);

  LOG("PUSHCH", "DingTalk: %s", body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendPushPlus(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url.length() > 0 ? ch.url : "http://www.pushplus.plus/send";
  auto session = request(url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  String channelValue = "wechat";
  if (ch.key2.length() > 0) {
    if (ch.key2 == "wechat" || ch.key2 == "extension" || ch.key2 == "app") {
      channelValue = ch.key2;
    } else {
      LOG("PUSHCH", "Invalid PushPlus channel '%s'. Using default 'wechat'.", ch.key2.c_str());
    }
  }

  String content = message.type == PUSH_BODY_CUSTOM ? message.content : ("<b>发送者:</b> " + sender + "<br><b>时间:</b> " + timestamp + "<br><b>内容:</b><br>" + message.content);

  JsonDocument doc;
  doc["token"]   = ch.key1;
  doc["title"]   = "短信来自: " + sender;
  doc["content"] = content;
  doc["channel"] = channelValue;
  String body;
  serializeJson(doc, body);

  LOG("PUSHCH", "PushPlus: %s", body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendServerChan(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url.length() > 0 ? ch.url : ("https://sctapi.ftqq.com/" + ch.key1 + ".send");
  auto session = request(url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/x-www-form-urlencoded");

  String desp = message.type == PUSH_BODY_CUSTOM ? message.content : ("**发送者:** " + sender + "\n\n**时间:** " + timestamp + "\n\n**内容:**\n\n" + message.content);
  String postData = "title=" + urlEncode("短信来自: " + sender);
  postData += "&desp=" + urlEncode(desp);

  LOG("PUSHCH", "Server酱: %s", postData.c_str());
  int code = session->http()->POST(postData);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendCustom(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  // 类型7（POST请求）：使用 message.content（可为空，FR-008: 留空时发送空 POST body）
  auto session = request(ch.url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  String body = message.content;
  LOG("PUSHCH", "POST请求: %s，body长度: %d", ch.url.c_str(), body.length());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendFeishu(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  auto session = request(ch.url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  JsonDocument doc;

  if (ch.key1.length() > 0) {
    int64_t ts = time(nullptr);
    doc["timestamp"] = String(ts);
    doc["sign"]      = computeHmacSha256Base64(ch.key1, String(ts) + "\n" + ch.key1);
  }

  String text = message.type == PUSH_BODY_CUSTOM ? message.content : ("📱短信通知\n发送者: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);
  doc["msg_type"]        = "text";
  doc["content"]["text"] = text;

  String body;
  serializeJson(doc, body);

  LOG("PUSHCH", "飞书: %s", body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendGotify(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String url = ch.url;
  if (!url.endsWith("/")) url += "/";
  url += "message?token=" + ch.key1;
  auto session = request(url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  String msg = message.type == PUSH_BODY_CUSTOM ? message.content : (message.content + "\n\n时间: " + timestamp);
  JsonDocument doc;
  doc["title"]    = "短信来自: " + sender;
  doc["message"]  = msg;
  doc["priority"] = 5;
  String body;
  serializeJson(doc, body);

  LOG("PUSHCH", "Gotify: %s", body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendTelegram(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String baseUrl = ch.url.length() > 0 ? ch.url : "https://api.telegram.org";
  if (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);
  String url = baseUrl + "/bot" + ch.key2 + "/sendMessage";
  auto session = request(url);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  String text = message.type == PUSH_BODY_CUSTOM ? message.content : ("📱短信通知\n发送者: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);
  JsonDocument doc;
  doc["chat_id"] = ch.key1;
  doc["text"]    = text;
  String body;
  serializeJson(doc, body);

  LOG("PUSHCH", "Telegram: %s", body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendWechatWork(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String webhookUrl = ch.url;

  if (ch.key1.length() > 0) {
    int64_t ts = getUtcMillis();
    char tsBuf[21];
    snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
    webhookUrl += (webhookUrl.indexOf('?') == -1) ? "?" : "&";
    webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + urlEncode(computeHmacSha256Base64(ch.key1, String(tsBuf) + "\n" + ch.key1));
  }

  auto session = request(webhookUrl);
  if (!session) return false;
  session->http()->addHeader("Content-Type", "application/json");

  String content = message.type == PUSH_BODY_CUSTOM ? message.content : ("📱短信通知\n发件人: " + sender + "\n内容: " + message.content + "\n时间: " + timestamp);
  JsonDocument doc;
  doc["msgtype"] = "text";
  doc["text"]["content"] = content;
  String body;
  serializeJson(doc, body);

  LOG("PUSHCH", "企业微信: %s", body.c_str());
  int code = session->http()->POST(body);
  return isResponseSuccessful(session.get(), code);
}

bool PushChannels::sendSmsPush(const PushChannel& ch, const String& sender, const PushBody& message, const String& timestamp) {
  String content = message.type == PUSH_BODY_CUSTOM ? message.content : ("[转发]发件人: " + sender + "\n内容: " + message.content);
  // Sms::sendPDU 内部自动处理长短信拆分，无需手动截断
  LOG("PUSHCH", "SMS备份推送到: %s", ch.url.c_str());
  bool ok = Sms::sendPDU(ch.url.c_str(), content.c_str());
  if (!ok) LOG("PUSHCH", "SMS备份推送失败");
  return ok;
}

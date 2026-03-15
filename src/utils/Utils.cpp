#include "Utils.h"

// ── JSON escape ───────────────────────────────────────────────────────────────
String jsonEscape(const String& str) {
  String r;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    switch (c) {
      case '"':  r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n";  break;
      case '\r': r += "\\r";  break;
      case '\t': r += "\\t";  break;
      default:   r += c;
    }
  }
  return r;
}

// ── URL encode ────────────────────────────────────────────────────────────────
String urlEncode(const String& str) {
  String enc;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == ' ') {
      enc += '+';
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      enc += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      enc += buf;
    }
  }
  return enc;
}

// ── Timestamp formatter ───────────────────────────────────────────────────────
// PDU timestamp format: "YYMMDDHHMMSS[TZ]" — e.g. "260122204818"
// Output: "2026-01-22 20:48:18"
String formatTimestamp(const char* ts) {
  if (!ts || strlen(ts) < 12) return String(ts ? ts : "");
  char out[20];
  snprintf(out, sizeof(out), "20%.2s-%.2s-%.2s %.2s:%.2s:%.2s",
           ts, ts+2, ts+4, ts+6, ts+8, ts+10);
  return String(out);
}

// ── UTC milliseconds ──────────────────────────────────────────────────────────
int64_t getUtcMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) == 0)
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  return (int64_t)time(nullptr) * 1000LL;
}

// ── DingTalk HMAC-SHA256 sign ─────────────────────────────────────────────────
String dingtalkSign(const String& secret, int64_t timestampMs) {
  String str2sign = String(timestampMs) + "\n" + secret;
  uint8_t result[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)secret.c_str(), secret.length());
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)str2sign.c_str(), str2sign.length());
  mbedtls_md_hmac_finish(&ctx, result);
  mbedtls_md_free(&ctx);
  return urlEncode(base64::encode(result, 32));
}

// ── Verification code extractor ───────────────────────────────────────────────
String extractVerifyCode(const char* text) {
  if (!text) return "";
  String cur;
  for (int i = 0; text[i]; i++) {
    if (text[i] >= '0' && text[i] <= '9') {
      cur += text[i];
    } else {
      if (cur.length() >= 4) return cur;
      cur = "";
    }
  }
  return (cur.length() >= 4) ? cur : "";
}

// ── Phone number country lookup ───────────────────────────────────────────────
String getCountryByCode(const String& number) {
  if (!number.startsWith("+")) return "";
  // 3-digit codes first (to avoid mis-matching 2-digit prefixes)
  if (number.startsWith("+886")) return "中国台湾";
  if (number.startsWith("+852")) return "中国香港";
  if (number.startsWith("+853")) return "中国澳门";
  if (number.startsWith("+971")) return "阿联酋";
  if (number.startsWith("+966")) return "沙特";
  if (number.startsWith("+353")) return "爱尔兰";
  if (number.startsWith("+351")) return "葡萄牙";
  if (number.startsWith("+234")) return "尼日利亚";
  if (number.startsWith("+380")) return "乌克兰";
  if (number.startsWith("+420")) return "捷克";
  if (number.startsWith("+372")) return "爱沙尼亚";
  // 2-digit codes
  if (number.startsWith("+86")) return "中国";
  if (number.startsWith("+81")) return "日本";
  if (number.startsWith("+82")) return "韩国";
  if (number.startsWith("+44")) return "英国";
  if (number.startsWith("+49")) return "德国";
  if (number.startsWith("+33")) return "法国";
  if (number.startsWith("+39")) return "意大利";
  if (number.startsWith("+61")) return "澳大利亚";
  if (number.startsWith("+91")) return "印度";
  if (number.startsWith("+65")) return "新加坡";
  if (number.startsWith("+60")) return "马来西亚";
  if (number.startsWith("+66")) return "泰国";
  if (number.startsWith("+84")) return "越南";
  if (number.startsWith("+63")) return "菲律宾";
  if (number.startsWith("+62")) return "印尼";
  if (number.startsWith("+55")) return "巴西";
  if (number.startsWith("+34")) return "西班牙";
  if (number.startsWith("+31")) return "荷兰";
  if (number.startsWith("+46")) return "瑞典";
  if (number.startsWith("+41")) return "瑞士";
  if (number.startsWith("+48")) return "波兰";
  if (number.startsWith("+90")) return "土耳其";
  if (number.startsWith("+20")) return "埃及";
  if (number.startsWith("+27")) return "南非";
  if (number.startsWith("+52")) return "墨西哥";
  if (number.startsWith("+54")) return "阿根廷";
  if (number.startsWith("+64")) return "新西兰";
  // 1-digit codes
  if (number.startsWith("+1")) return "美国/加拿大";
  if (number.startsWith("+7")) return "俄罗斯";
  return "";
}

int getCountryCodeLength(const String& number) {
  if (!number.startsWith("+")) return 0;
  // 3-digit codes
  if (number.startsWith("+886") || number.startsWith("+852") || number.startsWith("+853") ||
      number.startsWith("+971") || number.startsWith("+966") || number.startsWith("+353") ||
      number.startsWith("+351") || number.startsWith("+234") || number.startsWith("+380") ||
      number.startsWith("+420") || number.startsWith("+372")) return 3;
  // 2-digit codes
  if (number.startsWith("+86") || number.startsWith("+81") || number.startsWith("+82") ||
      number.startsWith("+44") || number.startsWith("+49") || number.startsWith("+33") ||
      number.startsWith("+39") || number.startsWith("+61") || number.startsWith("+91") ||
      number.startsWith("+65") || number.startsWith("+60") || number.startsWith("+66") ||
      number.startsWith("+84") || number.startsWith("+63") || number.startsWith("+62") ||
      number.startsWith("+55") || number.startsWith("+34") || number.startsWith("+31") ||
      number.startsWith("+46") || number.startsWith("+41") || number.startsWith("+48") ||
      number.startsWith("+90") || number.startsWith("+20") || number.startsWith("+27") ||
      number.startsWith("+52") || number.startsWith("+54") || number.startsWith("+64")) return 2;
  // 1-digit codes
  if (number.startsWith("+1") || number.startsWith("+7")) return 1;
  return 0;
}

String formatPhoneNumber(const char* number) {
  String num = String(number);
  if (!num.startsWith("+")) return num;
  String country = getCountryByCode(num);
  if (country.length() == 0) return num;
  int codeLen = getCountryCodeLength(num);
  if (codeLen == 0) return num;
  String areaCode = num.substring(0, codeLen + 1);  // includes '+'
  String localNum = num.substring(codeLen + 1);
  return country + "(" + areaCode + ") " + localNum;
}


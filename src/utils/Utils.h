#pragma once
#include <Arduino.h>
#include <mbedtls/md.h>
#include <base64.h>

// JSON / URL helpers
String jsonEscape(const String& str);
String urlEncode(const String& str);

// Time helpers
// ts is PDU timestamp "YYMMDDHHMMSS" → "YYYY-MM-DD HH:MM:SS"
String  formatTimestamp(const char* ts);
int64_t getUtcMillis();                    // UTC timestamp in milliseconds

// Crypto helpers
String dingtalkSign(const String& secret, int64_t timestampMs);

// SMS helpers
String extractVerifyCode(const char* text);  // extract 4+ digit code

// Phone number formatting helpers
// Returns country name for an international number (e.g. "+8613800138000" → "中国")
String getCountryByCode(const String& number);
// Returns length of numeric country code digits (without "+"), e.g. "+86…" → 2
int    getCountryCodeLength(const String& number);
// Formats an international number for display, e.g. "+8613812345678" → "中国(+86) 13812345678"
// Returns the original string if no country code is recognised.
String formatPhoneNumber(const char* number);


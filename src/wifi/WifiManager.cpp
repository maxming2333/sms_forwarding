#include "WifiManager.h"
#include "config/AppConfig.h"

bool timeSynced = false;

// ── WiFi init ─────────────────────────────────────────────────────────────────
void wifiInit(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str(), 0, nullptr, true);
  Serial.print("正在连接WiFi: ");
  Serial.println(ssid);

  unsigned long t = millis();
  bool connected  = false;
  while (millis() - t < 30000) {
    if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (connected) {
    Serial.println("WiFi连接成功，IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi连接超时，启动AP热点...");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SMS-Forwarder-AP");
    Serial.println("AP IP: " + WiFi.softAPIP().toString());
  }
}

// ── NTP ───────────────────────────────────────────────────────────────────────
void ntpSync() {
  Serial.println("正在同步NTP时间（UTC+8）...");
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");
  int retry = 0;
  while (time(nullptr) < 100000 && retry++ < 100) delay(100);
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    struct tm ti; getLocalTime(&ti);
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    Serial.println("NTP同步成功，本地时间=" + String(buf));
  } else {
    Serial.println("NTP同步失败，使用设备时间");
  }
}

// ── Helpers ───────────────────────────────────────────────────────────────────
String getDeviceUrl() {
  return "http://" + WiFi.localIP().toString() + "/";
}



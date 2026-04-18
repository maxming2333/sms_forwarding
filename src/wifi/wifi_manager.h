#pragma once
#include <Arduino.h>

enum WiFiMode {
  WIFI_MODE_UNINITIALIZED,
  WIFI_MODE_STA_CONNECTED,
  WIFI_MODE_AP_ACTIVE,
  WIFI_MODE_STA_FAILED,
  WIFI_MODE_RECONNECTING
};

// WiFi 重连任务常量
constexpr int WIFI_RECONNECT_ATTEMPTS_PER_SSID = 3;
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
constexpr int WIFI_RECONNECT_TASK_PRIORITY = 2;
constexpr int WIFI_RECONNECT_TASK_STACK = 3072;

// 重连成功回调类型
typedef void (*WifiReconnectCallback)();

void wifiManagerInit();
WiFiMode wifiManagerGetMode();
String wifiManagerGetIP();
String getDeviceUrl();

void wifiManagerSetReconnectCallback(WifiReconnectCallback cb);
void wifiManagerAbortReconnect();

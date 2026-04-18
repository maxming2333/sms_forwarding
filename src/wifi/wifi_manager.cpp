#include "wifi_manager.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config/config.h"
#include "logger.h"

static WiFiMode s_mode = WIFI_MODE_UNINITIALIZED;

// T008: 重连状态变量
static bool              s_everConnected  = false;
static volatile bool     s_abortReconnect = false;
static TaskHandle_t      s_reconnectTask  = nullptr;
static WifiReconnectCallback s_reconnectCb = nullptr;

static void enterAPMode() {
  WiFi.softAP("SMS-Forwarder-AP");
  s_mode = WIFI_MODE_AP_ACTIVE;
  LOG("WiFi", "AP模式启动，SSID: SMS-Forwarder-AP，IP: 192.168.4.1");
}

// T011: WiFi 持久重连 FreeRTOS 任务
static void wifiReconnectTask(void*) {
  s_mode = WIFI_MODE_RECONNECTING;
  LOG("WiFi", "重连任务启动，轮询 %d 条 SSID", config.wifiCount);

  for (int w = 0; w < config.wifiCount; w++) {
    if (config.wifiList[w].ssid.length() == 0) continue;

    const char* ssid = config.wifiList[w].ssid.c_str();
    const char* pass = config.wifiList[w].password.c_str();

    for (int attempt = 1; attempt <= WIFI_RECONNECT_ATTEMPTS_PER_SSID; attempt++) {
      if (s_abortReconnect) {
        LOG("WiFi", "重连任务被中止");
        s_reconnectTask = nullptr;
        vTaskDelete(nullptr);
        return;
      }

      LOG("WiFi", "重连尝试 SSID: %s，第 %d/%d 次", ssid, attempt, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
      WiFi.begin(ssid, pass, 0, nullptr, true);

      unsigned long start = millis();
      while (millis() - start < WIFI_RECONNECT_INTERVAL_MS) {
        if (s_abortReconnect) {
          WiFi.disconnect(true);
          LOG("WiFi", "重连任务被中止（等待期间）");
          s_reconnectTask = nullptr;
          vTaskDelete(nullptr);
          return;
        }
        if (WiFi.status() == WL_CONNECTED) {
          s_mode = WIFI_MODE_STA_CONNECTED;
          LOG("WiFi", "重连成功，SSID: %s，IP: %s", ssid, WiFi.localIP().toString().c_str());
          if (s_reconnectCb) s_reconnectCb();
          s_reconnectTask = nullptr;
          vTaskDelete(nullptr);
          return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
      }

      LOG("WiFi", "重连超时，SSID: %s，第 %d/%d 次", ssid, attempt, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }

  LOG("WiFi", "所有 SSID 重连均失败，保持当前状态（不切换 AP 模式）");
  s_reconnectTask = nullptr;
  vTaskDelete(nullptr);
}

void wifiManagerInit() {
  // 软重启（ESP.restart）后 WiFi 驱动状态可能残留，强制关闭后再初始化
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  if (config.wifiCount == 0) {
    LOG("WiFi", "未配置任何WiFi，直接进入AP模式");
    enterAPMode();
    return;
  }

  // 扫描所有信道以连接信号最强的 AP，防止在 mesh 组网这类场景中连接到弱 AP
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);

  for (int w = 0; w < config.wifiCount; w++) {
    if (config.wifiList[w].ssid.length() == 0) continue;

    const char* ssid = config.wifiList[w].ssid.c_str();
    const char* pass = config.wifiList[w].password.c_str();
    LOG("WiFi", "尝试第 %d/%d 条WiFi: %s", w + 1, config.wifiCount, ssid);

    for (int attempt = 1; attempt <= 5; attempt++) {
      WiFi.begin(ssid, pass, 0, nullptr, true);

      unsigned long start = millis();
      while (millis() - start < 3000) {
        if (WiFi.status() == WL_CONNECTED) {
          s_mode = WIFI_MODE_STA_CONNECTED;
          // T009: 标记曾成功连接
          s_everConnected = true;
          LOG("WiFi", "第 %d/%d 条WiFi第 %d/5 次连接成功，IP: %s", w + 1, config.wifiCount, attempt, WiFi.localIP().toString().c_str());
          // T010: 注册断线事件回调
          WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
            if (!s_everConnected) return;
            if (s_reconnectTask != nullptr) return;  // 已有重连任务在跑
            s_abortReconnect = false;
            LOG("WiFi", "检测到WiFi断线，启动重连任务");
            xTaskCreate(wifiReconnectTask, "wifi_reconnect",
                        WIFI_RECONNECT_TASK_STACK, nullptr,
                        WIFI_RECONNECT_TASK_PRIORITY, &s_reconnectTask);
          }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
          return;
        }
        delay(100);
      }

      LOG("WiFi", "第 %d/%d 条WiFi第 %d/5 次连接超时", w + 1, config.wifiCount, attempt);
      WiFi.disconnect(true);
      delay(500);
    }
  }

  LOG("WiFi", "所有WiFi条目全部失败，切换到AP模式");
  enterAPMode();
}

WiFiMode wifiManagerGetMode() {
  return s_mode;
}

String wifiManagerGetIP() {
  if (s_mode == WIFI_MODE_STA_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return IPAddress(192, 168, 4, 1).toString();
}

String getDeviceUrl() {
  return "http://" + wifiManagerGetIP() + "/";
}

// T012: 中止重连任务
void wifiManagerAbortReconnect() {
  s_abortReconnect = true;
}

// T012: 设置重连成功回调
void wifiManagerSetReconnectCallback(WifiReconnectCallback cb) {
  s_reconnectCb = cb;
}

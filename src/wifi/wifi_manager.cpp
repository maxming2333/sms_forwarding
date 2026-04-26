#include "wifi_manager.h"
#include "ble/blufi_handler.h"
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include "config/config.h"
#include "logger.h"

static WiFiMode s_mode = WIFI_MODE_UNINITIALIZED;

static bool                  s_everConnected = false;
static bool                  s_initDone      = false;  // 初始化完成标志：STA 获取到 IP 或已进入 AP 模式
static WifiReconnectCallback s_reconnectCb   = nullptr;

// 轮询重连状态机
enum ReconnState {
  RECONNECT_IDLE,
  RECONNECT_WAITING,
  RECONNECT_CONNECTING,
  RECONNECT_AP_SCAN_WAIT  // 等待异步扫描完成（AP 模式后台扫描专用）
};

static ReconnState   s_reconnState    = RECONNECT_IDLE;
static int           s_reconnWIdx     = 0;
static int           s_reconnAttempt  = 0;
static unsigned long s_lastAttemptMs  = 0;
static unsigned long s_apRescanNextMs = 0;     // 下次 AP 后台扫描触发时间戳
static bool          s_reconnFromAP   = false;  // 本次重连是否来自 AP 恢复

static void enterAPMode() {
  WiFi.softAP(kApSsid);
  // AP 模式下必须启用 Modem Sleep，否则 WiFi 持续占用射频，BLE 无法发送广播包
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  s_mode    = WIFI_MODE_AP_ACTIVE;
  s_initDone = true;
  LOG("WiFi", "AP模式启动，SSID: %s，IP: 192.168.4.1", kApSsid);
  blufiInit();
}

void wifiManagerInit() {
  // 重置重连状态机（支持重复调用）
  s_reconnState   = RECONNECT_IDLE;
  s_reconnWIdx    = 0;
  s_reconnAttempt = 0;
  s_lastAttemptMs = 0;
  s_everConnected = false;
  s_initDone      = false;

  // 软重启（ESP.restart）后 WiFi 驱动状态可能残留，强制关闭后再初始化
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
  esp_task_wdt_reset();

  if (config.wifiCount == 0) {
    LOG("WiFi", "未配置任何WiFi，直接进入AP模式");
    enterAPMode();
    return;
  }

  // 设置客户端主机名（路由器 DHCP 列表中显示的名称），格式：SMS-Forwarder-XXXXXXXX
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(getDeviceName().c_str());

  // 扫描所有信道以连接信号最强的 AP，防止在 mesh 组网这类场景中连接到弱 AP
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);

  for (int w = 0; w < config.wifiCount; w++) {
    if (config.wifiList[w].ssid.length() == 0) continue;

    const char* ssid = config.wifiList[w].ssid.c_str();
    const char* pass = config.wifiList[w].password.c_str();
    LOG("WiFi", "尝试第 %d/%d 条WiFi: %s", w + 1, config.wifiCount, ssid);

    for (int attempt = 1; attempt <= WIFI_RECONNECT_ATTEMPTS_PER_SSID; attempt++) {
      WiFi.begin(ssid, pass, 0, nullptr, true);
      unsigned long start = millis();
      while (millis() - start < 10000) {
        if (WiFi.status() == WL_CONNECTED) {
          s_mode          = WIFI_MODE_STA_CONNECTED;
          s_everConnected = true;
          s_initDone      = true;
          WiFi.setSleep(false);  // 关闭 WiFi Modem Sleep，避免 TCP SYN 丢包导致 3 秒连接延迟
          LOG("WiFi", "第 %d/%d 条WiFi，第 %d/%d 次连接成功，IP: %s", w + 1, config.wifiCount, attempt, WIFI_RECONNECT_ATTEMPTS_PER_SSID, WiFi.localIP().toString().c_str());
          return;
        }
        delay(100);
        esp_task_wdt_reset();
      }

      LOG("WiFi", "第 %d/%d 条WiFi，第 %d/%d 次连接超时", w + 1, config.wifiCount, attempt, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
      // disconnect(false)：只断开 AP 连接，保持 STA 模式开启
      // 避免 disconnect(true) 关掉 STA 后 begin() 的 enableSTA(true) 触发
      // ESP_ERR_WIFI_STATE（0x3014）"STA enable failed!" 的驱动状态竞争
      WiFi.disconnect(false);
      delay(500);
      esp_task_wdt_reset();
    }
  }

  LOG("WiFi", "所有WiFi条目全部失败，切换到AP模式");
  enterAPMode();
}

void wifiManagerTick() {
  if (s_mode == WIFI_MODE_AP_ACTIVE) return;

  switch (s_reconnState) {
    case RECONNECT_IDLE:
      if (s_everConnected && s_mode == WIFI_MODE_STA_CONNECTED && WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        s_mode = WIFI_MODE_RECONNECTING;
        s_reconnState   = RECONNECT_WAITING;
        s_reconnWIdx    = 0;
        s_reconnAttempt = 0;
        s_lastAttemptMs = 0;  // 使 WAITING 立即触发首次尝试
        LOG("WiFi", "检测到WiFi断线，启动重连轮询");
      }
      break;

    case RECONNECT_WAITING:
      if (millis() - s_lastAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
        // 跳过空 SSID
        while (s_reconnWIdx < config.wifiCount && config.wifiList[s_reconnWIdx].ssid.length() == 0) {
          s_reconnWIdx++;
        }
        if (s_reconnWIdx >= config.wifiCount) {
          // 所有 SSID 已尝试完一轮，重置回首条循环
          s_reconnWIdx    = 0;
          s_reconnAttempt = 0;
          LOG("WiFi", "所有 SSID 重连均失败，重置循环重试");
        }
        const char* ssid = config.wifiList[s_reconnWIdx].ssid.c_str();
        const char* pass = config.wifiList[s_reconnWIdx].password.c_str();
        LOG("WiFi", "重连尝试 SSID: %s，第 %d/%d 次", ssid, s_reconnAttempt + 1, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
        WiFi.disconnect(true);
        delay(300);
        WiFi.begin(ssid, pass, 0, nullptr, true);
        s_lastAttemptMs = millis();
        s_reconnState   = RECONNECT_CONNECTING;
      }
      break;

    case RECONNECT_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        s_mode        = WIFI_MODE_STA_CONNECTED;
        s_reconnState = RECONNECT_IDLE;
        WiFi.setSleep(false);  // 重连后重新关闭 Modem Sleep
        LOG("WiFi", "重连成功，SSID: %s，IP: %s", config.wifiList[s_reconnWIdx].ssid.c_str(), WiFi.localIP().toString().c_str());
        if (s_reconnectCb) s_reconnectCb();
      } else if (millis() - s_lastAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
        LOG("WiFi", "重连超时，SSID: %s，第 %d/%d 次", config.wifiList[s_reconnWIdx].ssid.c_str(), s_reconnAttempt + 1, WIFI_RECONNECT_ATTEMPTS_PER_SSID);
        WiFi.disconnect(true);
        s_reconnAttempt++;
        if (s_reconnAttempt >= WIFI_RECONNECT_ATTEMPTS_PER_SSID) {
          s_reconnAttempt = 0;
          s_reconnWIdx++;
        }
        s_lastAttemptMs = millis();
        s_reconnState   = RECONNECT_WAITING;
      }
      break;
  }
}

WiFiMode wifiManagerGetMode() {
  return s_mode;
}

bool wifiManagerIsInitDone() {
  return s_initDone;
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

String getDeviceId() {
  static String s_id = "";
  if (s_id.length() > 0) return s_id;
  uint64_t mac = ESP.getEfuseMac();
  // 取 MAC 后三字节（byte3:byte4:byte5），保持与 MAC 地址显示顺序一致
  uint32_t last3 = (uint32_t)(((mac >> 24) & 0xFF) << 16 |
                               ((mac >> 32) & 0xFF) << 8  |
                               ((mac >> 40) & 0xFF));
  s_id = String(last3, HEX);
  s_id.toUpperCase();
  while (s_id.length() < 6) s_id = "0" + s_id;
  return s_id;
}

String getDeviceName() {
  return String(APP_NAME "-") + getDeviceId();
}

void wifiManagerSetReconnectCallback(WifiReconnectCallback cb) {
  s_reconnectCb = cb;
}

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_task_wdt.h>

#include "wifi/wifi_manager.h"
#include "logger.h"
#include "config/config.h"
#include "sim/sim.h"
#include "call/call.h"
#include "time/time_module.h"
#include "sms/sms.h"
#include "push/push.h"
#include "push/push_retry.h"
#include "http/http_server.h"
#include "ota/ota_manager.h"

// Serial port mapping
#define TXD 3
#define RXD 4
#define MODEM_EN_PIN 5

AsyncWebServer server(80);

// 记录 SIM 信息是否已抓取（在 loop 中 SIM_READY 后执行一次）
static bool s_simInfoFetched = false;

// 开机推送：检测到 WiFi 初始化完成后延迟 BOOT_PUSH_DELAY_MS 触发
static bool          s_bootPushPending    = false;
static unsigned long s_bootPushAfterMs   = 0;
static bool          s_wifiInitWasSeen   = false;  // 已观测到初始化完成，防止重复触发

// WiFi 初始化完成（STA 连上或进入 AP 模式）后，等待此时长再发送开机推送通知
constexpr unsigned long BOOT_PUSH_DELAY_MS = 3000;

// ---------- helpers ----------

static void blinkShort(unsigned long gap = 500) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap);
}

static void delayWithWdt(unsigned long ms) {
  unsigned long elapsed = 0;
  while (elapsed < ms) {
    unsigned long step = min((unsigned long)500, ms - elapsed);
    delay(step);
    esp_task_wdt_reset();
    elapsed += step;
  }
}

static void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);
  LOG("SIM", "EN 拉低：关闭模组");
  digitalWrite(MODEM_EN_PIN, LOW);
  delayWithWdt(1200);
  LOG("SIM", "EN 拉高：开启模组");
  digitalWrite(MODEM_EN_PIN, HIGH);
  delayWithWdt(6000);
}

// ---------- Arduino entry points ----------

void setup() {
  // 立即喂狗：框架初始化可能已消耗部分 TWDT 窗口
  esp_task_wdt_reset();

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.begin(115200);
  delayWithWdt(1500);  // 替换裸 delay：此时尚未有任何输出，必须喂狗

  Serial1.setRxBufferSize(500);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);

  // Modem cold start
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();

  // 时区初始化（须在 NTP/SIM 时间同步前调用）
  timeModuleInit();

  initConcatBuffer();
  loadConfig();
  loadRebootSchedule(rebootSchedule);
  esp_task_wdt_reset();

  simInit();
  esp_task_wdt_reset();

  // WiFi
  wifiManagerSetReconnectCallback([]{ timeModuleSyncNTP(); });
  wifiManagerInit();
  esp_task_wdt_reset();

  // 时间同步：STA 模式下优先使用 NTP；AP 模式下等 SIM 就绪后从 NITZ 同步
  if (wifiManagerGetMode() == WIFI_MODE_STA_CONNECTED) {
    timeModuleSyncNTP();
    if (timeModuleIsTimeSynced()) {
      LOG("WiFi", "NTP时间同步成功，UTC: %ld", (long)time(nullptr));
    } else {
      LOG("WiFi", "NTP时间同步失败，将在SIM就绪后通过NITZ同步");
    }
  } else {
    LOG("WiFi", "AP模式，跳过NTP同步。请访问 %s 配置WiFi", getDeviceUrl().c_str());
  }

  // LittleFS — formatOnFail=true 保证首次烧录或分区损坏时自动格式化
  // 格式化操作可能耗时数秒，需在前后喂狗
  esp_task_wdt_reset();
  if (!LittleFS.begin(true)) {
    LOG("HTTP", "LittleFS 挂载失败，HTML 页面不可用");
  } else {
    LOG("HTTP", "LittleFS 挂载成功");
  }
  esp_task_wdt_reset();

  setupHttpServer(server);
  otaInit();

  callInit();
  pushRetryInit();
  simStartReaderTask();

  digitalWrite(LED_BUILTIN, LOW);
  // 开机推送在 loop() 中检测 wifiManagerIsInitDone() 上升沿后自动安排
}

void loop() {
  {
    static unsigned long lastUrlPrint = 0;
    if (millis() - lastUrlPrint >= 3000) {
      lastUrlPrint = millis();
      if (wifiManagerGetMode() == WIFI_MODE_AP_ACTIVE) {
        LOG("HTTP", "⚠️ 当前号码: %s，请访问 %s 配置WiFi，或通过 BluFi BLE 配网（设备名: %s）", simGetPhoneNum().c_str(), getDeviceUrl().c_str(), getDeviceName().c_str());
      } else {
        LOG("HTTP", "⚠️ 当前号码: %s，请访问 %s 进行配置", simGetPhoneNum().c_str(), getDeviceUrl().c_str());
      }
    }
  }

  // 开机推送：首次检测到 WiFi 初始化完成（STA 获取到 IP 或进入 AP 模式）后
  // 延迟 BOOT_PUSH_DELAY_MS 再触发，确保网络栈已就绪
  if (!s_wifiInitWasSeen && wifiManagerIsInitDone()) {
    s_wifiInitWasSeen = true;
    if (isConfigValid()) {
      s_bootPushPending = true;
      s_bootPushAfterMs = millis() + BOOT_PUSH_DELAY_MS;
      LOG("Push", "WiFi初始化完成，%lu ms 后触发开机推送", BOOT_PUSH_DELAY_MS);
    }
  }

  // 开机推送：WiFi 初始化后 3 秒延迟触发，不依赖 WiFi 连接状态
  if (s_bootPushPending && millis() >= s_bootPushAfterMs) {
    s_bootPushPending = false;
    LOG("Push", "触发开机推送...");
    sendPushNotification("设备",
      "设备已启动"
      "\n设备地址: " + getDeviceUrl() +
      "\nMAC: " + WiFi.macAddress() +
      "\n固件版本: " + otaGetVersion(),
      timeModuleGetDateStr(), MSG_TYPE_SIM);
  }

  checkConcatTimeout();
  callTick();
  simTick();
  pushRetryTick();
  timeModuleTick();
  wifiManagerTick();

  // SIM 就绪后抓取运营商/信号，并在 NTP 未同步时从 SIM NITZ 同步时间
  if (!s_simInfoFetched && simGetState() == SIM_READY) {
    s_simInfoFetched = true;
    simFetchInfo();
    if (!timeModuleIsTimeSynced()) {
      timeModuleSyncFromSIM();
    }
  }

  rebootTick();
}

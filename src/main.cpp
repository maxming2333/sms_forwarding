#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_task_wdt.h>

#include "wifi/wifi_manager.h"
#include "logger/logger.h"
#include "config/config.h"
#include "sim/sim.h"
#include "call/call.h"
#include "time/time_sync.h"
#include "sms/sms.h"
#include "push/push.h"
#include "push/push_retry.h"
#include "push/push_queue.h"
#include "http/http_server.h"
#include "ota/ota_manager.h"
#include "coredump/coredump.h"
#include <time.h>

// Serial port mapping
#define TXD 3
#define RXD 4
#define MODEM_EN_PIN 5

AsyncWebServer server(80);

// 记录 SIM 信息是否已抓取（在 loop 中 SIM_READY 后执行一次）
static bool s_simInfoFetched = false;

// 开机推送崩溃快照（在安排推送时捕获，防止 RTC 被后续更新覆写）
static bool   s_cachedHasCrash      = false;
static time_t s_cachedCrashTime     = 0;
static String s_cachedCrashVersion  = "";

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
  LOG("MAIN", "EN 拉低：关闭模组");
  digitalWrite(MODEM_EN_PIN, LOW);
  delayWithWdt(1200);
  LOG("MAIN", "EN 拉高：开启模组");
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
  TimeSync::init();

  Sms::initConcatBuffer();
  ConfigStore::load();
  Coredump::init();  // 断电重启时从 NVS 恢复崩溃时间估算
  ConfigStore::loadReboot(rebootSchedule);
  esp_task_wdt_reset();

  Sim::init();
  esp_task_wdt_reset();

  // WiFi
  WifiManager::setReconnectCallback([]{ TimeSync::syncNTP(); });
  WifiManager::init();
  esp_task_wdt_reset();

  // 时间同步：STA 模式下优先使用 NTP；AP 模式下等 SIM 就绪后从 NITZ 同步
  if (WifiManager::mode() == WIFI_MODE_STA_CONNECTED) {
    TimeSync::syncNTP();
    if (TimeSync::isSynced()) {
      LOG("MAIN", "NTP时间同步成功，UTC: %ld", (long)time(nullptr));
    } else {
      LOG("MAIN", "NTP时间同步失败，将在SIM就绪后通过NITZ同步");
    }
  } else {
    LOG("MAIN", "AP模式，跳过NTP同步。请访问 %s 配置WiFi", WifiManager::deviceUrl().c_str());
  }

  // LittleFS — formatOnFail=true 保证首次烧录或分区损坏时自动格式化
  // 格式化操作可能耗时数秒，需在前后喂狗
  esp_task_wdt_reset();
  if (!LittleFS.begin(true)) {
    LOG("MAIN", "LittleFS 挂载失败，HTML 页面不可用");
  } else {
    LOG("MAIN", "LittleFS 挂载成功");
    // 不写入文件的模块（仍输出到串口和内存缓冲）；如需全量记录传 nullptr。
    // 可用模块名：Push / PushQ / Retry / SMS / SIM / Call / WiFi / HTTP / OTA / BLE / Time / Cfg
    static const char* kLogFileSkip[] = { nullptr };
    Logger::init(kLogFileSkip);
    Logger::setStorageEnabled(config.logFileEnabled);
  }
  esp_task_wdt_reset();

  HttpServer::setup(server);
  Ota::init();

  Call::init();
  PushRetry::init();
  PushQueue::init();
  Sim::startReaderTask();

  digitalWrite(LED_BUILTIN, LOW);
  // 开机推送在 loop() 中检测 WifiManager::isInitDone() 上升沿后自动安排
}

void loop() {
  {
    static unsigned long lastUrlPrint = 0;
    if (millis() - lastUrlPrint >= 3000) {
      lastUrlPrint = millis();
      if (WifiManager::mode() == WIFI_MODE_AP_ACTIVE) {
        LOG("MAIN", "⚠️ 当前号码: %s，请访问 %s 配置WiFi，或通过 BluFi BLE 配网（设备名: %s）", Sim::phoneNum().c_str(), WifiManager::deviceUrl().c_str(), WifiManager::deviceName().c_str());
      } else {
        LOG("MAIN", "⚠️ 当前号码: %s，请访问 %s 进行配置", Sim::phoneNum().c_str(), WifiManager::deviceUrl().c_str());
      }
    }
  }

  // 开机推送：首次检测到 WiFi 初始化完成（STA 获取到 IP 或进入 AP 模式）后
  // 延迟 BOOT_PUSH_DELAY_MS 再触发，确保网络栈已就绪
  if (!s_wifiInitWasSeen && WifiManager::isInitDone()) {
    s_wifiInitWasSeen = true;
    if (ConfigStore::isValid()) {
      s_cachedHasCrash     = Coredump::hasData();
      s_cachedCrashTime    = Coredump::crashTime();
      s_cachedCrashVersion = Coredump::crashVersion();
      s_bootPushPending = true;
      s_bootPushAfterMs = millis() + BOOT_PUSH_DELAY_MS;
      LOG("MAIN", "WiFi初始化完成，%lu ms 后触发开机推送", BOOT_PUSH_DELAY_MS);
    }
  }

  // 开机推送：WiFi 初始化后 3 秒延迟触发，不依赖 WiFi 连接状态
  if (s_bootPushPending && millis() >= s_bootPushAfterMs) {
    s_bootPushPending = false;
    LOG("MAIN", "触发开机推送...");
    {
      String bootMsg = String("🚀 设备已启动") +
        "\n🌐 设备地址: " + WifiManager::deviceUrl() +
        "\n📶 MAC: " + WiFi.macAddress() +
        "\n📦 固件版本: " + Ota::version();
      if (s_cachedHasCrash) {
        time_t ct = s_cachedCrashTime;
        if (ct > 0) {
          char timeBuf[20];
          struct tm tmInfo;
          gmtime_r(&ct, &tmInfo);
          strftime(timeBuf, sizeof(timeBuf), "%Y%m%dT%H%M%S", &tmInfo);
          bootMsg += String("\n⚠️ 崩溃记录: 上次崩溃时间 ") + timeBuf + "（近似）";
        } else {
          bootMsg += "\n⚠️ 崩溃记录: 检测到上次崩溃（时间未知）";
        }
        if (s_cachedCrashVersion.length() > 0) {
          bootMsg += String("，崩溃版本: ") + s_cachedCrashVersion;
        }
        bootMsg += "，请前往工具箱导出 coredump";
      }
      Push::send("设备", bootMsg, TimeSync::dateStr(), MsgTypeInfo(MSG_TYPE_SIM));
    }
  }

  Sms::checkConcatTimeout();
  Call::tick();
  Sim::tick();
  PushQueue::tick();
  PushRetry::tick();
  TimeSync::tick();
  WifiManager::tick();

  // RTC 最后已知时间更新（每 10 秒，仅时间已同步时）
  {
    static unsigned long s_lastRtcUpdate = 0;
    if (TimeSync::isSynced() && millis() - s_lastRtcUpdate >= 10000) {
      s_lastRtcUpdate = millis();
      Coredump::updateLastKnownTime(time(nullptr));
    }
  }

  // SIM 就绪后抓取运营商/信号，并在 NTP 未同步时从 SIM NITZ 同步时间
  if (!s_simInfoFetched && Sim::state() == SIM_READY) {
    s_simInfoFetched = true;
    Sim::fetchInfo();
    if (!TimeSync::isSynced()) {
      TimeSync::syncFromSIM();
    }
  }

  ConfigStore::rebootTick();
}

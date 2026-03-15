// ============================================================
// main.cpp — entry point only; all logic is in src/ modules
// ============================================================
#include <Arduino.h>
#include <esp_task_wdt.h>      // Hardware watchdog
#include "config/AppConfig.h"
#include "wifi/WifiManager.h"
#include "sim/SimManager.h"
#include "sms/SmsSender.h"
#include "sms/SmsReceiver.h"
#include "email/EmailNotifier.h"
#include "push/PushManager.h"
#include "web/WebServer.h"
#include "scheduler/Scheduler.h"
#include "wifi_config.h"  // WIFI_SSID / WIFI_PASS defaults

// ── Placeholders to satisfy old-code references (not used in new flow) ──────
// (removed — all code now in modules)

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.begin(115200);
  delay(1500);

  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);

  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();

  loadConfig();
  configValid = isConfigValid();
  smsReceiverInit();

  while (!sendATandWaitOK("AT", 1000)) {
    Serial.println("[Main] AT未响应，重试...");
    blinkShort();
  }
  Serial.println("[Main] 模组AT响应正常");

  simPresent = initSIMDependent();

  String ssid = config.wifiSSID.length() > 0 ? config.wifiSSID : String(WIFI_SSID);
  String pass = config.wifiPass.length() > 0 ? config.wifiPass : String(WIFI_PASS);
  wifiInit(ssid, pass);
  ntpSync();
  emailInit();
  webServerInit();

  digitalWrite(LED_BUILTIN, LOW);

  if (configValid && !simInitialized)
    emailNotify("短信转发器已启动（未检测到SIM卡）",
      ("设备已启动，未检测到SIM卡\n请插入SIM卡\n设备地址: " + getDeviceUrl()).c_str());

  // ── Watchdog: 60 s timeout; if loop() stalls the ESP32 auto-reboots ───────
  esp_task_wdt_init(60, true);  // 60 seconds, panic (reboot) on timeout
  esp_task_wdt_add(NULL);       // add current task (loop) to WDT
  Serial.println("[Main] 看门狗已启动 (60s)");
}

void loop() {
  // Feed watchdog first — if anything below hangs >60 s the device reboots
  esp_task_wdt_reset();

  // WiFi keepalive
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 10000) {
      lastReconnect = millis();
      Serial.println("[Main] WiFi断开，尝试重连...");
      WiFi.reconnect();
    }
  }

  webServerTick();

  if (!configValid) {
    static unsigned long lastLog = 0;
    if (millis() - lastLog >= 1000) {
      lastLog = millis();
      Serial.println("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数");
    }
  }

  static unsigned long lastSimCheck = 0;
  if (millis() - lastSimCheck >= 5000) {
    lastSimCheck = millis();
    bool present = checkSIMPresent();
    if (present && !simPresent) {
      Serial.println("[Main] 🔔 SIM卡插入！");
      simPresent = true; simInitialized = false;
      devicePhoneNumber = "未知号码";
      delay(500); initSIMDependent();
    } else if (!present && simPresent) {
      Serial.println("[Main] ⚠️ SIM卡拔出！");
      simPresent = false; simInitialized = false;
      devicePhoneNumber = "未知号码";
      if (configValid)
        emailNotify("短信转发器：SIM卡已拔出",
          ("SIM卡已拔出，转发暂停\n设备地址: " + getDeviceUrl()).c_str());
    }
  }

  smsReceiverTick();
  checkScheduledReboot();
  checkTrafficKeep();
}

// ── END OF FILE — remaining content below is the old monolithic code ─────────
// It will be removed once the build is verified. For now #if 0 guards it out.
#if 0


//串口映射
#define TXD 3
#define RXD 4
#define MODEM_EN_PIN 5

// LED引脚定义（用于通过CI验证，给个假的）
#ifndef LED_BUILTIN
#define LED_BUILTIN 8
#endif

// 推送通道类型
enum PushType {
  PUSH_TYPE_NONE = 0,      // 未启用
  PUSH_TYPE_POST_JSON = 1, // POST JSON格式 {"sender":"xxx","message":"xxx","timestamp":"xxx"}
  PUSH_TYPE_BARK = 2,      // Bark格式 POST {"title":"xxx","body":"xxx"}
  PUSH_TYPE_GET = 3,       // GET请求，参数放URL中
  PUSH_TYPE_DINGTALK = 4,  // 钉钉机器人
  PUSH_TYPE_PUSHPLUS = 5,  // PushPlus
  PUSH_TYPE_SERVERCHAN = 6,// Server酱
  PUSH_TYPE_CUSTOM = 7,    // 自定义模板
  PUSH_TYPE_FEISHU = 8,    // 飞书机器人
  PUSH_TYPE_GOTIFY = 9,    // Gotify
  PUSH_TYPE_TELEGRAM = 10,  // Telegram Bot
  PUSH_TYPE_WORK_WEIXIN = 11,  // 企业微信机器人
  PUSH_TYPE_SMS = 12,  // 短信转发
};

// 最大推送通道数
#define MAX_PUSH_CHANNELS 5

// 推送通道配置（通用设计，支持多种推送方式）
struct PushChannel {
  bool enabled;           // 是否启用
  PushType type;          // 推送类型
  String name;            // 通道名称（用于显示）
  String url;             // 推送URL（webhook地址）
  String key1;            // 额外参数1（如：钉钉secret、pushplus token等）
  String key2;            // 额外参数2（备用）
  String customBody;      // 自定义请求体模板（使用 {sender} {message} {timestamp} 占位符）
};

// 配置参数结构体
struct Config {
  String smtpServer;
  int smtpPort;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;
  String adminPhone;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];  // 多推送通道
  String webUser;      // Web管理账号
  String webPass;      // Web管理密码
  String wifiSSID;     // WiFi名称（可通过Web界面修改）
  String wifiPass;     // WiFi密码（可通过Web界面修改）
  String numberBlackList;  // 号码黑名单（换行符分隔）
};

// 默认Web管理账号密码
#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

Config config;
Preferences preferences;
WiFiMulti WiFiMulti;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);

bool configValid = false;  // 配置是否有效
bool timeSynced = false;   // NTP时间是否已同步
unsigned long lastPrintTime = 0;  // 上次打印IP的时间
String devicePhoneNumber = "未知号码"; // 本机号码（SIM卡号）

// SIM卡热插拔状态
bool simPresent = false;     // SIM卡是否存在
bool simInitialized = false; // SIM卡相关功能是否已初始化完成

#define SERIAL_BUFFER_SIZE 500
#define MAX_PDU_LENGTH 300
char serialBuf[SERIAL_BUFFER_SIZE];
int serialBufLen = 0;

// 长短信合并相关定义
#define MAX_CONCAT_PARTS 10       // 最大支持的长短信分段数
#define CONCAT_TIMEOUT_MS 30000   // 长短信等待超时时间(毫秒)
#define MAX_CONCAT_MESSAGES 5     // 最多同时缓存的长短信组数

// 长短信分段结构
struct SmsPart {
  bool valid;           // 该分段是否有效
  String text;          // 分段内容
};

// 长短信缓存结构
struct ConcatSms {
  bool inUse;                           // 是否正在使用
  int refNumber;                        // 参考号
  String sender;                        // 发送者
  String timestamp;                     // 时间戳（使用第一个收到的分段的时间戳）
  int totalParts;                       // 总分段数
  int receivedParts;                    // 已收到的分段数
  unsigned long firstPartTime;          // 收到第一个分段的时间
  SmsPart parts[MAX_CONCAT_PARTS];      // 各分段内容
};

ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];  // 长短信缓存

// 前置声明
void sendEmailNotification(const char* subject, const char* body);
bool sendSMS(const char* phoneNumber, const char* message);
String jsonEscape(const String& str);
bool sendATandWaitOK(const char* cmd, unsigned long timeout);
int sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp, const char* devicePhone);
bool checkSIMPresent();
bool initSIMDependent();

// 保存配置到NVS
void saveConfig() {
  preferences.begin("sms_config", false);
  preferences.putString("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort", config.smtpPort);
  preferences.putString("smtpUser", config.smtpUser);
  preferences.putString("smtpPass", config.smtpPass);
  preferences.putString("smtpSendTo", config.smtpSendTo);
  preferences.putString("adminPhone", config.adminPhone);
  preferences.putString("webUser", config.webUser);
  preferences.putString("webPass", config.webPass);
  preferences.putString("wifiSSID", config.wifiSSID);
  preferences.putString("wifiPass", config.wifiPass);
  preferences.putString("numBlkList", config.numberBlackList);

  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    preferences.putBool((prefix + "en").c_str(), config.pushChannels[i].enabled);
    preferences.putUChar((prefix + "type").c_str(), (uint8_t)config.pushChannels[i].type);
    preferences.putString((prefix + "url").c_str(), config.pushChannels[i].url);
    preferences.putString((prefix + "name").c_str(), config.pushChannels[i].name);
    preferences.putString((prefix + "k1").c_str(), config.pushChannels[i].key1);
    preferences.putString((prefix + "k2").c_str(), config.pushChannels[i].key2);
    preferences.putString((prefix + "body").c_str(), config.pushChannels[i].customBody);
  }

  preferences.end();
  Serial.println("配置已保存");
}

// 从NVS加载配置
void loadConfig() {
  preferences.begin("sms_config", true);
  config.smtpServer = preferences.getString("smtpServer", "");
  config.smtpPort = preferences.getInt("smtpPort", 465);
  config.smtpUser = preferences.getString("smtpUser", "");
  config.smtpPass = preferences.getString("smtpPass", "");
  config.smtpSendTo = preferences.getString("smtpSendTo", "");
  config.adminPhone = preferences.getString("adminPhone", "");
  config.webUser = preferences.getString("webUser", DEFAULT_WEB_USER);
  config.webPass = preferences.getString("webPass", DEFAULT_WEB_PASS);
  config.wifiSSID = preferences.getString("wifiSSID", WIFI_SSID);
  config.wifiPass = preferences.getString("wifiPass", WIFI_PASS);
  config.numberBlackList = preferences.getString("numBlkList", "");

  // 加载推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    config.pushChannels[i].enabled = preferences.getBool((prefix + "en").c_str(), false);
    config.pushChannels[i].type = (PushType)preferences.getUChar((prefix + "type").c_str(), PUSH_TYPE_POST_JSON);
    config.pushChannels[i].url = preferences.getString((prefix + "url").c_str(), "");
    config.pushChannels[i].name = preferences.getString((prefix + "name").c_str(), "通道" + String(i + 1));
    config.pushChannels[i].key1 = preferences.getString((prefix + "k1").c_str(), "");
    config.pushChannels[i].key2 = preferences.getString((prefix + "k2").c_str(), "");
    config.pushChannels[i].customBody = preferences.getString((prefix + "body").c_str(), "");
  }

  // 兼容旧配置：如果有旧的httpUrl配置，迁移到第一个通道
  String oldHttpUrl = preferences.getString("httpUrl", "");
  if (oldHttpUrl.length() > 0 && !config.pushChannels[0].enabled) {
    config.pushChannels[0].enabled = true;
    config.pushChannels[0].url = oldHttpUrl;
    config.pushChannels[0].type = preferences.getUChar("barkMode", 0) != 0 ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
    config.pushChannels[0].name = "迁移通道";
    Serial.println("已迁移旧HTTP配置到推送通道1");
  }

  preferences.end();
  Serial.println("配置已加载");
}

// 检查推送通道是否有效配置
bool isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;

  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_DINGTALK:
    case PUSH_TYPE_FEISHU:
    case PUSH_TYPE_WORK_WEIXIN:
    case PUSH_TYPE_SMS:
    case PUSH_TYPE_CUSTOM:
      return ch.url.length() > 0;
    case PUSH_TYPE_PUSHPLUS:
    case PUSH_TYPE_SERVERCHAN:
      return ch.key1.length() > 0;  // 这两个主要靠key1（token/sendkey）
    case PUSH_TYPE_GOTIFY:
      return ch.url.length() > 0 && ch.key1.length() > 0;  // 需要URL和Token
    case PUSH_TYPE_TELEGRAM:
      return ch.key1.length() > 0 && ch.key2.length() > 0; // 需要Chat ID和Token
    default:
      return false;
  }
}

// 检查配置是否有效（至少配置了邮件或任一推送通道）
bool isConfigValid() {
  bool emailValid = config.smtpServer.length() > 0 &&
                    config.smtpUser.length() > 0 &&
                    config.smtpPass.length() > 0 &&
                    config.smtpSendTo.length() > 0;

  bool pushValid = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushValid = true;
      break;
    }
  }

  return emailValid || pushValid;
}

// 获取当前设备URL
String getDeviceUrl() {
  return "http://" + WiFi.localIP().toString() + "/";
}

// HTML配置页面
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>短信转发配置</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { color: #333; text-align: center; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; font-weight: bold; color: #555; }
    input[type="text"], input[type="password"], input[type="number"], textarea, select { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }
    textarea { resize: vertical; min-height: 80px; }
    button { width: 100%; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin-top: 10px; }
    button:hover { background: #45a049; }
    .label-inline { display:inline; font-weight:normal; margin-left: 5px; }
    .btn-send { background: #2196F3; }
    .btn-send:hover { background: #1976D2; }
    .section { border: 1px solid #ddd; padding: 15px; margin-bottom: 20px; border-radius: 5px; }
    .section-title { font-size: 18px; color: #333; margin-bottom: 10px; }
    .status { padding: 10px; background: #e7f3fe; border-left: 4px solid #2196F3; margin-bottom: 20px; }
    .warning { padding: 10px; background: #fff3cd; border-left: 4px solid #ffc107; margin-bottom: 20px; font-size: 12px; }
    .hint { font-size: 12px; color: #888; }
    .nav { display: flex; gap: 10px; margin-bottom: 20px; }
    .nav a { flex: 1; text-align: center; padding: 10px; background: #eee; border-radius: 5px; text-decoration: none; color: #333; }
    .nav a.active { background: #4CAF50; color: white; }
    .push-channel { border: 1px solid #e0e0e0; padding: 12px; margin-bottom: 15px; border-radius: 5px; background: #fafafa; }
    .push-channel-header { display: flex; align-items: center; margin-bottom: 10px; }
    .push-channel-header input[type="checkbox"] { width: auto; margin-right: 8px; }
    .push-channel-header label { margin: 0; font-weight: bold; }
    .push-channel-body { display: none; }
    .push-channel.enabled .push-channel-body { display: block; }
    .push-type-hint { font-size: 11px; color: #666; margin-top: 5px; padding: 8px; background: #f0f0f0; border-radius: 3px; }
    .btn-test { background: #FF9800; width: auto; padding: 8px 20px; margin-top: 8px; font-size: 14px; }
    .btn-test:hover { background: #F57C00; }
    .btn-test:disabled { background: #ccc; cursor: not-allowed; }
    .test-result { margin-top: 8px; padding: 8px; border-radius: 4px; font-size: 13px; display: none; }
    .test-result.test-success { background: #e8f5e9; border-left: 3px solid #4CAF50; color: #2e7d32; }
    .test-result.test-error { background: #ffebee; border-left: 3px solid #f44336; color: #c62828; }
    .test-result.test-loading { background: #fff3e0; border-left: 3px solid #FF9800; color: #e65100; }
  </style>
</head>
<body>
  <div class="container">
    <h1>📱 短信转发器</h1>
    <div class="nav">
      <a href="/" class="active">⚙️ 系统配置</a>
      <a href="/tools">🧰 工具箱</a>
      <a href="/api-docs">📚 API文档</a>
    </div>
    <div class="status" id="status">设备IP: <strong>%IP%</strong></div>

    <form action="/save" method="POST">
      <div class="section">
        <div class="section-title">🔐 Web管理账号设置</div>
        <div class="warning">⚠️ 首次使用请修改默认密码！默认账号: )rawliteral" DEFAULT_WEB_USER "，默认密码: " DEFAULT_WEB_PASS R"rawliteral(
        </div>
        <div class="form-group">
          <label>管理账号</label>
          <input type="text" name="webUser" value="%WEB_USER%" placeholder="admin">
        </div>
        <div class="form-group">
          <label>管理密码</label>
          <input type="password" name="webPass" value="%WEB_PASS%" placeholder="请设置复杂密码">
        </div>
      </div>

      <div class="section">
        <div class="section-title">📡 WiFi 设置</div>
        <div class="hint" style="margin-bottom:10px;">修改WiFi配置后保存将自动重启设备并连接新WiFi。若连接失败将自动开启热点 <b>SMS-Forwarder-AP</b>。</div>
        <div class="form-group">
          <label>WiFi 名称 (SSID)</label>
          <input type="text" name="wifiSSID" value="%WIFI_SSID%" placeholder="请输入WiFi名称">
        </div>
        <div class="form-group">
          <label>WiFi 密码</label>
          <input type="password" name="wifiPass" value="%WIFI_PASS%" placeholder="请输入WiFi密码">
        </div>
      </div>

      <div class="section">
        <div class="section-title">📧 邮件通知设置</div>
        <div class="form-group">
          <label>SMTP服务器</label>
          <input type="text" name="smtpServer" value="%SMTP_SERVER%" placeholder="smtp.qq.com">
        </div>
        <div class="form-group">
          <label>SMTP端口</label>
          <input type="number" name="smtpPort" value="%SMTP_PORT%" placeholder="465">
        </div>
        <div class="form-group">
          <label>邮箱账号</label>
          <input type="text" name="smtpUser" value="%SMTP_USER%" placeholder="your@qq.com">
        </div>
        <div class="form-group">
          <label>邮箱密码/授权码</label>
          <input type="password" name="smtpPass" value="%SMTP_PASS%" placeholder="授权码">
        </div>
        <div class="form-group">
          <label>接收邮件地址</label>
          <input type="text" name="smtpSendTo" value="%SMTP_SEND_TO%" placeholder="receiver@example.com">
        </div>
      </div>

      <div class="section">
        <div class="section-title">🔗 HTTP推送通道设置</div>
        <div class="hint" style="margin-bottom:15px;">可同时启用多个推送通道，每个通道独立配置。支持POST JSON、Bark、GET、钉钉、PushPlus、Server酱等多种方式。</div>

        %PUSH_CHANNELS%
      </div>

      <div class="section">
        <div class="section-title">🚫 号码黑名单</div>
        <div class="hint" style="margin-bottom:15px;">每行一个号码，来自黑名单号码的短信将被忽略（不转发、不执行命令）。</div>
        <div class="form-group">
          <label>黑名单号码</label>
          <textarea name="numberBlackList" rows="5">%NUMBER_BLACK_LIST%</textarea>
        </div>
      </div>

      <div class="section">
        <div class="section-title">👤 管理员设置</div>
        <div class="form-group">
          <label>管理员手机号</label>
          <input type="text" name="adminPhone" value="%ADMIN_PHONE%" placeholder="13800138000">
        </div>
      </div>

      <button type="submit">💾 保存配置</button>
    </form>
  </div>
  <script>
    function toggleChannel(idx) {
      var ch = document.getElementById('channel' + idx);
      var cb = document.getElementById('push' + idx + 'en');
      if (cb.checked) {
        ch.classList.add('enabled');
      } else {
        ch.classList.remove('enabled');
      }
    }
    function updateTypeHint(idx) {
      var sel = document.getElementById('push' + idx + 'type');
      var hint = document.getElementById('hint' + idx);
      var extraFields = document.getElementById('extra' + idx);
      var customFields = document.getElementById('custom' + idx);
      var type = parseInt(sel.value);

      // 隐藏所有额外字段
      extraFields.style.display = 'none';
      customFields.style.display = 'none';
      document.getElementById('key1label' + idx).innerText = '参数1';
      document.getElementById('key2label' + idx).innerText = '参数2';
      document.getElementById('key1' + idx).placeholder = '';
      document.getElementById('key2' + idx).placeholder = '';
      // key2 区域默认隐藏，只在需要用到 key2 的通知方式中显示
      document.getElementById('key2' + idx).closest('.form-group').style.display = 'none';

      if (type == 1) {
        hint.innerHTML = '<b>POST JSON格式：</b><br>{"sender":"发送者号码","message":"短信内容","timestamp":"时间戳"}';
      } else if (type == 2) {
        hint.innerHTML = '<b>Bark格式：</b><br>POST {"title":"发送者号码","body":"短信内容"}';
      } else if (type == 3) {
        hint.innerHTML = '<b>GET请求格式：</b><br>URL?sender=xxx&message=xxx&timestamp=xxx';
      } else if (type == 4) {
        hint.innerHTML = '<b>钉钉机器人：</b><br>填写Webhook地址，如需加签请填Secret';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Secret（加签密钥，可选）';
        document.getElementById('key1' + idx).placeholder = 'SEC...';
      } else if (type == 5) {
        hint.innerHTML = '<b>PushPlus：</b><br>填写Token，URL留空使用默认';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Token';
        document.getElementById('key1' + idx).placeholder = 'pushplus的token';
        // 显示 key2 区域
        document.getElementById('key2' + idx).closest('.form-group').style.display = 'block';
        document.getElementById('key2label' + idx).innerText = '发送渠道';
        document.getElementById('key2' + idx).placeholder = 'wechat(default), extension, app';
      } else if (type == 6) {
        hint.innerHTML = '<b>Server酱：</b><br>填写SendKey，URL留空使用默认';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'SendKey';
        document.getElementById('key1' + idx).placeholder = 'SCT...';
      } else if (type == 7) {
        hint.innerHTML = '<b>自定义模板：</b><br>在请求体模板中使用 {sender} {message} {timestamp} {device} 作为占位符';
        customFields.style.display = 'block';
      } else if (type == 8) {
        hint.innerHTML = '<b>飞书机器人：</b><br>填写Webhook地址，如需签名验证请填Secret';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Secret（签名密钥，可选）';
        document.getElementById('key1' + idx).placeholder = '飞书机器人的签名密钥';
      } else if (type == 9) {
        hint.innerHTML = '<b>Gotify：</b><br>填写服务器地址（如 http://gotify.example.com），Token填写应用Token';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Token（应用Token）';
        document.getElementById('key1' + idx).placeholder = 'A...';
      } else if (type == 10) {
        hint.innerHTML = '<b>Telegram Bot：</b><br>填写Chat ID（参数1）和Bot Token（参数2），URL留空默认使用官方API';
        extraFields.style.display = 'block';
        document.getElementById('key1label' + idx).innerText = 'Chat ID';
        document.getElementById('key1' + idx).placeholder = '123456789';
        document.getElementById('key2label' + idx).innerText = 'Bot Token';
        document.getElementById('key2' + idx).placeholder = '12345678:ABC...';
      } else if (type == 11) {
        hint.innerHTML = '<b>企业微信机器人：</b><br>填写Webhook地址';
      } else if (type == 12) {
        hint.innerHTML = '<b>短信转发：</b><br>填写目标手机号，国际号码请使用 “+国家码” 前缀，如 +8612345678900';
      }
    }
    document.addEventListener('DOMContentLoaded', function() {
      for (var i = 0; i < 5; i++) {
        toggleChannel(i);
        updateTypeHint(i);
      }
    });
    function testChannel(idx) {
      var btn = document.getElementById('testBtn' + idx);
      var result = document.getElementById('testResult' + idx);
      btn.disabled = true;
      btn.textContent = '⏳ 测试中...';
      result.className = 'test-result test-loading';
      result.style.display = 'block';
      result.textContent = '正在发送测试消息...';
      var formData = new URLSearchParams();
      formData.append('type', document.getElementById('push' + idx + 'type').value);
      formData.append('url', document.querySelector('[name=push' + idx + 'url]').value);
      var k1 = document.getElementById('key1' + idx);
      formData.append('key1', k1 ? k1.value : '');
      var k2 = document.getElementById('key2' + idx);
      formData.append('key2', k2 ? k2.value : '');
      var bodyEl = document.querySelector('[name=push' + idx + 'body]');
      formData.append('body', bodyEl ? bodyEl.value : '');
      fetch('/test_push', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: formData })
        .then(function(r) { return r.json(); })
        .then(function(data) {
          btn.disabled = false;
          btn.textContent = '🧪 测试';
          result.className = 'test-result ' + (data.success ? 'test-success' : 'test-error');
          result.style.display = 'block';
          result.innerHTML = (data.success ? '✅ ' : '❌ ') + data.message;
        })
        .catch(function(err) {
          btn.disabled = false;
          btn.textContent = '🧪 测试';
          result.className = 'test-result test-error';
          result.style.display = 'block';
          result.textContent = '❌ 请求失败: ' + err;
        });
    }
  </script>
</body>
</html>
)rawliteral";

// HTML工具箱页面
const char* htmlToolsPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>工具箱</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { color: #333; text-align: center; }
    .form-group { margin-bottom: 15px; }
    label { display: block; margin-bottom: 5px; font-weight: bold; color: #555; }
    input[type="text"], textarea { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }
    textarea { resize: vertical; min-height: 100px; }
    button { width: 100%; padding: 12px; background: #2196F3; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin-top: 10px; }
    button:hover { background: #1976D2; }
    .btn-query { background: #9C27B0; }
    .btn-query:hover { background: #7B1FA2; }
    .btn-ping { background: #FF9800; }
    .btn-ping:hover { background: #F57C00; }
    .btn-info { background: #607D8B; }
    .btn-info:hover { background: #455A64; }
    button:disabled { background: #ccc; cursor: not-allowed; }
    .section { border: 1px solid #ddd; padding: 15px; margin-bottom: 20px; border-radius: 5px; }
    .section-title { font-size: 18px; color: #333; margin-bottom: 10px; }
    .status { padding: 10px; background: #e7f3fe; border-left: 4px solid #2196F3; margin-bottom: 20px; }
    .nav { display: flex; gap: 10px; margin-bottom: 20px; }
    .nav a { flex: 1; text-align: center; padding: 10px; background: #eee; border-radius: 5px; text-decoration: none; color: #333; }
    .nav a.active { background: #2196F3; color: white; }
    .char-count { font-size: 12px; color: #888; text-align: right; }
    .hint { font-size: 12px; color: #888; margin-top: 5px; }
    .result-box { margin-top: 10px; padding: 10px; border-radius: 5px; display: none; }
    .result-success { background: #e8f5e9; border-left: 4px solid #4CAF50; color: #2e7d32; }
    .result-error { background: #ffebee; border-left: 4px solid #f44336; color: #c62828; }
    .result-loading { background: #fff3e0; border-left: 4px solid #FF9800; color: #e65100; }
    .result-info { background: #e3f2fd; border-left: 4px solid #2196F3; color: #1565c0; }
    .info-table { width: 100%; border-collapse: collapse; margin-top: 8px; }
    .info-table td { padding: 5px 8px; border-bottom: 1px solid #ddd; }
    .info-table td:first-child { font-weight: bold; width: 40%; color: #555; }
    .btn-group { display: flex; gap: 10px; flex-wrap: wrap; }
    .btn-group button { flex: 1; min-width: 120px; }
    #atLog { background: #333; color: #00ff00; font-family: 'Courier New', Courier, monospace; min-height: 150px; max-height: 300px; overflow-y: auto; padding: 10px; border-radius: 5px; margin-bottom: 10px; font-size: 13px; white-space: pre-wrap; word-break: break-all; }
    .at-input-group { display: flex; gap: 10px; }
    .at-input-group input { flex: 1; font-family: monospace; }
    .at-input-group button { width: auto; min-width: 80px; margin-top: 0; }
  </style>
</head>
<body>
  <div class="container">
    <h1>📱 短信转发器</h1>
    <div class="nav">
      <a href="/">⚙️ 系统配置</a>
      <a href="/tools" class="active">🧰 工具箱</a>
      <a href="/api-docs">📚 API文档</a>
    </div>
    <div class="status" id="status">设备IP: <strong>%IP%</strong></div>

    <form action="/sendsms" method="POST">
      <div class="section">
        <div class="section-title">📤 发送短信</div>
        <div class="form-group">
          <label>目标号码</label>
          <input type="text" name="phone" placeholder="填写目标手机号，国际号码请使用 “+国家码” 前缀，如 +8612345678900" required>
        </div>
        <div class="form-group">
          <label>短信内容</label>
          <textarea name="content" placeholder="请输入短信内容..." required oninput="updateCount(this)"></textarea>
          <div class="char-count">已输入 <span id="charCount">0</span> 字符</div>
        </div>
        <button type="submit">📨 发送短信</button>
      </div>
    </form>

    <div class="section">
      <div class="section-title">📊 模组信息查询</div>
      <div class="btn-group">
        <button type="button" class="btn-query" onclick="queryInfo('ati')">📋 固件信息</button>
        <button type="button" class="btn-query" onclick="queryInfo('signal')">📶 信号质量</button>
      </div>
      <div class="btn-group">
        <button type="button" class="btn-info" onclick="queryInfo('siminfo')">💳 SIM卡信息</button>
        <button type="button" class="btn-info" onclick="queryInfo('network')">🌍 网络状态</button>
      </div>
      <div class="btn-group">
        <button type="button" class="btn-info" onclick="queryInfo('wifi')" style="background:#00BCD4;">📡 WiFi状态</button>
      </div>
      <div class="result-box" id="queryResult"></div>
    </div>

    <div class="section">
      <div class="section-title">🌐 网络测试</div>
      <button type="button" class="btn-ping" id="pingBtn" onclick="confirmPing()">📡 点我消耗一点流量</button>
      <div class="hint">将向 8.8.8.8 进行 ping 操作，一次性消耗极少流量费用</div>
      <div class="result-box" id="pingResult"></div>
    </div>

    <div class="section">
      <div class="section-title">✈️ 模组控制</div>
      <div class="btn-group">
        <button type="button" id="flightBtn" onclick="toggleFlightMode()" style="background:#E91E63;">✈️ 切换飞行模式</button>
        <button type="button" onclick="queryFlightMode()" style="background:#9C27B0;">🔍 查询状态</button>
      </div>
      <div class="hint">飞行模式关闭时模组可正常收发短信，开启后将关闭射频无法使用移动网络</div>
      <div class="result-box" id="flightResult"></div>
    </div>

    <div class="section">
      <div class="section-title">💻 AT 指令调试</div>
      <div id="atLog">等待输入指令...</div>
      <div class="at-input-group">
        <input type="text" id="atCmd" placeholder="输入 AT 指令，如: AT+CSQ">
        <button type="button" onclick="sendAT()" id="atBtn">发送</button>
      </div>
      <div class="btn-group" style="margin-top:10px;">
        <button type="button" class="btn-info" onclick="clearATLog()">🧹 清空日志</button>
      </div>
      <div class="hint">直接向模组串口发送指令并接收响应，请谨慎操作</div>
    </div>
  </div>
  <script>
    function updateCount(el) {
      document.getElementById('charCount').textContent = el.value.length;
    }

    function queryInfo(type) {
      var result = document.getElementById('queryResult');
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在查询，请稍候...';

      fetch('/query?type=' + type)
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            result.className = 'result-box result-info';
            result.innerHTML = data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ 查询失败<br>' + data.message;
          }
        })
        .catch(error => {
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }

    function confirmPing() {
      if (confirm("确定要执行 Ping 操作吗？\n\n这将消耗少量流量。")) {
        doPing();
      }
    }

    function doPing() {
      var btn = document.getElementById('pingBtn');
      var result = document.getElementById('pingResult');

      btn.disabled = true;
      btn.textContent = '⏳ 正在 Ping...';
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在执行 Ping 操作，请稍候（最长等待30秒）...';

      fetch('/ping', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
          btn.disabled = false;
          btn.textContent = '📡 点我消耗一点流量';
          if (data.success) {
            result.className = 'result-box result-success';
            result.innerHTML = '✅ Ping 成功！<br>' + data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ Ping 失败<br>' + data.message;
          }
        })
        .catch(error => {
          btn.disabled = false;
          btn.textContent = '📡 点我消耗一点流量';
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }

    function queryFlightMode() {
      var result = document.getElementById('flightResult');
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在查询飞行模式状态...';

      fetch('/flight?action=query')
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            result.className = 'result-box result-info';
            result.innerHTML = data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ 查询失败: ' + data.message;
          }
        })
        .catch(error => {
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }

    function toggleFlightMode() {
      if (!confirm('确定要切换飞行模式吗？\n\n开启飞行模式后模组将无法收发短信。')) return;

      var btn = document.getElementById('flightBtn');
      var result = document.getElementById('flightResult');
      btn.disabled = true;
      result.className = 'result-box result-loading';
      result.style.display = 'block';
      result.textContent = '正在切换飞行模式...';

      fetch('/flight?action=toggle')
        .then(response => response.json())
        .then(data => {
          btn.disabled = false;
          if (data.success) {
            result.className = 'result-box result-success';
            result.innerHTML = '✅ ' + data.message;
          } else {
            result.className = 'result-box result-error';
            result.innerHTML = '❌ 切换失败: ' + data.message;
          }
        })
        .catch(error => {
          btn.disabled = false;
          result.className = 'result-box result-error';
          result.textContent = '❌ 请求失败: ' + error;
        });
    }

    function addLog(msg, type = 'resp') {
      var log = document.getElementById('atLog');
      var div = document.createElement('div');
      var b = document.createElement('b');

      if (type === 'user') {
        b.style.color = '#fff';
        b.textContent = '> ';
      } else if (type === 'error') {
        b.style.color = '#f44336';
        b.textContent = '❌ ';
      } else {
        b.style.color = '#4CAF50';
        b.textContent = '[RESP] ';
      }

      div.appendChild(b);
      var textNode = document.createTextNode(msg);
      div.appendChild(textNode);

      log.appendChild(div);
      log.scrollTop = log.scrollHeight;
    }

    function sendAT() {
      var input = document.getElementById('atCmd');
      var cmd = input.value.trim();
      if (!cmd) return;

      var btn = document.getElementById('atBtn');
      btn.disabled = true;
      btn.textContent = '...';

      addLog(cmd, 'user');
      input.value = '';

      fetch('/at?cmd=' + encodeURIComponent(cmd))
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            addLog(data.message);
          } else {
            addLog(data.message, 'error');
          }
        })
        .catch(error => {
          addLog('网络错误: ' + error, 'error');
        })
        .finally(() => {
          btn.disabled = false;
          btn.textContent = '发送';
        });
    }

    function clearATLog() {
      document.getElementById('atLog').innerHTML = '';
    }
    document.getElementById('atCmd').addEventListener('keydown', function(event) {
      if (event.key === 'Enter') {
        sendAT();
      }
    });
  </script>
</body>
</html>
)rawliteral";

// API文档页面
const char* htmlApiDocsPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>API 文档</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 20px; background: #fafafa; color: #333; }
    .container { max-width: 1000px; margin: 0 auto; background: white; padding: 30px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.05); }
    h1 { color: #2c3e50; border-bottom: 2px solid #eee; padding-bottom: 10px; margin-top: 0; }
    .nav { margin-bottom: 30px; display: flex; gap: 10px; border-bottom: 1px solid #eee; padding-bottom: 15px; flex-wrap: wrap; }
    .nav a { text-decoration: none; color: #555; padding: 8px 16px; border-radius: 6px; transition: all 0.3s; font-weight: 500; }
    .nav a:hover { background: #f0f0f0; color: #333; }
    .nav a.active { background: #007bff; color: white; }
    .endpoint { border: 1px solid #e0e0e0; border-radius: 8px; margin-bottom: 20px; overflow: hidden; }
    .endpoint-header { display: flex; align-items: center; padding: 15px; cursor: pointer; background: #fdfdfd; transition: background 0.2s; }
    .endpoint-header:hover { background: #f5f5f5; }
    .method { padding: 4px 10px; border-radius: 4px; font-weight: bold; font-size: 14px; margin-right: 15px; color: white; min-width: 50px; text-align: center; }
    .method.get { background: #61affe; }
    .method.post { background: #49cc90; }
    .path { font-family: monospace; font-size: 16px; font-weight: bold; color: #333; flex-grow: 1; }
    .summary { color: #666; font-size: 14px; }
    .endpoint-body { padding: 20px; border-top: 1px solid #e0e0e0; display: none; background: white; }
    .endpoint.open .endpoint-body { display: block; }
    h3 { margin-top: 0; font-size: 16px; color: #444; }
    table { width: 100%; border-collapse: collapse; margin-bottom: 20px; }
    th, td { text-align: left; padding: 10px; border-bottom: 1px solid #eee; }
    th { background: #f9f9f9; color: #555; font-weight: 600; width: 20%; }
    .required { color: #e74c3c; font-size: 12px; margin-left: 5px; }
    input[type="text"] { width: 100%; padding: 8px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }
    .btn-test { background: #007bff; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; font-size: 14px; font-weight: bold; }
    .btn-test:hover { background: #0056b3; }
    .btn-test:disabled { background: #ccc; cursor: not-allowed; }
    .response-area { margin-top: 20px; background: #282c34; border-radius: 6px; padding: 15px; overflow-x: auto; color: #abb2bf; font-family: monospace; font-size: 14px; display: none; white-space: pre-wrap; word-break: break-all; }
    .response-status { font-weight: bold; margin-bottom: 10px; display: inline-block; padding: 3px 8px; border-radius: 4px; font-size: 12px; }
    .response-status.success { background: #49cc90; color: white; }
    .response-status.error { background: #e74c3c; color: white; }
    .param-type { font-family: monospace; color: #888; font-size: 12px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>📱 短信转发器 API 文档</h1>
    <div class="nav">
      <a href="/">⚙️ 系统配置</a>
      <a href="/tools">🧰 工具箱</a>
      <a href="/api-docs" class="active">📚 API文档</a>
    </div>
    <p>在此页面可以查看服务支持的 API 接口，并直接进行调用测试。所有接口均受 Basic Auth 保护，此页面使用当前登录态发请求。</p>
    <p>基础 URL: <code>http://%IP%</code></p>

    <div class="endpoint">
      <div class="endpoint-header" onclick="toggleEndpoint(this)">
        <span class="method post">POST</span>
        <span class="path">/sendsms</span>
        <span class="summary">发送短信</span>
      </div>
      <div class="endpoint-body">
        <form onsubmit="return submitApi(event, '/sendsms', 'POST')">
          <h3>参数 (application/x-www-form-urlencoded)</h3>
          <table>
            <tr><th>名称</th><th>说明</th><th>测试值</th></tr>
            <tr><td>phone<span class="required">*</span><br><span class="param-type">string</span></td><td>目标手机号</td><td><input type="text" name="phone" placeholder="13800138000" required></td></tr>
            <tr><td>content<span class="required">*</span><br><span class="param-type">string</span></td><td>短信内容</td><td><input type="text" name="content" placeholder="测试短信内容..." required></td></tr>
          </table>
          <button type="submit" class="btn-test">发起测试</button>
          <div class="response-area"></div>
        </form>
      </div>
    </div>

    <div class="endpoint">
      <div class="endpoint-header" onclick="toggleEndpoint(this)">
        <span class="method get">GET</span>
        <span class="path">/query</span>
        <span class="summary">模组信息查询</span>
      </div>
      <div class="endpoint-body">
        <form onsubmit="return submitApi(event, '/query', 'GET')">
          <h3>参数</h3>
          <table>
            <tr><th>名称</th><th>说明</th><th>测试值</th></tr>
            <tr><td>type<span class="required">*</span><br><span class="param-type">string</span></td><td>查询类型 (ati, signal, siminfo, network, wifi)</td><td><input type="text" name="type" placeholder="wifi" required></td></tr>
          </table>
          <button type="submit" class="btn-test">发起测试</button>
          <div class="response-area"></div>
        </form>
      </div>
    </div>

    <div class="endpoint">
      <div class="endpoint-header" onclick="toggleEndpoint(this)">
        <span class="method post">POST</span>
        <span class="path">/flight</span>
        <span class="summary">飞行模式控制</span>
      </div>
      <div class="endpoint-body">
        <form onsubmit="return submitApi(event, '/flight', 'POST')">
          <h3>参数 (application/x-www-form-urlencoded)</h3>
          <table>
            <tr><th>名称</th><th>说明</th><th>测试值</th></tr>
            <tr><td>action<span class="required">*</span><br><span class="param-type">string</span></td><td>动作 (toggle, on, off, query)</td><td><input type="text" name="action" placeholder="toggle" required></td></tr>
          </table>
          <button type="submit" class="btn-test">发起测试</button>
          <div class="response-area"></div>
        </form>
      </div>
    </div>

    <div class="endpoint">
      <div class="endpoint-header" onclick="toggleEndpoint(this)">
        <span class="method post">POST</span>
        <span class="path">/ping</span>
        <span class="summary">执行设备 Ping 测试（可能耗时较长）</span>
      </div>
      <div class="endpoint-body">
        <form onsubmit="return submitApi(event, '/ping', 'POST')">
          <p>无需参数。执行 AT+MPING 发送 1 个数据包到 8.8.8.8。</p>
          <button type="submit" class="btn-test">发起测试</button>
          <div class="response-area"></div>
        </form>
      </div>
    </div>

    <div class="endpoint">
      <div class="endpoint-header" onclick="toggleEndpoint(this)">
        <span class="method post">POST</span>
        <span class="path">/at</span>
        <span class="summary">执行任意 AT 指令</span>
      </div>
      <div class="endpoint-body">
        <form onsubmit="return submitApi(event, '/at', 'POST')">
          <h3>参数 (application/x-www-form-urlencoded)</h3>
          <table>
            <tr><th>名称</th><th>说明</th><th>测试值</th></tr>
            <tr><td>cmd<span class="required">*</span><br><span class="param-type">string</span></td><td>AT指令</td><td><input type="text" name="cmd" placeholder="AT+CSQ" required></td></tr>
          </table>
          <button type="submit" class="btn-test">发起测试</button>
          <div class="response-area"></div>
        </form>
      </div>
    </div>

    <div class="endpoint">
      <div class="endpoint-header" onclick="toggleEndpoint(this)">
        <span class="method post">POST</span>
        <span class="path">/test_push</span>
        <span class="summary">测试推送通道</span>
      </div>
      <div class="endpoint-body">
        <form onsubmit="return submitApi(event, '/test_push', 'POST')">
          <h3>参数 (application/x-www-form-urlencoded)</h3>
          <table>
            <tr><th>名称</th><th>说明</th><th>测试值</th></tr>
            <tr><td>type<span class="required">*</span><br><span class="param-type">int</span></td><td>推送类型 (1-10)</td><td><input type="text" name="type" placeholder="1"></td></tr>
            <tr><td>url<br><span class="param-type">string</span></td><td>Webhook/推送地址</td><td><input type="text" name="url" placeholder="https://..."></td></tr>
            <tr><td>key1<br><span class="param-type">string</span></td><td>参数1 (Token/Secret等)</td><td><input type="text" name="key1"></td></tr>
            <tr><td>key2<br><span class="param-type">string</span></td><td>参数2</td><td><input type="text" name="key2"></td></tr>
            <tr><td>body<br><span class="param-type">string</span></td><td>自定义模板体</td><td><input type="text" name="body"></td></tr>
          </table>
          <button type="submit" class="btn-test">发起测试</button>
          <div class="response-area"></div>
        </form>
      </div>
    </div>

  </div>
  <script>
    function toggleEndpoint(element) {
      element.parentElement.classList.toggle('open');
    }
    async function submitApi(event, path, method) {
      event.preventDefault();
      const form = event.target;
      const btn = form.querySelector('.btn-test');
      const responseArea = form.querySelector('.response-area');
      btn.disabled = true;
      responseArea.style.display = 'none';
      let url = path;
      let options = { method: method };
      const formData = new FormData(form);
      const params = new URLSearchParams(formData);
      if (method === 'GET') {
        if (params.toString()) url += '?' + params.toString();
      } else {
        options.headers = { 'Content-Type': 'application/x-www-form-urlencoded' };
        options.body = params.toString();
      }
      try {
        const response = await fetch(url, options);
        const contentType = response.headers.get("content-type");
        let displayContent = '';
        if (contentType && contentType.indexOf("application/json") !== -1) {
          const data = await response.json();
          displayContent = JSON.stringify(data, null, 2);
        } else {
          displayContent = await response.text();
        }
        const escapeHtml = (s) => (s+"").replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
        const statusClass = response.ok ? 'success' : 'error';
        responseArea.innerHTML = '<span class="response-status ' + statusClass + '">HTTP ' + response.status + '</span>\n' + escapeHtml(displayContent);
        responseArea.style.display = 'block';
      } catch (error) {
        responseArea.innerHTML = '<span class="response-status error">Error</span>\n' + error.message;
        responseArea.style.display = 'block';
      } finally {
        btn.disabled = false;
      }
    }
  </script>
</body>
</html>
)rawliteral";

// 检查HTTP Basic认证
bool checkAuth() {
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS Forwarding", "请输入管理员账号密码");
    return false;
  }
  return true;
}

// 处理配置页面请求
void handleRoot() {
  if (!checkAuth()) return;

  String html = String(htmlPage);
  html.replace("%IP%", WiFi.localIP().toString());
  html.replace("%WEB_USER%", config.webUser);
  html.replace("%WEB_PASS%", config.webPass);
  html.replace("%SMTP_SERVER%", config.smtpServer);
  html.replace("%SMTP_PORT%", String(config.smtpPort));
  html.replace("%SMTP_USER%", config.smtpUser);
  html.replace("%SMTP_PASS%", config.smtpPass);
  html.replace("%SMTP_SEND_TO%", config.smtpSendTo);
  html.replace("%ADMIN_PHONE%", config.adminPhone);
  html.replace("%WIFI_SSID%", config.wifiSSID);
  html.replace("%WIFI_PASS%", config.wifiPass);
  html.replace("%NUMBER_BLACK_LIST%", config.numberBlackList);

  // 生成推送通道HTML
  String channelsHtml = "";
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enabledClass = config.pushChannels[i].enabled ? " enabled" : "";
    String checked = config.pushChannels[i].enabled ? " checked" : "";

    channelsHtml += "<div class=\"push-channel" + enabledClass + "\" id=\"channel" + idx + "\">";
    channelsHtml += "<div class=\"push-channel-header\">";
    channelsHtml += "<input type=\"checkbox\" name=\"push" + idx + "en\" id=\"push" + idx + "en\" onchange=\"toggleChannel(" + idx + ")\"" + checked + ">";
    channelsHtml += "<label for=\"push" + idx + "en\" class=\"label-inline\">启用推送通道 " + String(i + 1) + "</label>";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"push-channel-body\">";

    // 通道名称
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>通道名称</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "name\" value=\"" + config.pushChannels[i].name + "\" placeholder=\"自定义名称\">";
    channelsHtml += "</div>";

    // 推送类型
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送方式</label>";
    channelsHtml += "<select name=\"push" + idx + "type\" id=\"push" + idx + "type\" onchange=\"updateTypeHint(" + idx + ")\">";
    channelsHtml += "<option value=\"1\"" + String(config.pushChannels[i].type == PUSH_TYPE_POST_JSON ? " selected" : "") + ">POST JSON（通用格式）</option>";
    channelsHtml += "<option value=\"2\"" + String(config.pushChannels[i].type == PUSH_TYPE_BARK ? " selected" : "") + ">Bark（iOS推送）</option>";
    channelsHtml += "<option value=\"3\"" + String(config.pushChannels[i].type == PUSH_TYPE_GET ? " selected" : "") + ">GET请求（参数在URL中）</option>";
    channelsHtml += "<option value=\"4\"" + String(config.pushChannels[i].type == PUSH_TYPE_DINGTALK ? " selected" : "") + ">钉钉机器人</option>";
    channelsHtml += "<option value=\"5\"" + String(config.pushChannels[i].type == PUSH_TYPE_PUSHPLUS ? " selected" : "") + ">PushPlus</option>";
    channelsHtml += "<option value=\"6\"" + String(config.pushChannels[i].type == PUSH_TYPE_SERVERCHAN ? " selected" : "") + ">Server酱</option>";
    channelsHtml += "<option value=\"7\"" + String(config.pushChannels[i].type == PUSH_TYPE_CUSTOM ? " selected" : "") + ">自定义模板</option>";
    channelsHtml += "<option value=\"8\"" + String(config.pushChannels[i].type == PUSH_TYPE_FEISHU ? " selected" : "") + ">飞书机器人</option>";
    channelsHtml += "<option value=\"9\"" + String(config.pushChannels[i].type == PUSH_TYPE_GOTIFY ? " selected" : "") + ">Gotify</option>";
    channelsHtml += "<option value=\"10\"" + String(config.pushChannels[i].type == PUSH_TYPE_TELEGRAM ? " selected" : "") + ">Telegram Bot</option>";
    channelsHtml += "<option value=\"11\"" + String(config.pushChannels[i].type == PUSH_TYPE_WORK_WEIXIN ? " selected" : "") + ">企业微信机器人</option>";
    channelsHtml += "<option value=\"12\"" + String(config.pushChannels[i].type == PUSH_TYPE_SMS ? " selected" : "") + ">短信转发</option>";
    channelsHtml += "</select>";
    channelsHtml += "<div class=\"push-type-hint\" id=\"hint" + idx + "\"></div>";
    channelsHtml += "</div>";

    // URL
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>推送URL/Webhook</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "url\" value=\"" + config.pushChannels[i].url + "\" placeholder=\"http://your-server.com/api 或 webhook地址\">";
    channelsHtml += "</div>";

    // 额外参数区域（钉钉/PushPlus/Server酱等需要）
    channelsHtml += "<div id=\"extra" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label id=\"key1label" + idx + "\">参数1</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key1\" id=\"key1" + idx + "\" value=\"" + config.pushChannels[i].key1 + "\">";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"form-group\" id=\"key2group" + idx + "\">";
    channelsHtml += "<label id=\"key2label" + idx + "\">参数2</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key2\" id=\"key2" + idx + "\" value=\"" + config.pushChannels[i].key2 + "\">";
    channelsHtml += "</div>";
    channelsHtml += "</div>";

    // 自定义模板区域
    channelsHtml += "<div id=\"custom" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"form-group\">";
    channelsHtml += "<label>请求体模板（使用 {sender} {message} {timestamp} {device} 占位符）</label>";
    channelsHtml += "<textarea name=\"push" + idx + "body\" rows=\"4\" style=\"width:100%;font-family:monospace;\">" + config.pushChannels[i].customBody + "</textarea>";
    channelsHtml += "</div>";
    channelsHtml += "</div>";

    channelsHtml += "</div>";

    // 测试按钮
    channelsHtml += "<button type=\"button\" class=\"btn-test\" id=\"testBtn" + idx + "\" onclick=\"testChannel(" + idx + ")\">🧪 测试</button>";
    channelsHtml += "<div class=\"test-result\" id=\"testResult" + idx + "\"></div>";

    channelsHtml += "</div>";
  }
  html.replace("%PUSH_CHANNELS%", channelsHtml);

  server.send(200, "text/html", html);
}

// 处理工具箱页面请求
void handleToolsPage() {
  if (!checkAuth()) return;

  String html = String(htmlToolsPage);
  html.replace("%IP%", WiFi.localIP().toString());
  server.send(200, "text/html", html);
}

// 处理API文档页面请求
void handleApiDocs() {
  if (!checkAuth()) return;

  String html = String(htmlApiDocsPage);
  html.replace("%IP%", WiFi.localIP().toString());
  server.send(200, "text/html", html);
}

// 处理推送通道测试请求
void handleTestPush() {
  if (!checkAuth()) return;

  // 从POST参数构造临时通道
  PushChannel testCh;
  testCh.enabled = true;
  testCh.type = (PushType)server.arg("type").toInt();
  testCh.url = server.arg("url");
  testCh.key1 = server.arg("key1");
  testCh.key2 = server.arg("key2");
  testCh.customBody = server.arg("body");
  testCh.name = "测试通道";

  // 获取当前时间作为测试时间戳
  String testTimestamp = "测试时间";
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    testTimestamp = String(buf);
  }

  Serial.println("=== 推送通道测试 ===");
  Serial.println("类型: " + String((int)testCh.type));
  Serial.println("URL: " + testCh.url);

  // 发送测试消息
  int httpCode = sendToChannel(testCh, "测试发送者",
    "🧪 这是一条来自【短信转发器】的测试消息，如果您收到此消息，说明推送通道配置正确！",
    testTimestamp.c_str(), devicePhoneNumber.c_str());

  // 构建响应JSON
  String json = "{";
  if (httpCode >= 200 && httpCode < 300) {
    json += "\"success\":true,";
    json += "\"message\":\"测试消息发送成功！(HTTP " + String(httpCode) + ")\"";
  } else if (httpCode > 0) {
    json += "\"success\":false,";
    json += "\"message\":\"服务器返回错误 (HTTP " + String(httpCode) + ")，请检查配置\"";
  } else if (httpCode == -1) {
    json += "\"success\":false,";
    json += "\"message\":\"配置不完整，请检查必填项\"";
  } else {
    json += "\"success\":false,";
    json += "\"message\":\"请求失败 (" + String(httpCode) + ")，请检查URL是否正确\"";
  }
  json += "}";

  server.send(200, "application/json", json);
  Serial.println("=== 推送通道测试完成 ===");
}

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);

  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        delay(50);  // 等待剩余数据
        while (Serial1.available()) resp += (char)Serial1.read();
        return resp;
      }
    }
  }
  return resp;
}

// 处理飞行模式控制请求
void handleFlightMode() {
  if (!checkAuth()) return;

  String action = server.arg("action");
  String json = "{";
  bool success = false;
  String message = "";

  if (action == "query") {
    // 查询当前功能模式
    String resp = sendATCommand("AT+CFUN?", 2000);
    Serial.println("CFUN查询响应: " + resp);

    if (resp.indexOf("+CFUN:") >= 0) {
      success = true;
      int idx = resp.indexOf("+CFUN:");
      int mode = resp.substring(idx + 6).toInt();

      String modeStr;
      String statusIcon;
      if (mode == 0) {
        modeStr = "最小功能模式（关机）";
        statusIcon = "🔴";
      } else if (mode == 1) {
        modeStr = "全功能模式（正常）";
        statusIcon = "🟢";
      } else if (mode == 4) {
        modeStr = "飞行模式（射频关闭）";
        statusIcon = "✈️";
      } else {
        modeStr = "未知模式 (" + String(mode) + ")";
        statusIcon = "❓";
      }

      message = "<table class='info-table'>";
      message += "<tr><td>当前状态</td><td>" + statusIcon + " " + modeStr + "</td></tr>";
      message += "<tr><td>CFUN值</td><td>" + String(mode) + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (action == "toggle") {
    // 先查询当前状态
    String resp = sendATCommand("AT+CFUN?", 2000);
    Serial.println("CFUN查询响应: " + resp);

    if (resp.indexOf("+CFUN:") >= 0) {
      int idx = resp.indexOf("+CFUN:");
      int currentMode = resp.substring(idx + 6).toInt();

      // 切换模式：1(正常) <-> 4(飞行模式)
      int newMode = (currentMode == 1) ? 4 : 1;
      String cmd = "AT+CFUN=" + String(newMode);

      Serial.println("切换飞行模式: " + cmd);
      String setResp = sendATCommand(cmd.c_str(), 5000);
      Serial.println("CFUN设置响应: " + setResp);

      if (setResp.indexOf("OK") >= 0) {
        success = true;
        if (newMode == 4) {
          message = "已开启飞行模式 ✈️<br>模组射频已关闭，无法收发短信";
        } else {
          message = "已关闭飞行模式 🟢<br>模组恢复正常工作";
        }
      } else {
        message = "切换失败: " + setResp;
      }
    } else {
      message = "无法获取当前状态";
    }
  }
  else if (action == "on") {
    // 强制开启飞行模式
    String resp = sendATCommand("AT+CFUN=4", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      message = "已开启飞行模式 ✈️";
    } else {
      message = "开启失败: " + resp;
    }
  }
  else if (action == "off") {
    // 强制关闭飞行模式
    String resp = sendATCommand("AT+CFUN=1", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      message = "已关闭飞行模式 🟢";
    } else {
      message = "关闭失败: " + resp;
    }
  }
  else {
    message = "未知操作";
  }

  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + message + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// 处理AT指令测试请求
void handleATCommand() {
  if (!checkAuth()) return;

  String cmd = server.arg("cmd");
  bool success = false;
  String message = "";

  if (cmd.length() == 0) {
    message = "错误：指令不能为空";
  } else {
    Serial.println("网页端发送AT指令: " + cmd);
    String resp = sendATCommand(cmd.c_str(), 5000);
    Serial.println("模组响应: " + resp);

    if (resp.length() > 0) {
      success = true;
      message = resp;
    } else {
      message = "超时或无响应";
    }
  }

  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// 处理模组信息查询请求
void handleQuery() {
  if (!checkAuth()) return;

  String type = server.arg("type");
  String json = "{";
  bool success = false;
  String message = "";

  if (type == "ati") {
    // 固件信息查询
    String resp = sendATCommand("ATI", 2000);
    Serial.println("ATI响应: " + resp);

    if (resp.indexOf("OK") >= 0) {
      success = true;
      // 解析ATI响应
      String manufacturer = "未知";
      String model = "未知";
      String version = "未知";

      // 按行解析
      int lineStart = 0;
      int lineNum = 0;
      for (int i = 0; i < resp.length(); i++) {
        if (resp.charAt(i) == '\n' || i == resp.length() - 1) {
          String line = resp.substring(lineStart, i);
          line.trim();
          if (line.length() > 0 && line != "ATI" && line != "OK") {
            lineNum++;
            if (lineNum == 1) manufacturer = line;
            else if (lineNum == 2) model = line;
            else if (lineNum == 3) version = line;
          }
          lineStart = i + 1;
        }
      }

      message = "<table class='info-table'>";
      message += "<tr><td>制造商</td><td>" + manufacturer + "</td></tr>";
      message += "<tr><td>模组型号</td><td>" + model + "</td></tr>";
      message += "<tr><td>固件版本</td><td>" + version + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "signal") {
    // 信号质量查询
    String resp = sendATCommand("AT+CESQ", 2000);
    Serial.println("CESQ响应: " + resp);

    if (resp.indexOf("+CESQ:") >= 0) {
      success = true;
      // 解析 +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
      int idx = resp.indexOf("+CESQ:");
      String params = resp.substring(idx + 6);
      int endIdx = params.indexOf('\r');
      if (endIdx < 0) endIdx = params.indexOf('\n');
      if (endIdx > 0) params = params.substring(0, endIdx);
      params.trim();

      // 分割参数
      String values[6];
      int valIdx = 0;
      int startPos = 0;
      for (int i = 0; i <= params.length() && valIdx < 6; i++) {
        if (i == params.length() || params.charAt(i) == ',') {
          values[valIdx] = params.substring(startPos, i);
          values[valIdx].trim();
          valIdx++;
          startPos = i + 1;
        }
      }

      // RSRP转换为dBm (0-97映射到-140到-44 dBm, 99表示未知)
      int rsrp = values[5].toInt();
      String rsrpStr;
      if (rsrp == 99 || rsrp == 255) {
        rsrpStr = "未知";
      } else {
        int rsrpDbm = -140 + rsrp;
        rsrpStr = String(rsrpDbm) + " dBm";
        if (rsrpDbm >= -80) rsrpStr += " (信号极好)";
        else if (rsrpDbm >= -90) rsrpStr += " (信号良好)";
        else if (rsrpDbm >= -100) rsrpStr += " (信号一般)";
        else if (rsrpDbm >= -110) rsrpStr += " (信号较弱)";
        else rsrpStr += " (信号很差)";
      }

      // RSRQ转换 (0-34映射到-19.5到-3 dB)
      int rsrq = values[4].toInt();
      String rsrqStr;
      if (rsrq == 99 || rsrq == 255) {
        rsrqStr = "未知";
      } else {
        float rsrqDb = -19.5 + rsrq * 0.5;
        rsrqStr = String(rsrqDb, 1) + " dB";
      }

      message = "<table class='info-table'>";
      message += "<tr><td>信号强度 (RSRP)</td><td>" + rsrpStr + "</td></tr>";
      message += "<tr><td>信号质量 (RSRQ)</td><td>" + rsrqStr + "</td></tr>";
      message += "<tr><td>原始数据</td><td>" + params + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "siminfo") {
    // SIM卡信息查询
    success = true;
    message = "<table class='info-table'>";

    // 查询IMSI
    String resp = sendATCommand("AT+CIMI", 2000);
    String imsi = "未知";
    if (resp.indexOf("OK") >= 0) {
      int start = resp.indexOf('\n');
      if (start >= 0) {
        int end = resp.indexOf('\n', start + 1);
        if (end < 0) end = resp.indexOf('\r', start + 1);
        if (end > start) {
          imsi = resp.substring(start + 1, end);
          imsi.trim();
          if (imsi == "OK" || imsi.length() < 10) imsi = "未知";
        }
      }
    }
    message += "<tr><td>IMSI</td><td>" + imsi + "</td></tr>";

    // 查询ICCID
    resp = sendATCommand("AT+ICCID", 2000);
    String iccid = "未知";
    if (resp.indexOf("+ICCID:") >= 0) {
      int idx = resp.indexOf("+ICCID:");
      String tmp = resp.substring(idx + 7);
      int endIdx = tmp.indexOf('\r');
      if (endIdx < 0) endIdx = tmp.indexOf('\n');
      if (endIdx > 0) iccid = tmp.substring(0, endIdx);
      iccid.trim();
    }
    message += "<tr><td>ICCID</td><td>" + iccid + "</td></tr>";

    // 查询本机号码 (如果SIM卡支持)
    resp = sendATCommand("AT+CNUM", 2000);
    String phoneNum = "未存储或不支持";
    if (resp.indexOf("+CNUM:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          phoneNum = resp.substring(idx + 2, endIdx);
          // 更新全局本机号码缓存
          if (devicePhoneNumber == "未知号码" && phoneNum.length() > 5) {
            devicePhoneNumber = phoneNum;
            Serial.println("查询已更新本机号码缓存: " + devicePhoneNumber);
          }
        }
      }
    }
    message += "<tr><td>本机号码</td><td>" + phoneNum + "</td></tr>";

    message += "</table>";
  }
  else if (type == "network") {
    // 网络状态查询
    success = true;
    message = "<table class='info-table'>";

    // 查询网络注册状态
    String resp = sendATCommand("AT+CEREG?", 2000);
    String regStatus = "未知";
    if (resp.indexOf("+CEREG:") >= 0) {
      int idx = resp.indexOf("+CEREG:");
      String tmp = resp.substring(idx + 7);
      int commaIdx = tmp.indexOf(',');
      if (commaIdx >= 0) {
        String stat = tmp.substring(commaIdx + 1, commaIdx + 2);
        int s = stat.toInt();
        switch(s) {
          case 0: regStatus = "未注册，未搜索"; break;
          case 1: regStatus = "已注册，本地网络"; break;
          case 2: regStatus = "未注册，正在搜索"; break;
          case 3: regStatus = "注册被拒绝"; break;
          case 4: regStatus = "未知"; break;
          case 5: regStatus = "已注册，漫游"; break;
          default: regStatus = "状态码: " + stat;
        }
      }
    }
    message += "<tr><td>网络注册</td><td>" + regStatus + "</td></tr>";

    // 查询运营商
    resp = sendATCommand("AT+COPS?", 2000);
    String oper = "未知";
    if (resp.indexOf("+COPS:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          oper = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>运营商</td><td>" + oper + "</td></tr>";

    // 查询PDP上下文激活状态
    resp = sendATCommand("AT+CGACT?", 2000);
    String pdpStatus = "未激活";
    if (resp.indexOf("+CGACT: 1,1") >= 0) {
      pdpStatus = "已激活";
    } else if (resp.indexOf("+CGACT:") >= 0) {
      pdpStatus = "未激活";
    }
    message += "<tr><td>数据连接</td><td>" + pdpStatus + "</td></tr>";

    // 查询APN
    resp = sendATCommand("AT+CGDCONT?", 2000);
    String apn = "未知";
    if (resp.indexOf("+CGDCONT:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        idx = resp.indexOf(",\"", idx + 2);  // 跳过PDP类型
        if (idx >= 0) {
          int endIdx = resp.indexOf("\"", idx + 2);
          if (endIdx > idx) {
            apn = resp.substring(idx + 2, endIdx);
            if (apn.length() == 0) apn = "(自动)";
          }
        }
      }
    }
    message += "<tr><td>APN</td><td>" + apn + "</td></tr>";

    message += "</table>";
  }
  else if (type == "wifi") {
    // WiFi状态查询
    success = true;
    message = "<table class='info-table'>";

    // WiFi连接状态
    String wifiStatus = WiFi.isConnected() ? "已连接" : "未连接";
    message += "<tr><td>连接状态</td><td>" + wifiStatus + "</td></tr>";

    // SSID
    String ssid = WiFi.SSID();
    if (ssid.length() == 0) ssid = "未知";
    message += "<tr><td>当前SSID</td><td>" + ssid + "</td></tr>";

    // 信号强度 RSSI
    int rssi = WiFi.RSSI();
    String rssiStr = String(rssi) + " dBm";
    if (rssi >= -50) rssiStr += " (信号极好)";
    else if (rssi >= -60) rssiStr += " (信号很好)";
    else if (rssi >= -70) rssiStr += " (信号良好)";
    else if (rssi >= -80) rssiStr += " (信号一般)";
    else if (rssi >= -90) rssiStr += " (信号较弱)";
    else rssiStr += " (信号很差)";
    message += "<tr><td>信号强度 (RSSI)</td><td>" + rssiStr + "</td></tr>";

    // IP地址
    message += "<tr><td>IP地址</td><td>" + WiFi.localIP().toString() + "</td></tr>";

    // 网关
    message += "<tr><td>网关</td><td>" + WiFi.gatewayIP().toString() + "</td></tr>";

    // 子网掩码
    message += "<tr><td>子网掩码</td><td>" + WiFi.subnetMask().toString() + "</td></tr>";

    // DNS
    message += "<tr><td>DNS服务器</td><td>" + WiFi.dnsIP().toString() + "</td></tr>";

    // MAC地址
    message += "<tr><td>MAC地址</td><td>" + WiFi.macAddress() + "</td></tr>";

    // BSSID (路由器MAC)
    message += "<tr><td>路由器BSSID</td><td>" + WiFi.BSSIDstr() + "</td></tr>";

    // 信道
    message += "<tr><td>WiFi信道</td><td>" + String(WiFi.channel()) + "</td></tr>";

    message += "</table>";
  }
  else {
    message = "未知的查询类型";
  }

  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + message + "\"";
  json += "}";

  server.send(200, "application/json", json);
}


// 处理发送短信请求
void handleSendSms() {
  if (!checkAuth()) return;

  String phone = server.arg("phone");
  String content = server.arg("content");

  phone.trim();
  content.trim();

  bool success = false;
  String resultMsg = "";

  if (phone.length() == 0) {
    resultMsg = "错误：请输入目标号码";
  } else if (content.length() == 0) {
    resultMsg = "错误：请输入短信内容";
  } else {
    Serial.println("网页端发送短信请求");
    Serial.println("目标号码: " + phone);
    Serial.println("短信内容: " + content);

    success = sendSMS(phone.c_str(), content.c_str());
    resultMsg = success ? "短信发送成功！" : "短信发送失败，请检查模组状态";
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/sms">
  <title>发送结果</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .result { padding: 20px; border-radius: 10px; display: inline-block; }
    .success { background: #4CAF50; color: white; }
    .error { background: #f44336; color: white; }
  </style>
</head>
<body>
  <div class="result %CLASS%">
    <h2>%ICON% %MSG%</h2>
    <p>3秒后返回发送页面...</p>
  </div>
</body>
</html>
)rawliteral";

  html.replace("%CLASS%", success ? "success" : "error");
  html.replace("%ICON%", success ? "✅" : "❌");
  html.replace("%MSG%", resultMsg);

  server.send(200, "text/html", html);
}

// 处理Ping请求
void handlePing() {
  if (!checkAuth()) return;

  Serial.println("网页端发起Ping请求");

  // 清空串口缓冲区
  while (Serial1.available()) Serial1.read();

  // 激活PDP上下文（数据连接）
  Serial.println("激活数据连接(CGACT)...");
  String activateResp = sendATCommand("AT+CGACT=1,1", 10000);
  Serial.println("CGACT响应: " + activateResp);

  // 检查激活是否成功（OK或已激活的情况）
  bool networkActivated = (activateResp.indexOf("OK") >= 0);
  if (!networkActivated) {
    Serial.println("数据连接激活失败，尝试继续执行...");
  }

  // 清空串口缓冲区
  while (Serial1.available()) Serial1.read();
  delay(500);  // 等待网络稳定

  // 发送MPING命令，ping 8.8.8.8，超时30秒，ping 1次
  Serial1.println("AT+MPING=\"8.8.8.8\",30,1");

  // 等待响应
  unsigned long start = millis();
  String resp = "";
  bool gotOK = false;
  bool gotError = false;
  bool gotPingResult = false;
  String pingResultMsg = "";

  // 等待最多35秒（30秒超时 + 5秒余量）
  while (millis() - start < 35000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      Serial.print(c);  // 调试输出

      // 检查是否收到OK
      if (resp.indexOf("OK") >= 0 && !gotOK) {
        gotOK = true;
      }

      // 检查是否收到ERROR
      if (resp.indexOf("+CME ERROR") >= 0 || resp.indexOf("ERROR") >= 0) {
        gotError = true;
        pingResultMsg = "模组返回错误";
        break;
      }

      // 检查是否收到Ping结果URC
      // 成功格式: +MPING: 1,8.8.8.8,32,xxx,xxx
      // 失败格式: +MPING: 2 或其他
      int mpingIdx = resp.indexOf("+MPING:");
      if (mpingIdx >= 0) {
        // 找到换行符确定完整的一行
        int lineEnd = resp.indexOf('\n', mpingIdx);
        if (lineEnd >= 0) {
          String mpingLine = resp.substring(mpingIdx, lineEnd);
          mpingLine.trim();
          Serial.println("收到MPING结果: " + mpingLine);

          // 解析结果
          // +MPING: <result>[,<ip>,<packet_len>,<time>,<ttl>]
          int colonIdx = mpingLine.indexOf(':');
          if (colonIdx >= 0) {
            String params = mpingLine.substring(colonIdx + 1);
            params.trim();

            // 获取第一个参数（result）
            int commaIdx = params.indexOf(',');
            String resultStr;
            if (commaIdx >= 0) {
              resultStr = params.substring(0, commaIdx);
            } else {
              resultStr = params;
            }
            resultStr.trim();
            int result = resultStr.toInt();

            gotPingResult = true;

            // result=0或1都表示成功（不同模组可能返回不同值）
            // 如果有完整的响应参数（IP、时间等），也视为成功
            bool pingSuccess = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);

            if (pingSuccess) {
              // 成功，解析详细信息
              // 格式: 0/1,"8.8.8.8",16,时间,TTL
              int idx1 = params.indexOf(',');
              if (idx1 >= 0) {
                String rest = params.substring(idx1 + 1);
                // 处理IP地址（可能带引号）
                String ip;
                int idx2;
                if (rest.startsWith("\"")) {
                  // 带引号的IP
                  int quoteEnd = rest.indexOf('\"', 1);
                  if (quoteEnd >= 0) {
                    ip = rest.substring(1, quoteEnd);
                    idx2 = rest.indexOf(',', quoteEnd);
                  } else {
                    idx2 = rest.indexOf(',');
                    ip = rest.substring(0, idx2);
                  }
                } else {
                  idx2 = rest.indexOf(',');
                  ip = rest.substring(0, idx2);
                }

                if (idx2 >= 0) {
                  rest = rest.substring(idx2 + 1);
                  int idx3 = rest.indexOf(',');  // packet_len后
                  if (idx3 >= 0) {
                    rest = rest.substring(idx3 + 1);
                    int idx4 = rest.indexOf(',');  // time后
                    String timeStr, ttlStr;
                    if (idx4 >= 0) {
                      timeStr = rest.substring(0, idx4);
                      ttlStr = rest.substring(idx4 + 1);
                    } else {
                      timeStr = rest;
                      ttlStr = "N/A";
                    }
                    timeStr.trim();
                    ttlStr.trim();
                    pingResultMsg = "目标: " + ip + ", 延迟: " + timeStr + "ms, TTL: " + ttlStr;
                  }
                }
              }
              if (pingResultMsg.length() == 0) {
                pingResultMsg = "Ping成功";
              }
            } else {
              // 失败
              pingResultMsg = "Ping超时或目标不可达 (错误码: " + String(result) + ")";
            }
            break;
          }
        }
      }
    }

    if (gotError || gotPingResult) break;
    delay(10);
  }

  Serial.println("\nPing操作完成");

  // 关闭数据连接以节省流量
  Serial.println("关闭PDP上下文(CGACT=0)...");
  String deactivateResp = sendATCommand("AT+CGACT=0,1", 5000);
  Serial.println("CGACT关闭响应: " + deactivateResp);

  // 构建JSON响应
  String json = "{";
  if (gotPingResult && pingResultMsg.indexOf("延迟") >= 0) {
    json += "\"success\":true,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotError) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotPingResult) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else {
    json += "\"success\":false,";
    json += "\"message\":\"操作超时，未收到Ping结果\"";
  }
  json += "}";

  server.send(200, "application/json", json);
}

// 处理保存配置请求
void handleSave() {
  if (!checkAuth()) return;

  // 获取新的Web账号密码
  String newWebUser = server.arg("webUser");
  String newWebPass = server.arg("webPass");

  // 验证Web账号密码不能为空
  if (newWebUser.length() == 0) newWebUser = DEFAULT_WEB_USER;
  if (newWebPass.length() == 0) newWebPass = DEFAULT_WEB_PASS;

  config.webUser = newWebUser;
  config.webPass = newWebPass;
  config.smtpServer = server.arg("smtpServer");
  config.smtpPort = server.arg("smtpPort").toInt();
  if (config.smtpPort == 0) config.smtpPort = 465;
  config.smtpUser = server.arg("smtpUser");
  config.smtpPass = server.arg("smtpPass");
  config.smtpSendTo = server.arg("smtpSendTo");
  config.adminPhone = server.arg("adminPhone");

  // WiFi配置（变更后重启）
  String newSSID = server.arg("wifiSSID");
  String newPass = server.arg("wifiPass");
  bool wifiChanged = (newSSID != config.wifiSSID || newPass != config.wifiPass);
  config.wifiSSID = newSSID;
  config.wifiPass = newPass;

  // 号码黑名单
  config.numberBlackList = server.arg("numberBlackList");

  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    config.pushChannels[i].enabled = server.arg("push" + idx + "en") == "on";
    config.pushChannels[i].type = (PushType)server.arg("push" + idx + "type").toInt();
    config.pushChannels[i].url = server.arg("push" + idx + "url");
    config.pushChannels[i].name = server.arg("push" + idx + "name");
    config.pushChannels[i].key1 = server.arg("push" + idx + "key1");
    config.pushChannels[i].key2 = server.arg("push" + idx + "key2");
    config.pushChannels[i].customBody = server.arg("push" + idx + "body");
    if (config.pushChannels[i].name.length() == 0) {
      config.pushChannels[i].name = "通道" + String(i + 1);
    }
  }

  saveConfig();
  configValid = isConfigValid();

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/">
  <title>保存成功</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .success { background: #4CAF50; color: white; padding: 20px; border-radius: 10px; display: inline-block; }
  </style>
</head>
<body>
  <div class="success">
    <h2>✅ 配置保存成功！</h2>
    <p>3秒后返回配置页面...</p>
    <p>如果修改了账号密码，请使用新的账号密码登录</p>
  </div>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);

  // 如果配置有效，发送启动通知
  if (configValid) {
    Serial.println("配置有效，发送启动通知...");
    String subject = "短信转发器配置已更新";
    String body = "设备配置已更新\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }

  // WiFi配置变更，需要重启
  if (wifiChanged) {
    Serial.println("WiFi配置已更改，即将重启...");
    delay(2000);
    ESP.restart();
  }
}

// 发送邮件通知函数
void sendEmailNotification(const char* subject, const char* body) {
  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 ||
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    Serial.println("邮件配置不完整，跳过发送");
    return;
  }

  auto statusCallback = [](SMTPStatus status) {
    Serial.println(status.text);
  };
  smtp.connect(config.smtpServer.c_str(), config.smtpPort, statusCallback);
  if (smtp.isConnected()) {
    smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password);

    SMTPMessage msg;
    String from = "sms notify <"; from += config.smtpUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += config.smtpSendTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    msg.timestamp = time(nullptr);
    smtp.send(msg);
    Serial.println("邮件发送完成");
  } else {
    Serial.println("邮件服务器连接失败");
  }
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  Serial.println("准备发送短信...");
  Serial.print("目标号码: "); Serial.println(phoneNumber);
  Serial.print("短信内容: "); Serial.println(message);

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);

  if (pduLen < 0) {
    Serial.print("PDU编码失败，错误码: ");
    Serial.println(pduLen);
    return false;
  }

  Serial.print("PDU数据: "); Serial.println(pdu.getSMS());
  Serial.print("PDU长度: "); Serial.println(pduLen);

  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;

  while (Serial1.available()) Serial1.read();
  Serial1.println(cmgsCmd);

  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      Serial.print(c);
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
  }

  if (!gotPrompt) {
    Serial.println("未收到>提示符");
    return false;
  }

  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束

  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      Serial.print(c);
      if (resp.indexOf("OK") >= 0) {
        Serial.println("\n短信发送成功");
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        Serial.println("\n短信发送失败");
        return false;
      }
    }
  }
  Serial.println("短信发送超时");
  return false;
}

// 新增“模组断电重启”函数
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);

  Serial.println("EN 拉低：关闭模组");
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1200);  // 关机时间给够

  Serial.println("EN 拉高：开启模组");
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(6000);  // 等模组完全启动再发AT（关键）
}


// 重启模组
void resetModule() {
  Serial.println("正在硬重启模组（EN 断电重启）...");

  modemPowerCycle();

  // 清掉上电噪声/残留
  while (Serial1.available()) Serial1.read();

  // 硬重启后做 AT 握手确认（最多等 10 秒）
  bool ok = false;
  for (int i = 0; i < 10; i++) {
    if (sendATandWaitOK("AT", 1000)) {
      ok = true;
      break;
    }
    Serial.println("AT未响应，继续等模组启动...");
  }

  if (ok) Serial.println("模组AT恢复正常");
  else    Serial.println("模组AT仍未响应（检查EN接线/供电/波特率）");
}


// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;

  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = config.adminPhone;

  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }

  return senderStr.equals(adminStr);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();

  Serial.println("处理管理员命令: " + cmd);

  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);

    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);

      targetPhone.trim();
      smsContent.trim();

      Serial.println("目标号码: " + targetPhone);
      Serial.println("短信内容: " + smsContent);

      bool success = sendSMS(targetPhone.c_str(), smsContent.c_str());

      // 发送邮件通知结果
      String subject = success ? "短信发送成功" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "执行结果: " + String(success ? "成功" : "失败");

      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      Serial.println("SMS命令格式错误");
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    Serial.println("执行RESET命令");

    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");

    // 重启模组
    resetModule();

    // 重启ESP32
    Serial.println("正在重启ESP32...");
    delay(1000);
    ESP.restart();
  }
  else {
    Serial.println("未知命令: " + cmd);
  }
}

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse &&
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }

  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }

  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }

  // 覆盖最老的槽位
  Serial.println("⚠️ 长短信缓存已满，覆盖最老的槽位");
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  String result = "";
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 前置声明
void processSmsContent(const char* sender, const char* text, const char* timestamp);

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        Serial.println("⏰ 长短信超时，强制转发不完整消息");
        Serial.printf("  参考号: %d, 已收到: %d/%d\n",
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);

        // 合并已收到的分段
        String fullText = assembleConcatSms(i);

        // 处理短信内容
        processSmsContent(concatBuffer[i].sender.c_str(),
                         fullText.c_str(),
                         concatBuffer[i].timestamp.c_str());

        // 清空槽位
        clearConcatSlot(i);
      }
    }
  }
}

// 发送短信数据到服务器
// URL编码辅助函数
String urlEncode(const String& str) {
  String encoded = "";
  char c;
  char code0;
  char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

// 钉钉签名函数（时间戳为UTC毫秒级）
String dingtalkSign(const String& secret, int64_t timestamp) {
  String stringToSign = String(timestamp) + "\n" + secret;

  uint8_t hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)secret.c_str(), secret.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  String base64Encoded = base64::encode(hmacResult, 32);
  return urlEncode(base64Encoded);
}

// 获取当前UTC毫秒级时间戳（用于钉钉签名）
int64_t getUtcMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  }
  // 如果获取失败，使用time()函数
  return (int64_t)time(nullptr) * 1000LL;
}

// JSON转义函数
String jsonEscape(const String& str) {
  String result = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '"') result += "\\\"";
    else if (c == '\\') result += "\\\\";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += c;
  }
  return result;
}

// 检查发送者是否在号码黑名单中
bool isInNumberBlackList(const char* sender) {
  if (config.numberBlackList.length() == 0) return false;

  String senderStr = String(sender);
  // 去除发送者号码的+86前缀
  if (senderStr.startsWith("+86")) senderStr = senderStr.substring(3);

  String list = config.numberBlackList;
  int start = 0;
  while (start <= (int)list.length()) {
    int end = list.indexOf('\n', start);
    if (end == -1) end = list.length();
    String line = list.substring(start, end);
    line.trim();
    if (line.length() > 0 && line.equals(senderStr)) return true;
    start = end + 1;
  }
  return false;
}

// 格式化时间戳：将 YYMMDDHHMMSS 转换为 YY/MM/DD HH:MM:SS
String formatTimestamp(const char* timestamp) {
  if (timestamp == nullptr || strlen(timestamp) < 12) {
    return String(timestamp ? timestamp : "");
  }
  char formatted[20];
  snprintf(formatted, sizeof(formatted), "%.2s/%.2s/%.2s %.2s:%.2s:%.2s",
           timestamp,       // YY
           timestamp + 2,   // MM
           timestamp + 4,   // DD
           timestamp + 6,   // HH
           timestamp + 8,   // MM
           timestamp + 10   // SS
  );
  return String(formatted);
}

// 提取验证码：从字符串中提取4位及以上的连续数字
String extractVerifyCode(const char* text) {
  if (text == nullptr) return "";
  String result = "";
  String currentNum = "";
  for (int i = 0; text[i] != '\0'; i++) {
    if (text[i] >= '0' && text[i] <= '9') {
      currentNum += text[i];
    } else {
      if (currentNum.length() >= 4) { result = currentNum; break; }
      currentNum = "";
    }
  }
  if (result.length() == 0 && currentNum.length() >= 4) result = currentNum;
  return result;
}

// 向钉钉发送纯文本（用于单独推送验证码）
void sendTextToDingtalk(const PushChannel& channel, const String& text) {
  if (channel.type != PUSH_TYPE_DINGTALK || channel.url.length() == 0) return;
  HTTPClient http;
  String webhookUrl = channel.url;
  if (channel.key1.length() > 0) {
    int64_t ts = getUtcMillis();
    String sign = dingtalkSign(channel.key1, ts);
    webhookUrl += (webhookUrl.indexOf('?') == -1 ? "?" : "&");
    char tsBuf[21];
    snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
    webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + sign;
  }
  http.begin(webhookUrl);
  http.addHeader("Content-Type", "application/json");
  String jsonData = "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + jsonEscape(text) + "\"}}";
  Serial.println("钉钉验证码推送: " + jsonData);
  int httpCode = http.POST(jsonData);
  if (httpCode > 0) Serial.println("钉钉验证码响应码: " + String(httpCode));
  else Serial.println("钉钉验证码推送失败: " + http.errorToString(httpCode));
  http.end();
}

// 发送单个推送通道，返回HTTP状态码（>0表示有响应，-1表示跳过）
int sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp, const char* devicePhone) {
  if (!channel.enabled) return -1;

  // 对于某些推送方式，URL可以为空（使用默认URL）
  bool needUrl = (channel.type == PUSH_TYPE_POST_JSON || channel.type == PUSH_TYPE_BARK ||
                  channel.type == PUSH_TYPE_GET || channel.type == PUSH_TYPE_DINGTALK ||
                  channel.type == PUSH_TYPE_CUSTOM);
  if (needUrl && channel.url.length() == 0) return -1;

  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  Serial.println("发送到推送通道: " + channelName);

  String senderEscaped = jsonEscape(String(sender));
  String messageEscaped = jsonEscape(String(message));
  String timestampEscaped = jsonEscape(formatTimestamp(timestamp));
  String devicePhoneEscaped = jsonEscape(String(devicePhone ? devicePhone : ""));

  if (channel.type == PUSH_TYPE_SMS) {
    String phone = channel.url;
    String content = "📱短信通知\\n发送者: " + senderEscaped + "\\n接收卡号: " + devicePhoneEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;

    phone.trim();
    content.trim();

    String resultMsg = "";
    bool success = sendSMS(phone.c_str(), content.c_str());
    resultMsg = success ? "短信发送成功！" : "短信发送失败，请检查模组状态";
    Serial.printf("[%s] 响应码: %s\n", channelName.c_str(), resultMsg.c_str());
    return success ? 0 : -1;
  }

  HTTPClient http;
  int httpCode = 0;

  switch (channel.type) {
    case PUSH_TYPE_POST_JSON: {
      // 标准POST JSON格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"sender\":\"" + senderEscaped + "\",";
      jsonData += "\"message\":\"" + messageEscaped + "\",";
      jsonData += "\"timestamp\":\"" + timestampEscaped + "\",";
      jsonData += "\"device\":\"" + devicePhoneEscaped + "\"";
      jsonData += "}";
      Serial.println("POST JSON: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_BARK: {
      // Bark推送格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"title\":\"" + senderEscaped + "\",";
      jsonData += "\"body\":\"" + messageEscaped + "\\n接收卡号: " + devicePhoneEscaped + "\"";
      jsonData += "}";
      Serial.println("BARK JSON: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_GET: {
      // GET请求，参数放URL里
      String getUrl = channel.url;
      if (getUrl.indexOf('?') == -1) {
        getUrl += "?";
      } else {
        getUrl += "&";
      }
      getUrl += "sender=" + urlEncode(String(sender));
      getUrl += "&message=" + urlEncode(String(message));
      getUrl += "&timestamp=" + urlEncode(String(timestamp));
      getUrl += "&device=" + urlEncode(String(devicePhone ? devicePhone : ""));
      Serial.println("GET URL: " + getUrl);
      http.begin(getUrl);
      httpCode = http.GET();
      break;
    }

    case PUSH_TYPE_DINGTALK: {
      // 钉钉机器人
      String webhookUrl = channel.url;

      // 如果配置了secret，需要添加签名
      if (channel.key1.length() > 0) {
        // 获取UTC毫秒级时间戳（钉钉要求）
        int64_t ts = getUtcMillis();
        String sign = dingtalkSign(channel.key1, ts);
        if (webhookUrl.indexOf('?') == -1) {
          webhookUrl += "?";
        } else {
          webhookUrl += "&";
        }
        // 使用字符串拼接避免int64_t转换问题
        char tsBuf[21];
        snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
        webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + sign;
      }

      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{\"msgtype\":\"text\",\"text\":{\"content\":\"";
      jsonData += "📱短信通知\\n发送者: " + senderEscaped + "\\n接收卡号: " + devicePhoneEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      jsonData += "\"}}";
      Serial.println("钉钉: " + jsonData);
      httpCode = http.POST(jsonData);

      // 验证码检测：自动提取并单独推送
      {
        String msgStr = String(message);
        if (msgStr.indexOf("验证码") != -1 || msgStr.indexOf("验证") != -1) {
          String code = extractVerifyCode(message);
          if (code.length() > 0) {
            Serial.println("检测到验证码: " + code);
            sendTextToDingtalk(channel, code);
          }
        }
      }
      break;
    }

    case PUSH_TYPE_PUSHPLUS: {
      // PushPlus
      String pushUrl = channel.url.length() > 0 ? channel.url : "http://www.pushplus.plus/send";
      http.begin(pushUrl);
      http.addHeader("Content-Type", "application/json");
      // 发送渠道
      String channelValue = "wechat";
      if (channel.key2.length() > 0) {
          // 仅支持微信公众号（wechat）、浏览器插件（extension）和 PushPlus App（app）三种渠道
          if (channel.key2 == "wechat" || channel.key2 == "extension" || channel.key2 == "app") {
              channelValue = channel.key2;
          } else {
              Serial.println("Invalid PushPlus channel '" + channel.key2 + "'. Using default 'wechat'.");
          }
      }
      String jsonData = "{";
      jsonData += "\"token\":\"" + channel.key1 + "\",";
      jsonData += "\"title\":\"短信来自: " + senderEscaped + "\",";
      jsonData += "\"content\":\"<b>发送者:</b> " + senderEscaped + "<br><b>接收卡号:</b> " + devicePhoneEscaped + "<br><b>时间:</b> " + timestampEscaped + "<br><b>内容:</b><br>" + messageEscaped + "\",";
      jsonData += "\"channel\":\"" + channelValue + "\"";
      jsonData += "}";
      Serial.println("PushPlus: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_SERVERCHAN: {
      // Server酱
      String scUrl = channel.url.length() > 0 ? channel.url : ("https://sctapi.ftqq.com/" + channel.key1 + ".send");
      http.begin(scUrl);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String postData = "title=" + urlEncode("短信来自: " + String(sender));
      postData += "&desp=" + urlEncode("**发送者:** " + String(sender) + "\n\n**接收卡号:** " + String(devicePhone ? devicePhone : "") + "\n\n**时间:** " + String(timestamp) + "\n\n**内容:**\n\n" + String(message));
      Serial.println("Server酱: " + postData);
      httpCode = http.POST(postData);
      break;
    }

    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        Serial.println("自定义模板为空，跳过");
        return -1;
      }
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String body = channel.customBody;
      body.replace("{sender}", senderEscaped);
      body.replace("{message}", messageEscaped);
      body.replace("{timestamp}", timestampEscaped);
      body.replace("{device}", devicePhoneEscaped);
      Serial.println("自定义: " + body);
      httpCode = http.POST(body);
      break;
    }

    case PUSH_TYPE_FEISHU: {
      // 飞书机器人
      String webhookUrl = channel.url;
      String jsonData = "{";

      // 如果配置了secret，需要添加签名
      if (channel.key1.length() > 0) {
        // 飞书使用秒级时间戳
        int64_t ts = time(nullptr);
        // 飞书签名: base64(hmac-sha256(timestamp + "\n" + secret, secret))
        String stringToSign = String(ts) + "\n" + channel.key1;
        uint8_t hmacResult[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&ctx, (const unsigned char*)channel.key1.c_str(), channel.key1.length());
        mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
        mbedtls_md_hmac_finish(&ctx, hmacResult);
        mbedtls_md_free(&ctx);
        String sign = base64::encode(hmacResult, 32);

        jsonData += "\"timestamp\":\"" + String(ts) + "\",";
        jsonData += "\"sign\":\"" + sign + "\",";
      }

      // 飞书消息体
      jsonData += "\"msg_type\":\"text\",";
      jsonData += "\"content\":{\"text\":\"";
      jsonData += "📱短信通知\\n发送者: " + senderEscaped + "\\n接收卡号: " + devicePhoneEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      jsonData += "\"}}";

      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      Serial.println("飞书: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_GOTIFY: {
      // Gotify 推送
      String gotifyUrl = channel.url;
      // 确保URL以/结尾
      if (!gotifyUrl.endsWith("/")) gotifyUrl += "/";
      gotifyUrl += "message?token=" + channel.key1;

      http.begin(gotifyUrl);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"title\":\"短信来自: " + senderEscaped + "\",";
      jsonData += "\"message\":\"接收卡号: " + devicePhoneEscaped + "\\n\\n" + messageEscaped + "\\n\\n时间: " + timestampEscaped + "\",";
      jsonData += "\"priority\":5";
      jsonData += "}";
      Serial.println("Gotify: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_TELEGRAM: {
      // Telegram Bot 推送
      // channel.key1 是 Chat ID, channel.key2 是 Bot Token
      String tgBaseUrl = channel.url.length() > 0 ? channel.url : "https://api.telegram.org";
      if (tgBaseUrl.endsWith("/")) tgBaseUrl.remove(tgBaseUrl.length() - 1);

      String tgUrl = tgBaseUrl + "/bot" + channel.key2 + "/sendMessage";
      http.begin(tgUrl);
      http.addHeader("Content-Type", "application/json");

      String jsonData = "{";
      jsonData += "\"chat_id\":\"" + channel.key1 + "\",";
      String text = "📱短信通知\n发送者: " + senderEscaped + "\n接收卡号: " + devicePhoneEscaped + "\n内容: " + messageEscaped + "\n时间: " + timestampEscaped;
      jsonData += "\"text\":\"" + text + "\"";
      jsonData += "}";

      Serial.println("Telegram: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    case PUSH_TYPE_WORK_WEIXIN: {
      // 企业微信机器人
      String webhookUrl = channel.url;
      String jsonData = "{";

      // 企业微信消息体
      jsonData += "\"msgtype\":\"text\",";
      jsonData += "\"text\":{\"content\":\"";
      jsonData += "📱短信通知\\n发送者: " + senderEscaped + "\\n接收卡号: " + devicePhoneEscaped + "\\n内容: " + messageEscaped + "\\n时间: " + timestampEscaped;
      jsonData += "\"}}";

      http.begin(webhookUrl);
      http.addHeader("Content-Type", "application/json");
      Serial.println("企业微信: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }

    default:
      Serial.println("未知推送类型");
      return -1;
  }

  if (httpCode > 0) {
    Serial.printf("[%s] 响应码: %d\n", channelName.c_str(), httpCode);
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      String response = http.getString();
      Serial.println("响应: " + response);
    }
  } else {
    Serial.printf("[%s] HTTP请求失败: %s\n", channelName.c_str(), http.errorToString(httpCode).c_str());
  }
  http.end();
  return httpCode;
}

// 发送短信到所有启用的推送通道
void sendSMSToServer(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi未连接，跳过推送");
    return;
  }

  bool hasEnabledChannel = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      hasEnabledChannel = true;
      break;
    }
  }

  if (!hasEnabledChannel) {
    Serial.println("没有启用的推送通道");
    return;
  }

  Serial.println("\n=== 开始多通道推送 ===");
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      sendToChannel(config.pushChannels[i], sender, message, timestamp, devicePhoneNumber.c_str());
      delay(100); // 短暂延迟避免请求过快
    }
  }
  Serial.println("=== 多通道推送完成 ===\n");
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 处理最终的短信内容（管理员命令检查和转发）
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  Serial.println("=== 处理短信内容 ===");
  Serial.println("发送者: " + String(sender));
  Serial.println("时间戳: " + String(timestamp));
  Serial.println("内容: " + String(text));
  Serial.println("====================");

  // 检查是否在号码黑名单中
  if (isInNumberBlackList(sender)) {
    Serial.println("🚫 发送者在号码黑名单中，忽略该短信");
    return;
  }

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    Serial.println("收到管理员短信，检查命令...");
    String smsText = String(text);
    smsText.trim();

    // 检查是否为命令格式
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return;
    }
  }

  // 发送通知http（推送到所有启用的通道）
  sendSMSToServer(sender, text, timestamp);
  // 发送通知邮件
  String subject = ""; subject+="短信";subject+=sender;subject+=",";subject+=text;
  String body = ""; body+="来自：";body+=sender;body+="，时间：";body+=timestamp;body+="，内容：";body+=text;
  sendEmailNotification(subject.c_str(), body.c_str());
}

// 处理URC和PDU
void checkSerial1URC() {
  static enum { IDLE,
                WAIT_PDU } state = IDLE;

  String line = readSerialLine(Serial1);
  if (line.length() == 0) return;

  // 打印到调试串口
  Serial.println("Debug> " + line);

  if (state == IDLE) {
    // 检测到短信上报URC头
    if (line.startsWith("+CMT:")) {
      Serial.println("检测到+CMT，等待PDU数据...");
      state = WAIT_PDU;
    }
    // ========== SIM卡热插拔URC处理 ==========
    // ML307C 在SIM卡就绪时上报 "+CPIN: READY"，拔出时可能上报 "+CPIN: NOT READY" 或 "^SIMST:0"
    else if (line.indexOf("+CPIN:") >= 0) {
      if (line.indexOf("READY") >= 0 && line.indexOf("NOT") < 0) {
        // SIM就绪通知：如果当前未初始化，立即触发初始化（无需等待轮询周期）
        if (!simInitialized) {
          Serial.println("🔔 URC检测到SIM就绪，触发初始化...");
          simPresent = true;
          devicePhoneNumber = "未知号码";
          delay(500);
          initSIMDependent();
        }
      } else {
        // SIM未就绪/拔出
        if (simPresent) {
          Serial.println("⚠️ URC检测到SIM不可用，重置SIM状态");
          simPresent = false;
          simInitialized = false;
          devicePhoneNumber = "未知号码";
        }
      }
    }
    // 部分移远/ML30x模组使用 ^SIMST URC（0=移除，1=插入就绪）
    else if (line.startsWith("^SIMST:") || line.startsWith("+SIMCARD:")) {
      if (line.indexOf(":1") >= 0 || line.indexOf(": 1") >= 0) {
        if (!simInitialized) {
          Serial.println("🔔 URC检测到SIM插入(^SIMST/+SIMCARD)，触发初始化...");
          simPresent = true;
          devicePhoneNumber = "未知号码";
          delay(500);
          initSIMDependent();
        }
      } else if (line.indexOf(":0") >= 0 || line.indexOf(": 0") >= 0) {
        if (simPresent) {
          Serial.println("⚠️ URC检测到SIM拔出(^SIMST/+SIMCARD)，重置SIM状态");
          simPresent = false;
          simInitialized = false;
          devicePhoneNumber = "未知号码";
        }
      }
    }
  } else if (state == WAIT_PDU) {
    // 跳过空行
    if (line.length() == 0) {
      return;
    }

    // 如果是十六进制字符串，认为是PDU数据
    if (isHexString(line)) {
      Serial.println("收到PDU数据: " + line);
      Serial.println("PDU长度: " + String(line.length()) + " 字符");

      // 解析PDU
      if (!pdu.decodePDU(line.c_str())) {
        Serial.println("❌ PDU解析失败！");
      } else {
        Serial.println("✓ PDU解析成功");
        Serial.println("=== 短信内容 ===");
        Serial.println("发送者: " + String(pdu.getSender()));
        Serial.println("时间戳: " + String(pdu.getTimeStamp()));
        Serial.println("内容: " + String(pdu.getText()));

        // 获取长短信信息
        int* concatInfo = pdu.getConcatInfo();
        int refNumber = concatInfo[0];
        int partNumber = concatInfo[1];
        int totalParts = concatInfo[2];

        Serial.printf("长短信信息: 参考号=%d, 当前=%d, 总计=%d\n", refNumber, partNumber, totalParts);
        Serial.println("===============");

        // 判断是否为长短信
        if (totalParts > 1 && partNumber > 0) {
          // 这是长短信的一部分
          Serial.printf("📧 收到长短信分段 %d/%d\n", partNumber, totalParts);

          // 查找或创建缓存槽位
          int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);

          // 存储该分段（partNumber从1开始，数组从0开始）
          int partIndex = partNumber - 1;
          if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
            if (!concatBuffer[slot].parts[partIndex].valid) {
              concatBuffer[slot].parts[partIndex].valid = true;
              concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
              concatBuffer[slot].receivedParts++;

              // 如果是第一个收到的分段，保存时间戳
              if (concatBuffer[slot].receivedParts == 1) {
                concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
              }

              Serial.printf("  已缓存分段 %d，当前已收到 %d/%d\n",
                           partNumber,
                           concatBuffer[slot].receivedParts,
                           totalParts);
            } else {
              Serial.printf("  ⚠️ 分段 %d 已存在，跳过\n", partNumber);
            }
          }

          // 检查是否已收齐所有分段
          if (concatBuffer[slot].receivedParts >= totalParts) {
            Serial.println("✅ 长短信已收齐，开始合并转发");

            // 合并所有分段
            String fullText = assembleConcatSms(slot);

            // 处理完整短信
            processSmsContent(concatBuffer[slot].sender.c_str(),
                             fullText.c_str(),
                             concatBuffer[slot].timestamp.c_str());

            // 清空槽位
            clearConcatSlot(slot);
          }
        } else {
          // 普通短信，直接处理
          processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
        }
      }

      // 返回IDLE状态
      state = IDLE;
    }
    // 如果是其他内容（OK、ERROR等），也返回IDLE
    else {
      Serial.println("收到非PDU数据，返回IDLE状态");
      state = IDLE;
    }
  }
}

void blink_short(unsigned long gap_time = 500) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

// 检测网络注册状态（LTE/4G）
// CEREG状态: 1=已注册本地, 5=已注册漫游
bool waitCEREG() {
  Serial1.println("AT+CEREG?");
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 2000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      // +CEREG: <n>,<stat> 其中stat=1或5表示已注册
      if (resp.indexOf("+CEREG:") >= 0) {
        // 检查是否已注册（状态1或5）
        if (resp.indexOf(",1") >= 0 || resp.indexOf(",5") >= 0) return true;
        if (resp.indexOf(",0") >= 0 || resp.indexOf(",2") >= 0 ||
            resp.indexOf(",3") >= 0 || resp.indexOf(",4") >= 0) return false;
      }
    }
  }
  return false;
}

// ========== SIM卡状态检测 ==========
// 使用 AT+CPIN? 查询SIM卡是否就绪
// 返回 true: SIM卡存在且就绪; false: 未插入或未就绪
bool checkSIMPresent() {
  while (Serial1.available()) Serial1.read();  // 清空缓冲区
  Serial1.println("AT+CPIN?");
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 3000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
    }
    if (resp.indexOf("+CPIN:") >= 0 || resp.indexOf("ERROR") >= 0) {
      delay(50);
      while (Serial1.available()) resp += (char)Serial1.read();
      break;
    }
  }
  Serial.println("AT+CPIN? 响应: " + resp);
  // +CPIN: READY 表示SIM卡就绪
  if (resp.indexOf("READY") >= 0) return true;
  // +CME ERROR: 10 / +CME ERROR: 11 表示无SIM卡或SIM故障
  return false;
}

// ========== SIM卡相关初始化 ==========
// 将所有依赖SIM卡的初始化逻辑集中在此函数中
// 返回 true 表示初始化成功，false 表示中途失败（如SIM被拔出）
bool initSIMDependent() {
  Serial.println("========== 开始SIM卡初始化 ==========");

  // 禁用数据连接，防止流量消耗
  int retry = 0;
  while (!sendATandWaitOK("AT+CGACT=0,1", 5000)) {
    Serial.println("设置CGACT失败，重试...");
    blink_short();
    if (++retry >= 10) {
      Serial.println("⚠️ AT+CGACT=0,1 多次失败，跳过（可能无SIM）");
      // 不强制中断，继续尝试后续步骤
      break;
    }
  }
  if (retry < 10) Serial.println("已禁用数据连接(AT+CGACT=0,1)");

  // 设置短信自动上报
  retry = 0;
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) {
    Serial.println("设置CNMI失败，重试...");
    blink_short();
    if (++retry >= 15) {
      Serial.println("❌ AT+CNMI 设置失败，SIM卡初始化中止");
      return false;
    }
  }
  Serial.println("CNMI参数设置完成");

  // 配置PDU模式
  retry = 0;
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) {
    Serial.println("设置PDU模式失败，重试...");
    blink_short();
    if (++retry >= 15) {
      Serial.println("❌ AT+CMGF=0 设置失败，SIM卡初始化中止");
      return false;
    }
  }
  Serial.println("PDU模式设置完成");

  // 等待网络注册（LTE/4G），最多等待约60秒
  retry = 0;
  while (!waitCEREG()) {
    Serial.println("等待网络注册...");
    blink_short();
    if (++retry >= 40) {
      Serial.println("❌ 网络注册超时，SIM卡初始化中止");
      return false;
    }
    // 每次等待前重新检查SIM是否仍然在位
    if (!checkSIMPresent()) {
      Serial.println("⚠️ 等待网络注册期间SIM卡被拔出，中止初始化");
      return false;
    }
  }
  Serial.println("网络已注册");

  // 查询本机号码（SIM卡号）
  {
    String cnumResp = sendATCommand("AT+CNUM", 2000);
    if (cnumResp.indexOf("+CNUM:") >= 0) {
      int idx = cnumResp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = cnumResp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          devicePhoneNumber = cnumResp.substring(idx + 2, endIdx);
        }
      }
    }
    Serial.println("本机号码: " + devicePhoneNumber);
  }

  simInitialized = true;
  Serial.println("========== SIM卡初始化完成 ==========");

  // 如果配置有效，发送启动/插卡通知
  if (configValid) {
    String subject = "短信转发器：SIM卡已就绪";
    String body = "SIM卡初始化成功\n本机号码: " + devicePhoneNumber + "\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }

  return true;
}

void setup() {
  //  指示灯
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // USB 串口日志
  Serial.begin(115200);
  delay(1500);  // 等 USB CDC 稳定

  // 模组串口（UART）
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);

  // 模组从“干净状态”启动（EN 断电重启 + 清串口噪声）
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();

  // 初始化长短信缓存
  initConcatBuffer();

  // 加载配置
  loadConfig();
  configValid = isConfigValid();


  // ========== 先初始化模组 ==========
  while (!sendATandWaitOK("AT", 1000)) {
    Serial.println("AT未响应，重试...");
    blink_short();
  }
  Serial.println("模组AT响应正常");

  // 检查SIM卡是否插入，决定是否执行SIM相关初始化
  simPresent = checkSIMPresent();
  if (simPresent) {
    Serial.println("✅ 检测到SIM卡，开始SIM相关初始化...");
    initSIMDependent();
  } else {
    Serial.println("⚠️ 未检测到SIM卡，跳过SIM初始化，系统将继续启动");
    Serial.println("   SIM卡插入后将自动完成初始化");
  }
  // ========== 模组初始化完成（SIM部分已异步化）==========

  // 连接WiFi（优先使用NVS中保存的SSID，支持隐藏SSID，失败时开AP热点）
  WiFi.mode(WIFI_STA);
  String wifiSSID = config.wifiSSID.length() > 0 ? config.wifiSSID : String(WIFI_SSID);
  String wifiPass = config.wifiPass.length() > 0 ? config.wifiPass : String(WIFI_PASS);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str(), 0, nullptr, true);
  Serial.print("正在连接WiFi: ");
  Serial.println(wifiSSID);

  unsigned long startWifi = millis();
  bool wifiConnected = false;
  while (millis() - startWifi < 30000) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      break;
    }
    blink_short();
    Serial.print(".");
  }
  Serial.println();

  if (wifiConnected) {
    Serial.println("WiFi连接成功");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi连接超时，启动AP模式...");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    String apName = "SMS-Forwarder-AP";
    WiFi.softAP(apName.c_str()); // 无密码开放热点
    Serial.print("AP已启动: "); Serial.println(apName);
    Serial.print("AP IP地址: "); Serial.println(WiFi.softAPIP());
  }

  // NTP时间同步（获取UTC时间）
  Serial.println("正在同步NTP时间...");
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 100) {
    delay(100);
    ntpRetry++;
  }
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    Serial.println("NTP时间同步成功");
    time_t now = time(nullptr);
    Serial.print("当前UTC时间戳: ");
    Serial.println(now);
  } else {
    Serial.println("NTP时间同步失败，将使用设备时间");
  }

  // 启动HTTP服务器
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleToolsPage);
  server.on("/sms", handleToolsPage);  // 兼容旧链接
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/query", handleQuery);
  server.on("/flight", handleFlightMode);
  server.on("/at", handleATCommand);
  server.on("/test_push", HTTP_POST, handleTestPush);
  server.on("/api-docs", handleApiDocs);
  server.begin();
  Serial.println("HTTP服务器已启动");

  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);

  // 如果配置有效且SIM未插入，发送启动通知（SIM存在时通知由initSIMDependent发出）
  if (configValid && !simInitialized) {
    Serial.println("配置有效，发送启动通知（无SIM卡）...");
    String subject = "短信转发器已启动（未检测到SIM卡）";
    String body = "设备已启动，但未检测到SIM卡\n请插入SIM卡以启用短信转发功能\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
}

void loop() {
  // 处理HTTP请求
  server.handleClient();

  // 如果配置无效，每秒打印一次IP地址
  if (!configValid) {
    if (millis() - lastPrintTime >= 1000) {
      lastPrintTime = millis();
      Serial.println("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数");
    }
  }

  // ========== SIM卡热插拔检测 ==========
  // 每5秒轮询一次SIM卡状态，检测插入/拔出
  static unsigned long lastSimCheck = 0;
  if (millis() - lastSimCheck >= 5000) {
    lastSimCheck = millis();
    bool nowPresent = checkSIMPresent();

    if (nowPresent && !simPresent) {
      // SIM卡从无到有：插入事件
      Serial.println("🔔 检测到SIM卡插入！开始初始化...");
      simPresent = true;
      simInitialized = false;
      devicePhoneNumber = "未知号码";
      delay(500);  // 等待SIM稳定
      initSIMDependent();

    } else if (!nowPresent && simPresent) {
      // SIM卡从有到无：拔出事件
      Serial.println("⚠️ 检测到SIM卡被拔出！");
      simPresent = false;
      simInitialized = false;
      devicePhoneNumber = "未知号码";
      // 可选：发送拔卡通知
      if (configValid) {
        String simRemovedBody = "SIM卡已被拔出，短信转发功能暂停\n设备地址: " + getDeviceUrl();
        sendEmailNotification("短信转发器：SIM卡已拔出", simRemovedBody.c_str());
      }
    }
  }

  // 检查长短信超时
  checkConcatTimeout();

  // 本地透传
  if (Serial.available()) Serial1.write(Serial.read());
  // 检查URC和解析
  checkSerial1URC();
}
#endif  // end of old monolithic code


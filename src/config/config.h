#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// 推送通道类型
enum PushType {
  PUSH_TYPE_NONE         = 0,
  PUSH_TYPE_POST_JSON    = 1,
  PUSH_TYPE_BARK         = 2,
  PUSH_TYPE_GET          = 3,
  PUSH_TYPE_DINGTALK     = 4,
  PUSH_TYPE_PUSHPLUS     = 5,
  PUSH_TYPE_SERVERCHAN   = 6,
  PUSH_TYPE_CUSTOM       = 7,
  PUSH_TYPE_FEISHU       = 8,
  PUSH_TYPE_GOTIFY       = 9,
  PUSH_TYPE_TELEGRAM     = 10,
  PUSH_TYPE_WECHAT_WORK  = 11,
  PUSH_TYPE_SMS          = 12
};

// 推送策略
enum PushStrategy {
  PUSH_STRATEGY_BROADCAST = 0,
  PUSH_STRATEGY_FAILOVER  = 1
};

// 消息类型
enum MsgType {
  MSG_TYPE_SMS  = 0,
  MSG_TYPE_CALL = 1,
  MSG_TYPE_SIM  = 2
};

constexpr int MAX_PUSH_CHANNELS    = 10;
constexpr int MAX_WIFI_ENTRIES      = 5;
constexpr int MAX_BLACKLIST_ENTRIES = 20;

#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

// 应用名称常量（用宏支持字符串字面量拼接）
#define APP_NAME    "SMS-Forwarder"
#define APP_AUTHOR  "keroming"
constexpr char kAppName[] = APP_NAME;        // 基础名称
constexpr char kApSsid[]  = APP_NAME "-AP";  // WiFi AP 模式 SSID

struct PushChannel {
  bool     enabled;
  PushType type;
  String   name;
  String   url;
  String   key1;
  String   key2;
  String   customBody;
  bool     retryOnFail;
};

struct WifiEntry {
  String ssid;
  String password;
};

struct Config {
  String     adminPhone;
  String     webUser;
  String     webPass;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];
  int          pushCount;
  bool         simNotifyEnabled;
  bool         dataTraffic;
  WifiEntry    wifiList[MAX_WIFI_ENTRIES];
  int          wifiCount;
  String       blacklist[MAX_BLACKLIST_ENTRIES];
  int          blacklistCount;
  PushStrategy pushStrategy;
  String       remark;   // 设备备注，最大 64 字符
};

struct RebootSchedule {
  bool    enabled;
  uint8_t mode;       // 0 = daily, 1 = interval
  uint8_t hour;
  uint8_t minute;
  uint16_t intervalH;
};

extern Config config;
extern RebootSchedule rebootSchedule;

void loadConfig();
void saveConfig();
void resetConfig();
void loadRebootSchedule(RebootSchedule& sched);
void saveRebootSchedule(const RebootSchedule& sched);
void rebootTick();
bool isPushChannelValid(const PushChannel& ch);
bool isConfigValid();

// JSON 序列化 / 反序列化（供导出和导入控制器共用）
// 新增或修改 Config / RebootSchedule 字段时只需更新这两个函数即可。
void configToJson(JsonDocument& doc);
void configFromJson(JsonDocument& doc);

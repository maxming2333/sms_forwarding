#include "blufi_handler.h"
#include "config/config.h"
#include "wifi/wifi_manager.h"
#include "logger.h"
#include <Arduino.h>
#include <esp_blufi_api.h>
#include <esp_bt.h>
#include <esp_gap_ble_api.h>
#include <esp_bt_main.h>

// ---------- internal state ----------

static bool   s_blufiInitialized = false;
static String s_pendingSsid      = "";
static String s_pendingPass      = "";

// ---------- helpers ----------

// WiFi 优先级插入：将新 SSID 插到 wifiList 首位
static void insertWifiFirst(const String& ssid, const String& pass) {
  // 查找重复 SSID
  int dupIdx = -1;
  for (int i = 0; i < config.wifiCount; i++) {
    if (config.wifiList[i].ssid == ssid) {
      dupIdx = i;
      break;
    }
  }
  if (dupIdx >= 0) {
    // 移除重复条目
    for (int i = dupIdx; i < config.wifiCount - 1; i++) {
      config.wifiList[i] = config.wifiList[i + 1];
    }
    config.wifiCount--;
  }
  // 前移所有现有条目
  int newCount = min(config.wifiCount + 1, MAX_WIFI_ENTRIES);
  for (int i = newCount - 1; i > 0; i--) {
    config.wifiList[i] = config.wifiList[i - 1];
  }
  config.wifiList[0].ssid     = ssid;
  config.wifiList[0].password = pass;
  config.wifiCount = newCount;
  saveConfig();
  LOG("BluFi", "WiFi已插入首位: %s，列表共 %d 条", ssid.c_str(), config.wifiCount);
}

// ---------- BluFi 事件回调 ----------

static void blufiEventCallback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
  switch (event) {
    case ESP_BLUFI_EVENT_RECV_STA_SSID: {
      if (param->sta_ssid.ssid && param->sta_ssid.ssid_len > 0) {
        s_pendingSsid = String((const char*)param->sta_ssid.ssid).substring(0, param->sta_ssid.ssid_len);
        LOG("BluFi", "收到 SSID: %s", s_pendingSsid.c_str());
      }
      break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
      if (param->sta_passwd.passwd && param->sta_passwd.passwd_len > 0) {
        s_pendingPass = String((const char*)param->sta_passwd.passwd).substring(0, param->sta_passwd.passwd_len);
        LOG("BluFi", "收到密码（已隐藏）");
      }
      break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE: {
      LOG("BluFi", "BLE 断开");
      break;
    }
    default:
      break;
  }

  // 当 SSID 和密码均已收到时写入配置并重启
  if (s_pendingSsid.length() > 0
      && (event == ESP_BLUFI_EVENT_RECV_STA_PASSWD || event == ESP_BLUFI_EVENT_RECV_STA_SSID)) {
    esp_blufi_extra_info_t info;
    memset(&info, 0, sizeof(info));
    esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
    insertWifiFirst(s_pendingSsid, s_pendingPass);
    s_pendingSsid = "";
    s_pendingPass = "";
    delay(500);
    ESP.restart();
  }
}

// ---------- 公开接口 ----------

void blufiInit() {
  if (s_blufiInitialized) {
    return;
  }

  // 初始化 BT 控制器（BLE 模式）
  esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_bt_controller_init(&btCfg);
  if (err != ESP_OK) {
    LOG("BluFi", "BT 控制器初始化失败: %d", (int)err);
    return;
  }
  err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (err != ESP_OK) {
    LOG("BluFi", "BT 控制器启用失败: %d", (int)err);
    return;
  }
  err = esp_bluedroid_init();
  if (err != ESP_OK) {
    LOG("BluFi", "Bluedroid 初始化失败: %d", (int)err);
    return;
  }
  err = esp_bluedroid_enable();
  if (err != ESP_OK) {
    LOG("BluFi", "Bluedroid 启用失败: %d", (int)err);
    return;
  }

  // 从公共接口获取设备名称并设置 BLE 广播名
  String name = getDeviceName();
  esp_ble_gap_set_device_name(name.c_str());

  // 注册 BluFi 回调
  err = esp_blufi_register_callbacks((esp_blufi_callbacks_t*)blufiEventCallback);
  if (err != ESP_OK) {
    LOG("BluFi", "回调注册失败: %d", (int)err);
    return;
  }

  // AES-128 加密（已评估，T010/T011）：
  // 当前 espressif32 Arduino framework 内置的 ESP-IDF 版本中，BluFi AES-128 加密
  // 需通过 esp_blufi_callbacks_t.encrypt_func / decrypt_func 函数指针实现，
  // 不存在 esp_blufi_security_init() 单一入口。
  // 此功能需实现完整的 AES-CBC + DH 密钥协商，范围超出本特性；决策：暂不启用加密。

  err = esp_blufi_profile_init();
  if (err != ESP_OK) {
    LOG("BluFi", "BluFi profile 初始化失败: %d", (int)err);
    return;
  }

  s_blufiInitialized = true;
  LOG("BluFi", "BluFi 已启动，设备名: %s", name.c_str());
}

void blufiDeinit() {
  if (!s_blufiInitialized) {
    return;
  }
  esp_blufi_profile_deinit();
  s_blufiInitialized = false;
  LOG("BluFi", "BluFi 已关闭");
}

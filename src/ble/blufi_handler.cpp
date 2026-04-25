#include "blufi_handler.h"
#include "config/config.h"
#include "wifi/wifi_manager.h"
#include "logger.h"
#include <Arduino.h>
#include <esp_blufi_api.h>
extern "C" {
#include <esp_blufi.h>  // esp_blufi_adv_start/stop；该头文件缺少 extern "C" 保护，需手动包裹
}
#include <esp_bt.h>
#include <esp_gap_ble_api.h>
#include <esp_bt_main.h>

// ---------- internal state ----------

static bool   s_blufiInitialized = false;
static String s_deviceName       = "";
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

// ---------- 广播参数 ----------

static esp_ble_adv_params_t s_blufiAdvParams = {
  .adv_int_min       = 0x100,
  .adv_int_max       = 0x100,
  .adv_type          = ADV_TYPE_IND,
  .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
  .peer_addr         = {0},
  .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
  .channel_map       = ADV_CHNL_ALL,
  .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// 使用原始广播数据，将设备名字节直接写入广播包。
// 不能依赖 esp_blufi_adv_start() 内部的 include_name=true 机制：
// esp_blufi_profile_init() 内部会将设备名重置为 "BLUFI_DEVICE"（BTC 异步命令），
// INIT_FINISH 回调触发时重置已完成；再在 INIT_FINISH 里调 set_device_name + adv_start
// 仍有竞态（config_adv_data 可能先于 set_device_name 在 BTC 队列里处理）。
// esp_ble_gap_config_adv_data_raw() 直接控制字节，不读取栈内部名称，彻底绕开竞态。
static void startBlufiAdvertising() {
  uint8_t nameLen = (uint8_t)s_deviceName.length();
  if (nameLen > 20) nameLen = 20;  // 31 字节上限：3(flags)+4(UUID)+2(name header)+nameLen

  uint8_t adv[31];
  uint8_t idx = 0;

  // Flags: LE General Discoverable, BR/EDR Not Supported
  adv[idx++] = 0x02; adv[idx++] = 0x01; adv[idx++] = 0x06;
  // 16-bit UUID: 0xFFFF (BluFi 服务，EspBluFi App 靠此识别设备类型)
  adv[idx++] = 0x03; adv[idx++] = 0x02; adv[idx++] = 0xFF; adv[idx++] = 0xFF;
  // Complete Local Name
  adv[idx++] = nameLen + 1; adv[idx++] = 0x09;
  memcpy(adv + idx, s_deviceName.c_str(), nameLen); idx += nameLen;

  esp_ble_gap_config_adv_data_raw(adv, idx);
}

// GAP 回调包装：转发给 BluFi 内部处理器，并在原始广播数据写入完成后启动广播
static void gapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  esp_blufi_gap_event_handler(event, param);
  if (event == ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT) {
    esp_ble_gap_start_advertising(&s_blufiAdvParams);
    LOG("BluFi", "BLE 广播已启动");
  }
}

// ---------- Scan Response（包含制造商数据 + 设备名）----------
// Scan Response 是独立于 ADV 包的第二个广播包（最多 31 字节），
// 由扫描方主动请求，不影响 BluFi 服务 UUID 广播。
// Manufacturer Data 格式：[len][0xFF][company_id_low][company_id_high][data...]
// 使用 Espressif 公司 ID 0x02E5（已由 Bluetooth SIG 分配）。
static void configureScanResponse() {
  // Company ID: Espressif Systems (0x02E5), little-endian
  const uint8_t companyId[2] = { 0xE5, 0x02 };
  // 自定义 payload，最多 27 字节（31 - 2(header) - 2(company_id) = 27）
  // Manufacturer Data payload 直接复用 config.h 中的 kAppName
  constexpr uint8_t mfgDataLen = sizeof(kAppName) - 1;  // 排除 null 终止符

  uint8_t rsp[31];
  uint8_t idx = 0;

  // Manufacturer Specific Data
  uint8_t mfgFieldLen = 1 + 2 + mfgDataLen;  // type(1) + companyId(2) + payload
  rsp[idx++] = mfgFieldLen;
  rsp[idx++] = 0xFF;  // AD type: Manufacturer Specific
  rsp[idx++] = companyId[0];
  rsp[idx++] = companyId[1];
  memcpy(rsp + idx, kAppName, mfgDataLen); idx += mfgDataLen;

  // Complete Local Name（再放入 Scan Rsp，扫描时双重保障能看到名字）
  uint8_t nameLen = (uint8_t)s_deviceName.length();
  if (nameLen > 0 && idx + 2 + nameLen <= 31) {
    rsp[idx++] = nameLen + 1;
    rsp[idx++] = 0x09;  // AD type: Complete Local Name
    memcpy(rsp + idx, s_deviceName.c_str(), nameLen); idx += nameLen;
  }

  esp_ble_gap_config_scan_rsp_data_raw(rsp, idx);
}

// ---------- BluFi 事件回调 ----------

static void blufiEventCallback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
  switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH: {
      // esp_blufi_profile_init() 内部将设备名重置为 "BLUFI_DEVICE"；
      // 在此重新设置，使 GATT Device Name 特征值显示正确名称（异步，不影响广播包）
      esp_ble_gap_set_device_name(s_deviceName.c_str());
      // 使用原始广播数据：直接将设备名字节嵌入广播包，彻底绕过 set_device_name 的异步竞态。
      // gapEventHandler 收到 ADV_DATA_RAW_SET_COMPLETE_EVT 后调用 esp_ble_gap_start_advertising()。
      startBlufiAdvertising();
      // 配置 Scan Response：Manufacturer Data（Espressif ID + 自定义载荷）和设备名
      configureScanResponse();
      LOG("BluFi", "BLE 广播配置已提交");
      break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_SSID: {
      if (param->sta_ssid.ssid && param->sta_ssid.ssid_len > 0) {
        // ssid 不含 null 终止符，必须用带长度的构造函数
        s_pendingSsid = String((const char*)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        LOG("BluFi", "收到 SSID: %s", s_pendingSsid.c_str());
      }
      break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
      if (param->sta_passwd.passwd && param->sta_passwd.passwd_len > 0) {
        // passwd 同样不含 null 终止符
        s_pendingPass = String((const char*)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
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
  // 必须等 PASSWD 事件到达后才触发，避免在仅收到 SSID 时以空密码重启
  if (event == ESP_BLUFI_EVENT_RECV_STA_PASSWD && s_pendingSsid.length() > 0) {
    esp_blufi_extra_info_t info{};  // 零初始化，C++17 值初始化替代 memset
    esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
    insertWifiFirst(s_pendingSsid, s_pendingPass);
    s_pendingSsid.clear();
    s_pendingPass.clear();
    blufiDeinit();  // 释放 BLE 资源，避免重启后与 WiFi 射频竞争
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

  // 从公共接口获取设备名称，存入静态变量供 INIT_FINISH 回调使用
  // 不在此处调用 esp_ble_gap_set_device_name()：
  // esp_blufi_profile_init() 内部会将名称重置为 "BLUFI_DEVICE"，在此设置无效。
  // 广播包中的名称通过 startBlufiAdvertising() 直接写字节，GATT 名称在 INIT_FINISH 里设置。
  s_deviceName = getDeviceName();

  // 注册 GAP 回调包装器（gapEventHandler）：
  // 转发所有事件给 esp_blufi_gap_event_handler，
  // 并在 ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT 时调用 esp_ble_gap_start_advertising()
  err = esp_ble_gap_register_callback(gapEventHandler);
  if (err != ESP_OK) {
    LOG("BluFi", "GAP 回调注册失败: %d", (int)err);
    return;
  }

  // 注册 BluFi 回调
  static esp_blufi_callbacks_t blufiCallbacks = {
    .event_cb              = blufiEventCallback,
    .negotiate_data_handler = nullptr,
    .encrypt_func          = nullptr,
    .decrypt_func          = nullptr,
    .checksum_func         = nullptr,
  };
  err = esp_blufi_register_callbacks(&blufiCallbacks);
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
  LOG("BluFi", "BluFi 已启动，设备名: %s", s_deviceName.c_str());
}

void blufiDeinit() {
  if (!s_blufiInitialized) {
    return;
  }
  // 必须按初始化的逆序完整释放，否则 BLE 射频不会关闭，影响 WiFi 共存
  esp_blufi_profile_deinit();
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  s_blufiInitialized = false;
  LOG("BluFi", "BluFi 已关闭");
}

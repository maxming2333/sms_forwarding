#include "blufi_handler.h"
#include "blufi_security.h"
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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------- internal state ----------

static bool   s_blufiInitialized  = false;
static bool   s_provisioningDone  = false;  // REQ_CONNECT_TO_AP 成功后置位，BLE_DISCONNECT 时操作
static String s_deviceName        = "";
static String s_pendingSsid       = "";
static String s_pendingPass       = "";

// 异步延迟重启在 BLE_DISCONNECT 里通过 xTaskCreate 内联 lambda 实现

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
    case ESP_BLUFI_EVENT_BLE_CONNECT: {
      // 停止广播，重置 DH 安全状态，等待配网
      esp_ble_gap_stop_advertising();
      blufi_security_reset();
      s_provisioningDone = false;
      LOG("BluFi", "BLE 已连接");
      break;
    }
    case ESP_BLUFI_EVENT_BLE_DISCONNECT: {
      if (s_provisioningDone) {
        // App 收到成功回包后主动断开，此时整个配网流程真正结束，再异步重启
        LOG("BluFi", "BLE 已断开（配网完成），即将重启");
        xTaskCreate([](void*) {
            taskYIELD();  // 让 BTC 任务先跑完
            vTaskDelay(pdMS_TO_TICKS(500));
            blufiDeinit();
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP.restart();
        }, "blufi_rst", 4096, nullptr, 5, nullptr);
      } else {
        // 普通断开（用户中途退出、还未配网），清除待定数据并重新广播
        s_pendingSsid.clear();
        s_pendingPass.clear();
        startBlufiAdvertising();
        LOG("BluFi", "BLE 已断开，重新开始广播");
      }
      break;
    }
    case ESP_BLUFI_EVENT_INIT_FINISH: {
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
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
      // App 发送 SSID + PASSWORD 后，会明确发出此命令说明希望设备去连 WiFi。
      // 此时发 conn_report，App 收到后会主动断开 BLE，在 BLE_DISCONNECT 里再重启。
      if (s_pendingSsid.length() > 0) {
        insertWifiFirst(s_pendingSsid, s_pendingPass);
        s_provisioningDone = true;
        esp_blufi_extra_info_t info{};
        // 填入目标 SSID 供 App 展示；BSSID 尚未连接无法填写，保持 bssid_set=false
        // info.sta_ssid 借用 s_pendingSsid 内部指针，必须在 send_report 之后才能 clear()
        info.sta_ssid     = (uint8_t*)s_pendingSsid.c_str();
        info.sta_ssid_len = (int)s_pendingSsid.length();
        esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
        s_pendingSsid.clear();
        s_pendingPass.clear();
        LOG("BluFi", "配置已保存，回包成功，等待 App 断开 BLE");
      }
      break;
    }
    default:
      break;
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
    .event_cb               = blufiEventCallback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func           = blufi_aes_encrypt,
    .decrypt_func           = blufi_aes_decrypt,
    .checksum_func          = blufi_crc_checksum,
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

#pragma once
#include <Arduino.h>

// OTA 状态机
enum class OtaState : uint8_t {
  IDLE        = 0,    // 空闲
  CHECKING    = 1,    // 正在查询 GitHub Release 最新版本
  DOWNLOADING = 2,    // 正在下载固件
  WRITING     = 3,    // 正在写入 OTA 分区
  SUCCESS     = 4,    // 成功（等待重启）
  FAILED      = 5,    // 失败（message 携带原因）
  FLASHING_FS = 6,    // 正在烧录 LittleFS（替换 HTML 资源）
};

// 给前端的状态快照（JSON 序列化后通过 /api/ota/status 返回）
struct OtaStatusPayload {
  OtaState state          = OtaState::IDLE;
  uint8_t  progress       = 0;     // 0~100
  String   message        = "";    // 失败原因或当前阶段说明
  String   currentVersion = "";    // 当前固件版本
  String   latestVersion  = "";    // 远端最新版本（仅 CHECKING 完成后）
};

// GitHub Release 基础 URL（拼接 tag 得到具体下载地址）
static const char* const OTA_RELEASES_BASE_URL = "https://github.com/maxming2333/esp32-sms-forwarding/releases";
// OTA 后台任务栈/优先级：
//   - 12 KiB 栈：HTTPS 下载 + mbedTLS 握手栈占用较高，实测低于此值会爆栈
//   - 优先级 1：低于 SIM Reader（3），高于 idle，避免抢占短信处理
static constexpr uint32_t    OTA_TASK_STACK_SIZE  = 12288;
static constexpr UBaseType_t OTA_TASK_PRIORITY    = 1;
static constexpr int         OTA_HTTP_TIMEOUT_MS  = 15000;
constexpr unsigned long      OTA_CHECK_DEBOUNCE_MS = 30000;  // 同一会话内至多每 30s 检查一次

// Ota：固件 OTA + LittleFS 烧录入口。所有耗时操作都在独立 FreeRTOS 任务执行，
// 不阻塞 HTTP 请求。注意：本类内部使用**栈上**的 WiFiClientSecure，与 Push 共享禁用。
class Ota {
public:
  // 当前运行中固件版本（由 esp_app_desc 提供，CI 注入）
  static String           version();

  // 初始化（注册回调、清理上次状态等）
  static void             init();

  // 当前 OTA 状态快照（线程安全）
  static OtaStatusPayload status();

  // 异步：启动版本检查（去 GitHub Release 查询最新 tag）
  static void             startVersionCheck();

  // 异步：启动 OTA 升级到指定 tag（如 "v1.2.3"）。返回 false 表示已在进行或参数无效。
  static bool             startOnlineUpgrade(const String& targetTag);

  // 处理本地上传的固件分片（HTTP /api/ota/upload）
  static bool             handleUploadChunk(uint8_t* data, size_t len, size_t index, bool final_);

  // 处理本地上传的 LittleFS 镜像分片（HTTP /api/ota/lfs）
  static bool             handleLfsUploadChunk(uint8_t* data, size_t len, size_t index,
                                               size_t totalSize, bool final_);
};

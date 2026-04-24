#pragma once
#include <Arduino.h>

// OTA 升级状态枚举
enum class OtaState : uint8_t {
    IDLE        = 0,  // 空闲，无升级任务
    CHECKING    = 1,  // 正在向 GitHub 查询最新版本
    DOWNLOADING = 2,  // 正在下载固件（在线升级）
    WRITING     = 3,  // 正在写入 Flash（手动上传，逐块写入中）
    SUCCESS     = 4,  // 升级写入完成，设备将在 2s 后自动重启
    FAILED      = 5,  // 升级失败（写入错误、空间不足、传输中断）
    FLASHING_FS = 6,  // 正在写入 LittleFS 分区（在线升级第二阶段 / 手动上传 FS）
};

// OTA 状态快照（仅用于 HTTP 响应序列化，不持久化）
struct OtaStatusPayload {
    OtaState state          = OtaState::IDLE;
    uint8_t  progress       = 0;      // 0–100 百分比
    String   message        = "";     // 人类可读状态/错误信息
    String   currentVersion = "";     // 当前运行固件版本
    String   latestVersion  = "";     // 版本检查后的最新版本号（检查前为空串）
};

// 在线升级地址常量（修改此处即可变更升级源）
static const char* const OTA_RELEASES_BASE_URL = "https://github.com/maxming2333/esp32-sms-forwarding/releases";

// 版本检查地址（latest 重定向）
static const char* const OTA_LATEST_URL = "https://github.com/maxming2333/esp32-sms-forwarding/releases/latest";

// FreeRTOS 任务栈大小（字节）
// HTTPS 下载 + OTA 写入涉及 TLS 握手及较深的调用栈，适当增大以防溢出
static constexpr uint32_t OTA_TASK_STACK_SIZE = 12288;

// FreeRTOS 任务优先级
static constexpr UBaseType_t OTA_TASK_PRIORITY = 1;

// HTTPS 请求超时（ms）
static constexpr int OTA_HTTP_TIMEOUT_MS = 15000;

// 初始化：读取当前固件版本，重置所有 OTA 状态
void otaInit();

// 获取当前 OTA 状态快照（供 HTTP 控制器使用）
OtaStatusPayload otaGetStatus();

// 启动后台版本检查任务（非阻塞，仅查询最新版本号，不下载固件）
// 完成后状态回到 IDLE，g_latestVer 被填充
void otaStartVersionCheck();

// 启动后台在线升级任务（非阻塞，直接下载并刷写固件）
// 返回 false 表示当前已有升级在进行中
bool otaStartOnlineUpgrade();

// 手动上传：处理单个数据块（由 ESPAsyncWebServer upload 回调逐块调用）
// index=0 表示第一块（需调用 esp_ota_begin）；final=true 表示最后一块
// 返回 false 表示写入失败
bool otaHandleUploadChunk(uint8_t* data, size_t len, size_t index, bool final);

// 手动上传 LittleFS：处理单个数据块（由 ESPAsyncWebServer upload 回调逐块调用）
// index=0 表示第一块（先卸载 LittleFS、擦除分区）；final=true 表示最后一块
// totalSize: Content-Length（0 表示未知）
// 返回 false 表示写入失败
bool otaHandleLfsUploadChunk(uint8_t* data, size_t len, size_t index, size_t totalSize, bool final);

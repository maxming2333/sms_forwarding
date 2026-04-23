#include "ota_manager.h"
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "http/http_util.h"
#include "logger.h"

// ── 内部静态状态 ─────────────────────────────────────────────────
static volatile OtaState       g_state     = OtaState::IDLE;
static volatile uint8_t        g_progress  = 0;
static String                  g_message   = "";
static String                  g_currentVer = "";
static String                  g_latestVer  = "";
static volatile bool           g_inProgress = false;

static esp_ota_handle_t        g_otaHandle  = 0;
static const esp_partition_t*  g_otaPart    = nullptr;
static TaskHandle_t            g_taskHandle = nullptr;

// ── otaInit ──────────────────────────────────────────────────────
void otaInit() {
    // 直接使用构建宏，避免 --allow-multiple-definition 下 esp_app_desc
    // 被 libapp_update.a（arduino-lib-builder）的同名符号覆盖的问题。
    g_currentVer = String(APP_VERSION);
    g_state      = OtaState::IDLE;
    g_progress   = 0;
    g_message    = "";
    g_latestVer  = "";
    g_inProgress = false;
    LOG("OTA", "初始化完成，版本: %s | 编译: %s %s",
        g_currentVer.c_str(), APP_BUILD_DATE, APP_BUILD_TIME);
}

// ── otaGetStatus ─────────────────────────────────────────────────
OtaStatusPayload otaGetStatus() {
    OtaStatusPayload p;
    p.state          = g_state;
    p.progress       = g_progress;
    p.message        = g_message;
    p.currentVersion = g_currentVer;
    p.latestVersion  = g_latestVer;
    return p;
}

// ── checkVersionTask — 仅查询最新版本，不下载固件 ─────────────────
static void checkVersionTask(void* /*param*/) {
    g_state    = OtaState::CHECKING;
    g_progress = 0;
    g_message  = "正在查询最新版本...";
    LOG("OTA", "版本检查: %s", OTA_LATEST_URL);

    HTTPClient verHttp;
    httpClientBegin(verHttp, OTA_LATEST_URL);
    verHttp.setTimeout(OTA_HTTP_TIMEOUT_MS);
    verHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    int verCode = verHttp.GET();
    String latestTag = "";
    if (verCode == 301 || verCode == 302) {
        String location = verHttp.getLocation();
        int slash = location.lastIndexOf('/');
        if (slash >= 0 && slash < (int)location.length() - 1) {
            latestTag = location.substring(slash + 1);
        }
        LOG("OTA", "最新版本: %s", latestTag.c_str());
    } else {
        LOG("OTA", "版本检查响应码: %d（期望 301/302）", verCode);
    }
    verHttp.end();

    g_latestVer  = latestTag;
    g_inProgress = false;
    g_state      = OtaState::IDLE;
    g_message    = "";
    vTaskDelete(nullptr);
}
// ── otaTaskAbort — 失败后延时清理并终止任务（仅限 FreeRTOS 任务内调用）──
[[noreturn]] static void otaTaskAbort(const String& userMsg) {
    g_state      = OtaState::FAILED;
    g_message    = userMsg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    g_inProgress = false;
    g_state      = OtaState::IDLE;
    vTaskDelete(nullptr);
    for (;;);
}
// ── onlineUpgradeTask — 后台 FreeRTOS 任务 ───────────────────────
static void onlineUpgradeTask(void* /*param*/) {
    // --- 阶段1: 版本检查 ---
    g_state    = OtaState::CHECKING;
    g_progress = 0;
    g_message  = "正在查询最新版本...";
    LOG("OTA", "开始版本检查: %s", OTA_LATEST_URL);

    HTTPClient verHttp;
    httpClientBegin(verHttp, OTA_LATEST_URL);
    verHttp.setTimeout(OTA_HTTP_TIMEOUT_MS);
    verHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    int verCode = verHttp.GET();
    String latestTag = "";
    if (verCode == 301 || verCode == 302) {
        String location = verHttp.getLocation();
        int slash = location.lastIndexOf('/');
        if (slash >= 0 && slash < (int)location.length() - 1) {
            latestTag = location.substring(slash + 1);
        }
        LOG("OTA", "重定向 URL: %s → tag: %s", location.c_str(), latestTag.c_str());
    } else {
        LOG("OTA", "版本检查响应码: %d（期望 301/302）", verCode);
    }
    verHttp.end();

    if (latestTag.isEmpty()) {
        LOG("OTA", "无法解析最新版本 tag，升级中止");
        otaTaskAbort("版本查询失败，请检查网络连接");
    }
    g_latestVer = latestTag;

    // --- 阶段2: 下载并写入固件 ---
    String firmwareUrl = String(OTA_RELEASES_BASE_URL)
                         + "/download/" + latestTag
                         + "/ota-" + latestTag + ".bin";
    LOG("OTA", "开始下载固件: %s", firmwareUrl.c_str());
    g_state    = OtaState::DOWNLOADING;
    g_progress = 0;
    g_message  = "正在下载固件...";

    g_otaPart = esp_ota_get_next_update_partition(nullptr);
    if (!g_otaPart) {
        LOG("OTA", "esp_ota_get_next_update_partition 返回 null");
        otaTaskAbort("找不到可用的 OTA 分区，请确认分区表已正确烧录");
    }

    esp_err_t err = esp_ota_begin(g_otaPart, OTA_WITH_SEQUENTIAL_WRITES, &g_otaHandle);
    if (err != ESP_OK) {
        LOG("OTA", "esp_ota_begin 失败: %s", esp_err_to_name(err));
        otaTaskAbort("OTA 初始化失败: " + String(esp_err_to_name(err)));
    }

    HTTPClient dlHttp;
    httpClientBegin(dlHttp, firmwareUrl);
    dlHttp.setTimeout(OTA_HTTP_TIMEOUT_MS);
    dlHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    dlHttp.setRedirectLimit(10);
    int dlCode = dlHttp.GET();
    if (dlCode != 200) {
        dlHttp.end();
        esp_ota_abort(g_otaHandle);
        LOG("OTA", "固件下载 HTTP 响应码: %d", dlCode);
        otaTaskAbort("连接固件服务器失败，响应码: " + String(dlCode));
    }

    int contentLen = dlHttp.getSize();
    LOG("OTA", "固件大小: %d 字节", contentLen);

    WiFiClient* dlStream = dlHttp.getStreamPtr();
    uint8_t dlBuf[512];
    int totalWritten = 0;
    int remaining    = contentLen;
    bool dlFailed    = false;

    while (dlHttp.connected() && (remaining > 0 || remaining == -1)) {
        int available = dlStream->available();
        if (available > 0) {
            int toRead  = min(available, (int)sizeof(dlBuf));
            int readLen = dlStream->readBytes(dlBuf, toRead);
            if (readLen > 0) {
                esp_err_t wErr = esp_ota_write(g_otaHandle, dlBuf, readLen);
                if (wErr != ESP_OK) {
                    LOG("OTA", "esp_ota_write 失败: %s", esp_err_to_name(wErr));
                    dlFailed = true;
                    break;
                }
                totalWritten += readLen;
                if (remaining > 0) remaining -= readLen;
                if (contentLen > 0) {
                    g_progress = (uint8_t)((totalWritten * 100) / contentLen);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    dlHttp.end();

    if (dlFailed) {
        esp_ota_abort(g_otaHandle);
        LOG("OTA", "固件下载失败，已中止 OTA");
        otaTaskAbort("固件下载失败，请检查网络");
    }

    LOG("OTA", "下载完成，共写入 %d 字节，提交 OTA...", totalWritten);

    err = esp_ota_end(g_otaHandle);
    if (err != ESP_OK) {
        LOG("OTA", "esp_ota_end 失败: %s", esp_err_to_name(err));
        otaTaskAbort("固件校验失败: " + String(esp_err_to_name(err)));
    }

    err = esp_ota_set_boot_partition(g_otaPart);
    if (err != ESP_OK) {
        LOG("OTA", "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        otaTaskAbort("设置启动分区失败: " + String(esp_err_to_name(err)));
    }

    g_progress = 100;
    g_state    = OtaState::SUCCESS;
    g_message  = "升级成功！设备将在 2 秒后自动重启";
    LOG("OTA", "OTA 升级完成，2s 后重启");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    vTaskDelete(nullptr);
}

// ── otaStartVersionCheck ──────────────────────────────────────────
void otaStartVersionCheck() {
    if (g_inProgress) return;
    g_inProgress = true;
    g_latestVer  = "";
    g_state      = OtaState::IDLE;
    xTaskCreate(checkVersionTask, "ota_check", OTA_TASK_STACK_SIZE, nullptr, OTA_TASK_PRIORITY, nullptr);
}

// ── otaStartOnlineUpgrade ─────────────────────────────────────────
bool otaStartOnlineUpgrade() {
    if (g_inProgress) return false;

    g_inProgress = true;
    g_latestVer  = "";
    g_progress   = 0;
    g_message    = "";
    g_state      = OtaState::IDLE;

    BaseType_t ret = xTaskCreate(
        onlineUpgradeTask,
        "ota_online",
        OTA_TASK_STACK_SIZE,
        nullptr,
        OTA_TASK_PRIORITY,
        &g_taskHandle
    );

    if (ret != pdPASS) {
        g_inProgress = false;
        g_state      = OtaState::FAILED;
        g_message    = "无法创建 OTA 任务（内存不足）";
        LOG("OTA", "xTaskCreate 失败，OTA 任务未启动");
        return false;
    }

    return true;
}

// ── otaHandleUploadChunk ──────────────────────────────────────────
bool otaHandleUploadChunk(uint8_t* data, size_t len, size_t index, bool final) {
    esp_err_t err;

    if (index == 0) {
        // 第一块：初始化
        if (g_inProgress) return false;
        g_inProgress = true;
        g_state      = OtaState::WRITING;
        g_progress   = 0;
        g_message    = "正在写入固件...";

        g_otaPart = esp_ota_get_next_update_partition(nullptr);
        if (!g_otaPart) {
            g_state   = OtaState::FAILED;
            g_message = "找不到可用的 OTA 分区";
            LOG("OTA", "手动上传：找不到 OTA 分区");
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        err = esp_ota_begin(g_otaPart, OTA_WITH_SEQUENTIAL_WRITES, &g_otaHandle);
        if (err != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "OTA 初始化失败: " + String(esp_err_to_name(err));
            LOG("OTA", "手动上传 esp_ota_begin 失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
        LOG("OTA", "手动上传开始，OTA 分区: %s", g_otaPart->label);
    }

    // 写入当前块
    err = esp_ota_write(g_otaHandle, data, len);
    if (err != ESP_OK) {
        esp_ota_abort(g_otaHandle);
        g_state   = OtaState::FAILED;
        g_message = "固件写入失败: " + String(esp_err_to_name(err));
        LOG("OTA", "手动上传 esp_ota_write 失败: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        return false;
    }

    // 简单进度（按字节数无法精确，只在写入时递增）
    if (g_progress < 95) g_progress += 1;

    if (final) {
        err = esp_ota_end(g_otaHandle);
        if (err != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "固件校验失败: " + String(esp_err_to_name(err));
            LOG("OTA", "手动上传 esp_ota_end 失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        err = esp_ota_set_boot_partition(g_otaPart);
        if (err != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "设置启动分区失败: " + String(esp_err_to_name(err));
            LOG("OTA", "手动上传 esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        g_progress = 100;
        g_state    = OtaState::SUCCESS;
        g_message  = "上传成功！设备将在 2 秒后自动重启";
        LOG("OTA", "手动上传完成，2s 后重启");

        // 延时重启（在调用线程/中断上下文中不能用 vTaskDelay，此处使用简单 delay）
        delay(2000);
        esp_restart();
    }

    return true;
}

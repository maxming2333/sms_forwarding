#include "ota_manager.h"
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
    const esp_app_desc_t* d = esp_app_get_description();
    g_currentVer = String(d->version);
    g_state      = OtaState::IDLE;
    g_progress   = 0;
    g_message    = "";
    g_latestVer  = "";
    g_inProgress = false;
    LOG("OTA", "初始化完成，当前版本: %s", g_currentVer.c_str());
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

// ── onlineUpgradeTask — 后台 FreeRTOS 任务 ───────────────────────
static void onlineUpgradeTask(void* /*param*/) {
    // --- 阶段1: 版本检查 ---
    g_state    = OtaState::CHECKING;
    g_progress = 0;
    g_message  = "正在查询最新版本...";
    LOG("OTA", "开始版本检查: %s", OTA_LATEST_URL);

    esp_http_client_config_t verCfg = {};
    verCfg.url                      = OTA_LATEST_URL;
    verCfg.skip_cert_common_name_check = true;
    verCfg.transport_type           = HTTP_TRANSPORT_OVER_SSL;
    verCfg.disable_auto_redirect    = false;
    verCfg.max_redirection_count    = 10;
    verCfg.timeout_ms               = OTA_HTTP_TIMEOUT_MS;
    verCfg.crt_bundle_attach        = nullptr;

    esp_http_client_handle_t verClient = esp_http_client_init(&verCfg);
    if (!verClient) {
        g_state    = OtaState::FAILED;
        g_message  = "版本检查失败：HTTP 客户端初始化错误";
        LOG("OTA", "版本检查失败：HTTP 客户端初始化错误");
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t verErr = esp_http_client_perform(verClient);
    String latestTag = "";
    if (verErr == ESP_OK) {
        // 从重定向后的 URL 末尾提取 tag（如 /releases/tag/v1-abc1234）
        char urlBuf[256] = {};
        esp_http_client_get_url(verClient, urlBuf, sizeof(urlBuf) - 1);
        String finalUrl = String(urlBuf);
        int slash = finalUrl.lastIndexOf('/');
        if (slash >= 0 && slash < (int)finalUrl.length() - 1) {
            latestTag = finalUrl.substring(slash + 1);
        }
        LOG("OTA", "重定向 URL: %s → tag: %s", urlBuf, latestTag.c_str());
    } else {
        LOG("OTA", "版本检查 HTTP 错误: %s", esp_err_to_name(verErr));
    }
    esp_http_client_cleanup(verClient);

    if (latestTag.isEmpty()) {
        g_state   = OtaState::FAILED;
        g_message = "版本查询失败，请检查网络连接";
        LOG("OTA", "无法解析最新版本 tag，升级中止");
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
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
        g_state   = OtaState::FAILED;
        g_message = "找不到可用的 OTA 分区，请确认分区表已正确烧录";
        LOG("OTA", "esp_ota_get_next_update_partition 返回 null");
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = esp_ota_begin(g_otaPart, OTA_WITH_SEQUENTIAL_WRITES, &g_otaHandle);
    if (err != ESP_OK) {
        g_state   = OtaState::FAILED;
        g_message = "OTA 初始化失败: " + String(esp_err_to_name(err));
        LOG("OTA", "esp_ota_begin 失败: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_config_t dlCfg = {};
    dlCfg.url                      = firmwareUrl.c_str();
    dlCfg.skip_cert_common_name_check = true;
    dlCfg.transport_type           = HTTP_TRANSPORT_OVER_SSL;
    dlCfg.disable_auto_redirect    = false;
    dlCfg.max_redirection_count    = 10;
    dlCfg.timeout_ms               = OTA_HTTP_TIMEOUT_MS;
    dlCfg.buffer_size              = 1024;
    dlCfg.crt_bundle_attach        = nullptr;

    esp_http_client_handle_t dlClient = esp_http_client_init(&dlCfg);
    if (!dlClient) {
        esp_ota_abort(g_otaHandle);
        g_state   = OtaState::FAILED;
        g_message = "下载初始化失败";
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_http_client_open(dlClient, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(dlClient);
        esp_ota_abort(g_otaHandle);
        g_state   = OtaState::FAILED;
        g_message = "连接固件服务器失败: " + String(esp_err_to_name(err));
        LOG("OTA", "esp_http_client_open 失败: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    int contentLen = esp_http_client_fetch_headers(dlClient);
    LOG("OTA", "固件大小: %d 字节", contentLen);

    static uint8_t dlBuf[1024];
    int totalWritten = 0;
    bool dlFailed    = false;

    while (true) {
        int readLen = esp_http_client_read(dlClient, (char*)dlBuf, sizeof(dlBuf));
        if (readLen < 0) {
            LOG("OTA", "下载读取错误: %d", readLen);
            dlFailed = true;
            break;
        }
        if (readLen == 0) break;  // 下载完成

        err = esp_ota_write(g_otaHandle, dlBuf, readLen);
        if (err != ESP_OK) {
            LOG("OTA", "esp_ota_write 失败: %s", esp_err_to_name(err));
            dlFailed = true;
            break;
        }
        totalWritten += readLen;
        if (contentLen > 0) {
            g_progress = (uint8_t)((totalWritten * 100) / contentLen);
        }
    }

    esp_http_client_close(dlClient);
    esp_http_client_cleanup(dlClient);

    if (dlFailed) {
        esp_ota_abort(g_otaHandle);
        g_state   = OtaState::FAILED;
        g_message = "固件下载失败，请检查网络";
        LOG("OTA", "固件下载失败，已中止 OTA");
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    LOG("OTA", "下载完成，共写入 %d 字节，提交 OTA...", totalWritten);

    err = esp_ota_end(g_otaHandle);
    if (err != ESP_OK) {
        g_state   = OtaState::FAILED;
        g_message = "固件校验失败: " + String(esp_err_to_name(err));
        LOG("OTA", "esp_ota_end 失败: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_ota_set_boot_partition(g_otaPart);
    if (err != ESP_OK) {
        g_state   = OtaState::FAILED;
        g_message = "设置启动分区失败: " + String(esp_err_to_name(err));
        LOG("OTA", "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        vTaskDelete(nullptr);
        return;
    }

    g_progress = 100;
    g_state    = OtaState::SUCCESS;
    g_message  = "升级成功！设备将在 2 秒后自动重启";
    LOG("OTA", "OTA 升级完成，2s 后重启");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    vTaskDelete(nullptr);
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

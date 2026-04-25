#include "ota_manager.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
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

// LittleFS 手动上传专用状态
static const esp_partition_t*  g_lfsPart        = nullptr;
static size_t                  g_lfsWriteOffset = 0;
static size_t                  g_lfsTotalSize   = 0;

static void beginHttpClient(HTTPClient& http, WiFiClientSecure& tlsClient, const String& url) {
    if (url.startsWith("https://")) {
        tlsClient.setInsecure();
        http.begin(tlsClient, url);
    } else {
        http.begin(url);
    }
}

// ── otaInit ──────────────────────────────────────────────────────
void otaInit() {
    // 重建与 GitHub release tag 一致的完整版本号：
    //   APP_VERSION   = "v1-551f992"        (prefix-sha)
    //   release tag   = "v1-20260423T210356-551f992" (prefix-dateTimestamp-sha)
    // APP_BUILD_DATE = "2026-04-23" → 去除 '-' → "20260423"
    // APP_BUILD_TIME = "21:03:56"   → 去除 ':' → "210356"
    {
        String ver = String(APP_VERSION);
        int lastDash = ver.lastIndexOf('-');
        if (lastDash > 0) {
            String prefix      = ver.substring(0, lastDash);
            String sha         = ver.substring(lastDash + 1);
            String dateCompact = String(APP_BUILD_DATE); dateCompact.replace("-", "");
            String timeCompact = String(APP_BUILD_TIME); timeCompact.replace(":", "");
            g_currentVer = prefix + "-" + dateCompact + "T" + timeCompact + "-" + sha;
        } else {
            g_currentVer = ver;
        }
    }
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
    WiFiClientSecure verTls;
    beginHttpClient(verHttp, verTls, OTA_LATEST_URL);
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
    WiFiClientSecure verTls;
    beginHttpClient(verHttp, verTls, OTA_LATEST_URL);
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
    WiFiClientSecure fwTls;
    beginHttpClient(dlHttp, fwTls, firmwareUrl);
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
    unsigned long lastDataMs = millis();  // 用于检测无数据超时

    while (dlHttp.connected() && (remaining > 0 || remaining == -1)) {
        int available = dlStream->available();
        if (available > 0) {
            lastDataMs = millis();  // 收到数据，重置超时计时器
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
            // 无数据可读：检查是否超时（防止 TCP 连接活着但数据停止时无限挂起）
            if (millis() - lastDataMs > (unsigned long)OTA_HTTP_TIMEOUT_MS) {
                LOG("OTA", "固件下载超时：%d ms 内无数据，已写入 %d/%d 字节", OTA_HTTP_TIMEOUT_MS, totalWritten, contentLen);
                dlFailed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    dlHttp.end();

    if (!dlFailed && contentLen > 0 && totalWritten < contentLen) {
        LOG("OTA", "固件下载不完整：%d/%d 字节", totalWritten, contentLen);
        esp_ota_abort(g_otaHandle);
        otaTaskAbort("固件下载不完整 (" + String(totalWritten) + "/" + String(contentLen) + " 字节)");
    }

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

    // --- 阶段3: 下载并写入 LittleFS 分区 ---
    String fsUrl = String(OTA_RELEASES_BASE_URL)
                   + "/download/" + latestTag
                   + "/littlefs-" + latestTag + ".bin";
    LOG("OTA", "开始下载 LittleFS: %s", fsUrl.c_str());
    g_state    = OtaState::FLASHING_FS;
    g_progress = 0;
    g_message  = "正在下载 Web UI...";

    const esp_partition_t* fsPart = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!fsPart) {
        LOG("OTA", "找不到 LittleFS 分区，跳过 Web UI 升级");
        // 不中止升级，仅写入固件
    } else {
        HTTPClient fsHttp;
        WiFiClientSecure fsTls;
        beginHttpClient(fsHttp, fsTls, fsUrl);
        fsHttp.setTimeout(OTA_HTTP_TIMEOUT_MS);
        fsHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        fsHttp.setRedirectLimit(10);
        int fsCode = fsHttp.GET();
        if (fsCode != 200) {
            fsHttp.end();
            LOG("OTA", "LittleFS 下载 HTTP 响应码: %d，跳过 Web UI 升级", fsCode);
            // 不中止，仅升级固件
        } else {
            int fsContentLen = fsHttp.getSize();
            LOG("OTA", "LittleFS 大小: %d 字节", fsContentLen);
            g_message = "正在写入 Web UI...";

            // 卸载 LittleFS，再擦除分区
            LittleFS.end();
            esp_err_t eraseErr = esp_partition_erase_range(fsPart, 0, fsPart->size);
            if (eraseErr != ESP_OK) {
                fsHttp.end();
                LOG("OTA", "LittleFS 分区擦除失败: %s", esp_err_to_name(eraseErr));
                otaTaskAbort("Web UI 分区擦除失败: " + String(esp_err_to_name(eraseErr)));
            }

            WiFiClient* fsStream = fsHttp.getStreamPtr();
            uint8_t fsBuf[512];
            int fsTotalWritten  = 0;
            int fsRemaining     = fsContentLen;
            bool fsFailed       = false;
            unsigned long fsLastDataMs = millis();

            while (fsHttp.connected() && (fsRemaining > 0 || fsRemaining == -1)) {
                int fsAvailable = fsStream->available();
                if (fsAvailable > 0) {
                    fsLastDataMs = millis();
                    int fsToRead  = min(fsAvailable, (int)sizeof(fsBuf));
                    int fsReadLen = fsStream->readBytes(fsBuf, fsToRead);
                    if (fsReadLen > 0) {
                        esp_err_t wErr = esp_partition_write(fsPart, fsTotalWritten, fsBuf, fsReadLen);
                        if (wErr != ESP_OK) {
                            LOG("OTA", "LittleFS esp_partition_write 失败: %s", esp_err_to_name(wErr));
                            fsFailed = true;
                            break;
                        }
                        fsTotalWritten += fsReadLen;
                        if (fsRemaining > 0) fsRemaining -= fsReadLen;
                        if (fsContentLen > 0) {
                            g_progress = (uint8_t)((fsTotalWritten * 100) / fsContentLen);
                        }
                    }
                } else {
                    if (millis() - fsLastDataMs > (unsigned long)OTA_HTTP_TIMEOUT_MS) {
                        LOG("OTA", "LittleFS 下载超时，已写入 %d/%d 字节", fsTotalWritten, fsContentLen);
                        fsFailed = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            fsHttp.end();

            if (fsFailed || (fsContentLen > 0 && fsTotalWritten < fsContentLen)) {
                LOG("OTA", "LittleFS 写入不完整或失败，继续完成固件升级");
                // 不中止整体 OTA，固件已成功写入
            } else {
                LOG("OTA", "LittleFS 写入完成，共 %d 字节", fsTotalWritten);
            }
        }
    }

    // --- 阶段4: 设置启动分区并重启 ---
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

        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
            vTaskDelete(nullptr);
        }, "ota_rst", 2048, nullptr, 5, nullptr);
    }

    return true;
}

// ── otaHandleLfsUploadChunk ───────────────────────────────────────
bool otaHandleLfsUploadChunk(uint8_t* data, size_t len, size_t index, size_t totalSize, bool final) {
    if (index == 0) {
        // 第一块：初始化
        if (g_inProgress) return false;
        g_inProgress     = true;
        g_state          = OtaState::FLASHING_FS;
        g_progress       = 0;
        g_message        = "正在写入 Web UI...";
        g_lfsWriteOffset = 0;
        g_lfsTotalSize   = totalSize;

        g_lfsPart = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
        if (!g_lfsPart) {
            g_state   = OtaState::FAILED;
            g_message = "找不到 LittleFS 分区";
            LOG("OTA", "LittleFS 手动上传：找不到分区");
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }

        // 卸载 LittleFS，擦除整个分区
        LittleFS.end();
        esp_err_t eraseErr = esp_partition_erase_range(g_lfsPart, 0, g_lfsPart->size);
        if (eraseErr != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "LittleFS 分区擦除失败: " + String(esp_err_to_name(eraseErr));
            LOG("OTA", "LittleFS 手动上传 esp_partition_erase_range 失败: %s", esp_err_to_name(eraseErr));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
        LOG("OTA", "LittleFS 手动上传开始，分区: %s (0x%x, %u 字节)",
            g_lfsPart->label, (unsigned)g_lfsPart->address, (unsigned)g_lfsPart->size);
    }

    // 写入当前块
    if (len > 0) {
        esp_err_t wErr = esp_partition_write(g_lfsPart, g_lfsWriteOffset, data, len);
        if (wErr != ESP_OK) {
            g_state   = OtaState::FAILED;
            g_message = "LittleFS 写入失败: " + String(esp_err_to_name(wErr));
            LOG("OTA", "LittleFS 手动上传 esp_partition_write 失败 @ offset %u: %s",
                (unsigned)g_lfsWriteOffset, esp_err_to_name(wErr));
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
        g_lfsWriteOffset += len;
        if (g_lfsTotalSize > 0) {
            g_progress = (uint8_t)((g_lfsWriteOffset * 100) / g_lfsTotalSize);
        } else if (g_progress < 95) {
            g_progress += 1;
        }
    }

    if (final) {
        g_progress = 100;
        g_state    = OtaState::SUCCESS;
        g_message  = "Web UI 上传成功！设备将在 2 秒后自动重启";
        LOG("OTA", "LittleFS 手动上传完成，共 %u 字节，2s 后重启", (unsigned)g_lfsWriteOffset);
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
            vTaskDelete(nullptr);
        }, "lfs_rst", 2048, nullptr, 5, nullptr);
    }

    return true;
}

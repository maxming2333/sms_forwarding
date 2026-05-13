#include "ota_manager.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../logger/logger.h"
#include "../utils/http.h"

// ── 内部静态状态 ─────────────────────────────────────────────────
static volatile OtaState       g_state     = OtaState::IDLE;
static volatile uint8_t        g_progress  = 0;
static String                  g_message   = "";
static String                  g_currentVer = "";
static String                  g_latestVer  = "";
static volatile bool           g_inProgress = false;
static unsigned long           g_lastCheckMs = 0;  // 版本检查防抖时间戳

static esp_ota_handle_t        g_otaHandle  = 0;
static const esp_partition_t*  g_otaPart    = nullptr;
static TaskHandle_t            g_taskHandle = nullptr;

// LittleFS 手动上传专用状态
static const esp_partition_t*  g_lfsPart        = nullptr;
static size_t                  g_lfsWriteOffset = 0;
static size_t                  g_lfsTotalSize   = 0;

// ── Ota::version ────────────────────────────────────────
String Ota::version() {
    // 重建与 GitHub release tag 一致的完整版本号：
    //   APP_VERSION   = "1-551f992"        (prefix-sha)
    //   release tag   = "1-20260423T210356-551f992" (prefix-dateTimestamp-sha)
    // APP_BUILD_DATE = "2026-04-23" → 去除 '-' → "20260423"
    // APP_BUILD_TIME = "21:03:56"   → 去除 ':' → "210356"
    String ver = String(APP_VERSION);
    int lastDash = ver.lastIndexOf('-');
    if (lastDash > 0) {
        String prefix      = ver.substring(0, lastDash);
        String sha         = ver.substring(lastDash + 1);
        String dateCompact = String(APP_BUILD_DATE); dateCompact.replace("-", "");
        String timeCompact = String(APP_BUILD_TIME); timeCompact.replace(":", "");
        return prefix + "-" + dateCompact + "T" + timeCompact + "-" + sha;
    }
    return ver;
}

// ── Ota::init ──────────────────────────────────────────────────────
void Ota::init() {
    g_currentVer = Ota::version();
    g_state      = OtaState::IDLE;
    g_progress   = 0;
    g_message    = "";
    g_latestVer  = "";
    g_inProgress = false;
    LOG("OTA", "初始化完成，版本: %s | 编译: %s %s", g_currentVer.c_str(), APP_BUILD_DATE, APP_BUILD_TIME);
}

// ── Ota::status ─────────────────────────────────────────────────
OtaStatusPayload Ota::status() {
    OtaStatusPayload p;
    p.state          = g_state;
    p.progress       = g_progress;
    p.message        = g_message;
    p.currentVersion = g_currentVer;
    p.latestVersion  = g_latestVer;
    return p;
}

// ── fetchLatestTag — 向 GitHub 查询最新 release tag──
// 成功返回 tag 字符串，失败返回空串
static String fetchLatestTag(const String& url) {
    String latestTag = "";
    auto session = std::unique_ptr<HttpSession>(HttpSession::request(url));
    if (!session) {
        LOG("OTA", "版本检查：无法建立 HTTP 连接");
        return latestTag;
    }
    session->http()->setTimeout(OTA_HTTP_TIMEOUT_MS);
    session->http()->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    int verCode = session->http()->GET();
    if (verCode == 301 || verCode == 302) {
        String location = session->http()->getLocation();
        int slash = location.lastIndexOf('/');
        if (slash >= 0 && slash < (int)location.length() - 1) {
            latestTag = location.substring(slash + 1);
        }
        LOG("OTA", "最新版本: %s（重定向: %s）", latestTag.c_str(), location.c_str());
    } else {
        LOG("OTA", "版本检查响应码: %d（期望 301/302）", verCode);
    }
    return latestTag;
}

// ── checkVersionTask — 仅查询最新版本，不下载固件 ─────────────────
static void checkVersionTask(void* /*param*/) {
    String latestUrl = String(OTA_RELEASES_BASE_URL) + "/latest";
    LOG("OTA", "版本检查: %s", latestUrl.c_str());
    g_state      = OtaState::CHECKING;
    g_progress   = 0;
    g_message    = "正在查询最新版本...";
    g_latestVer  = fetchLatestTag(latestUrl);
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
// ── onlineUpgradeTask — 后台 FreeRTOS 任务（param = heap-allocated String* tag）──
static void onlineUpgradeTask(void* param) {
    // 取出调用方通过 param 传入的目标 tag，并立即释放堆内存
    String latestTag = *reinterpret_cast<String*>(param);
    delete reinterpret_cast<String*>(param);

    g_latestVer = latestTag;

    // 注意：每个 HTTP/TLS 阶段使用独立的 HttpSession（unique_ptr），确保
    // WiFiClientSecure 的 mbedtls 上下文（~35KB heap）在下一阶段开始前
    // 由 destructor 释放，避免多个 TLS 上下文并存导致 MBEDTLS_ERR_SSL_ALLOC_FAILED。

    // --- 阶段1（已跳过版本检查，tag 由调用方提供）---
    LOG("OTA", "开始在线升级，目标版本: %s", latestTag.c_str());

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

    bool dlFailed    = false;
    int  totalWritten = 0;
    {
        auto dlSession = std::unique_ptr<HttpSession>(HttpSession::request(firmwareUrl));
        if (!dlSession) {
            esp_ota_abort(g_otaHandle);
            otaTaskAbort("无法建立固件下载连接");
        }
        dlSession->http()->setTimeout(OTA_HTTP_TIMEOUT_MS);
        dlSession->http()->setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        dlSession->http()->setRedirectLimit(10);
        int dlCode = dlSession->http()->GET();
        if (dlCode != 200) {
            esp_ota_abort(g_otaHandle);
            LOG("OTA", "固件下载 HTTP 响应码: %d", dlCode);
            otaTaskAbort("连接固件服务器失败，响应码: " + String(dlCode));
        }

        int contentLen = dlSession->http()->getSize();
        LOG("OTA", "固件大小: %d 字节", contentLen);
        if (contentLen <= 0) {
            esp_ota_abort(g_otaHandle);
            otaTaskAbort("固件服务器未返回有效大小");
        }
        if ((size_t)contentLen > g_otaPart->size) {
            esp_ota_abort(g_otaHandle);
            LOG("OTA", "固件大小超过 OTA 分区: %d > %u", contentLen, (unsigned)g_otaPart->size);
            otaTaskAbort("固件大小超过 OTA 分区");
        }

        WiFiClient* dlStream = dlSession->http()->getStreamPtr();
        uint8_t dlBuf[512];
        int remaining        = contentLen;
        unsigned long lastDataMs = millis();

        while (dlSession->http()->connected() && (remaining > 0 || remaining == -1)) {
            int available = dlStream->available();
            if (available > 0) {
                lastDataMs = millis();
                int toRead  = min(available, (int)sizeof(dlBuf));
                int readLen = dlStream->readBytes(dlBuf, toRead);
                if (readLen > 0) {
                    if ((size_t)totalWritten + (size_t)readLen > g_otaPart->size) {
                        LOG("OTA", "固件写入超过 OTA 分区边界");
                        dlFailed = true;
                        break;
                    }
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
                if (millis() - lastDataMs > (unsigned long)OTA_HTTP_TIMEOUT_MS) {
                    LOG("OTA", "固件下载超时：%d ms 内无数据，已写入 %d/%d 字节", OTA_HTTP_TIMEOUT_MS, totalWritten, contentLen);
                    dlFailed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        if (!dlFailed && contentLen > 0 && totalWritten < contentLen) {
            LOG("OTA", "固件下载不完整：%d/%d 字节", totalWritten, contentLen);
            dlFailed = true;
        }
    }  // dlSession destructor 此处释放 HTTPClient 及 mbedtls TLS 上下文

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
        bool fsFailed      = false;
        int  fsTotalWritten = 0;
        {
            auto fsSession = std::unique_ptr<HttpSession>(HttpSession::request(fsUrl));
            if (!fsSession) {
                LOG("OTA", "LittleFS：无法建立 HTTP 连接，跳过 Web UI 升级");
                fsFailed = true;
            } else {
                fsSession->http()->setTimeout(OTA_HTTP_TIMEOUT_MS);
                fsSession->http()->setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                fsSession->http()->setRedirectLimit(10);
                int fsCode = fsSession->http()->GET();
                if (fsCode != 200) {
                    LOG("OTA", "LittleFS 下载 HTTP 响应码: %d，跳过 Web UI 升级", fsCode);
                    // 不中止，仅升级固件
                } else {
                    int fsContentLen = fsSession->http()->getSize();
                    LOG("OTA", "LittleFS 大小: %d 字节", fsContentLen);
                    if (fsContentLen <= 0) {
                        LOG("OTA", "LittleFS 响应未返回有效大小，跳过 Web UI 升级");
                        fsFailed = true;
                    } else if ((size_t)fsContentLen > fsPart->size) {
                        LOG("OTA", "LittleFS 镜像超过分区大小: %d > %u", fsContentLen, (unsigned)fsPart->size);
                        fsFailed = true;
                    } else {
                        g_message = "正在写入 Web UI...";

                        LittleFS.end();
                        esp_err_t eraseErr = esp_partition_erase_range(fsPart, 0, fsPart->size);
                        if (eraseErr != ESP_OK) {
                            LOG("OTA", "LittleFS 分区擦除失败: %s", esp_err_to_name(eraseErr));
                            otaTaskAbort("Web UI 分区擦除失败: " + String(esp_err_to_name(eraseErr)));
                        }

                        WiFiClient* fsStream = fsSession->http()->getStreamPtr();
                        uint8_t fsBuf[512];
                        int fsRemaining        = fsContentLen;
                        unsigned long fsLastDataMs = millis();

                        while (fsSession->http()->connected() && (fsRemaining > 0 || fsRemaining == -1)) {
                            int fsAvailable = fsStream->available();
                            if (fsAvailable > 0) {
                                fsLastDataMs = millis();
                                int fsToRead  = min(fsAvailable, (int)sizeof(fsBuf));
                                int fsReadLen = fsStream->readBytes(fsBuf, fsToRead);
                                if (fsReadLen > 0) {
                                    if ((size_t)fsTotalWritten + (size_t)fsReadLen > fsPart->size) {
                                        LOG("OTA", "LittleFS 写入超过分区边界");
                                        fsFailed = true;
                                        break;
                                    }
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

                        if (fsContentLen > 0 && fsTotalWritten < fsContentLen) {
                            fsFailed = true;
                        }
                    }
                }
            }
        }  // fsSession destructor 此处释放 HTTPClient 及 mbedtls TLS 上下文

        if (fsFailed) {
            LOG("OTA", "LittleFS 写入不完整或失败，继续完成固件升级");
        } else {
            LOG("OTA", "LittleFS 写入完成，共 %d 字节", fsTotalWritten);
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

// ── Ota::startVersionCheck ──────────────────────────────────────────
void Ota::startVersionCheck() {
    if (g_inProgress) return;
    unsigned long now = millis();
    if (g_lastCheckMs != 0 && now - g_lastCheckMs < OTA_CHECK_DEBOUNCE_MS) return;
    g_lastCheckMs = now;
    g_inProgress  = true;
    g_latestVer   = "";
    g_state       = OtaState::CHECKING;  // 同步设置，确保首次 getStatus() 即可见
    xTaskCreate(checkVersionTask, "ota_check", OTA_TASK_STACK_SIZE, nullptr, OTA_TASK_PRIORITY, nullptr);
}

// ── Ota::startOnlineUpgrade ─────────────────────────────────────────
bool Ota::startOnlineUpgrade(const String& targetTag) {
    if (targetTag.isEmpty()) return false;
    if (g_inProgress) return false;

    // 在堆上分配 tag，传入任务后由任务负责 delete
    String* tagParam = new String(targetTag);

    g_inProgress = true;
    g_latestVer  = targetTag;
    g_progress   = 0;
    g_message    = "";
    g_state      = OtaState::DOWNLOADING;

    BaseType_t ret = xTaskCreate(
        onlineUpgradeTask,
        "ota_online",
        OTA_TASK_STACK_SIZE,
        tagParam,
        OTA_TASK_PRIORITY,
        &g_taskHandle
    );

    if (ret != pdPASS) {
        delete tagParam;
        g_inProgress = false;
        g_state      = OtaState::FAILED;
        g_message    = "无法创建 OTA 任务（内存不足）";
        LOG("OTA", "xTaskCreate 失败，OTA 任务未启动");
        return false;
    }

    return true;
}

// ── Ota::handleUploadChunk ──────────────────────────────────────────
bool Ota::handleUploadChunk(uint8_t* data, size_t len, size_t index, bool final) {
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

    if (g_otaPart && index + len > g_otaPart->size) {
        esp_ota_abort(g_otaHandle);
        g_state   = OtaState::FAILED;
        g_message = "固件大小超过 OTA 分区";
        LOG("OTA", "手动上传超过 OTA 分区边界: %u > %u", (unsigned)(index + len), (unsigned)g_otaPart->size);
        vTaskDelay(pdMS_TO_TICKS(5000));
        g_inProgress = false;
        g_state      = OtaState::IDLE;
        return false;
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

// ── Ota::handleLfsUploadChunk ───────────────────────────────────────
bool Ota::handleLfsUploadChunk(uint8_t* data, size_t len, size_t index, size_t totalSize, bool final) {
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
        if (g_lfsPart && g_lfsWriteOffset + len > g_lfsPart->size) {
            g_state   = OtaState::FAILED;
            g_message = "LittleFS 镜像超过分区大小";
            LOG("OTA", "LittleFS 手动上传超过分区边界: %u > %u",
                (unsigned)(g_lfsWriteOffset + len), (unsigned)g_lfsPart->size);
            vTaskDelay(pdMS_TO_TICKS(5000));
            g_inProgress = false;
            g_state      = OtaState::IDLE;
            return false;
        }
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

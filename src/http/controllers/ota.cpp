#include "ota.h"
#include "ota/ota_manager.h"
#include "http/body_accumulator.h"
#include "http/json_response.h"
#include "../../logger/logger.h"
#include <AsyncJson.h>
#include <ArduinoJson.h>

// ── 辅助：OtaState → JSON 字符串 ──────────────────────────────────
static const char* otaStateToStr(OtaState s) {
    switch (s) {
        case OtaState::IDLE:        return "idle";
        case OtaState::CHECKING:    return "checking";
        case OtaState::DOWNLOADING: return "downloading";
        case OtaState::WRITING:     return "writing";
        case OtaState::SUCCESS:     return "success";
        case OtaState::FAILED:      return "failed";
        case OtaState::FLASHING_FS: return "flashing_fs";
        default:                    return "idle";
    }
}

// ── 辅助：将 OtaStatusPayload 序列化到 JsonObject ─────────────────
static void serializeOtaStatus(const OtaStatusPayload& p, JsonObject& root) {
    root["state"]          = otaStateToStr(p.state);
    root["progress"]       = p.progress;
    root["message"]        = p.message;
    root["currentVersion"] = p.currentVersion;
    root["latestVersion"]  = p.latestVersion;
}

// ── GET /api/ota/status ───────────────────────────────────────────
void otaStatusController(AsyncWebServerRequest* request) {
    // 若当前空闲则自动触发版本检查（含防抖）
    if (Ota::status().state == OtaState::IDLE) {
        Ota::startVersionCheck();
    }
    AsyncJsonResponse* resp = new AsyncJsonResponse();
    JsonObject root = resp->getRoot();
    OtaStatusPayload status = Ota::status();
    serializeOtaStatus(status, root);
    resp->setLength();
    request->send(resp);
}

// ── GET /api/ota/version ──────────────────────────────────────────
void otaVersionController(AsyncWebServerRequest* request) {
    OtaStatusPayload status = Ota::status();

    // 每次请求时若当前空闲则触发新一轮版本检查（不使用缓存，保证显示最新版本）
    if (status.state == OtaState::IDLE) {
        Ota::startVersionCheck();
        status = Ota::status();
    }

    AsyncJsonResponse* resp = new AsyncJsonResponse();
    JsonObject root = resp->getRoot();
    serializeOtaStatus(status, root);
    resp->setLength();
    request->send(resp);
}

// ── POST /api/ota/start ───────────────────────────────────────────
void otaStartController(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    const char* body = nullptr;
    if (!httpAccumulateBody(request, data, len, index, total, HTTP_JSON_BODY_MAX_BYTES, &body)) return;
    if (body == nullptr) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    httpReleaseAccumulatedBody(request);
    String targetTag = err ? String() : (doc["tag"] | String());
    targetTag.trim();

    if (targetTag.isEmpty()) {
        JsonResp::err(request, 400, "缺少或无效的 tag 参数");
        return;
    }

    bool started = Ota::startOnlineUpgrade(targetTag);
    if (!started) {
        JsonResp::err(request, 409, "当前已有升级任务进行中，请稍候");
        return;
    }

    JsonResp::ok(request, "在线升级已开始");
}

// ── POST /api/ota/upload — onUpload 回调（逐块写入） ─────────────
void otaUploadChunkController(AsyncWebServerRequest* request,
                              const String& filename,
                              size_t index,
                              uint8_t* data,
                              size_t len,
                              bool final) {
    bool ok = Ota::handleUploadChunk(data, len, index, final);
    if (!ok) {
        // 写入失败：立即发送 500（ESPAsyncWebServer 允许在 upload 回调中发送）
        JsonResp::err(request, 500, "固件写入失败");
    }
}

// ── POST /api/ota/upload — onRequest 回调（发送最终响应） ─────────
void otaUploadCompleteController(AsyncWebServerRequest* request) {
    OtaStatusPayload status = Ota::status();

    if (status.state == OtaState::SUCCESS) {
        JsonResp::ok(request, "固件上传成功，设备即将重启");
    } else if (status.state == OtaState::FAILED) {
        JsonResp::err(request, 500, status.message.isEmpty() ? "固件上传失败" : status.message);
    } else {
        // 并发冲突或状态异常
        JsonResp::err(request, 409, "升级状态异常，请刷新后重试");
    }
}

// ── POST /api/ota/upload-fs — onUpload 回调（逐块写入 LittleFS） ──
void otaUploadFsChunkController(AsyncWebServerRequest* request,
                                const String& /*filename*/,
                                size_t index,
                                uint8_t* data,
                                size_t len,
                                bool final) {
    size_t totalSize = 0;
    if (request->hasHeader("Content-Length")) {
        totalSize = (size_t)request->header("Content-Length").toInt();
    }
    bool ok = Ota::handleLfsUploadChunk(data, len, index, totalSize, final);
    if (!ok) {
        JsonResp::err(request, 500, "LittleFS 写入失败");
    }
}

// ── POST /api/ota/upload-fs — onRequest 回调（发送最终响应） ───────
void otaUploadFsCompleteController(AsyncWebServerRequest* request) {
    OtaStatusPayload status = Ota::status();

    if (status.state == OtaState::SUCCESS) {
        JsonResp::ok(request, "Web UI 上传成功，设备即将重启");
    } else if (status.state == OtaState::FAILED) {
        JsonResp::err(request, 500, status.message.isEmpty() ? "LittleFS 上传失败" : status.message);
    } else {
        JsonResp::err(request, 409, "升级状态异常，请刷新后重试");
    }
}

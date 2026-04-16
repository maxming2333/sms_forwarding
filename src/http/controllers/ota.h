#pragma once
#include <ESPAsyncWebServer.h>

// GET /api/ota/status — 返回当前 OTA 状态快照（前端 1s 轮询）
void otaStatusController(AsyncWebServerRequest* request);

// GET /api/ota/version — 触发版本检查（若 IDLE 则启动后台任务），返回状态快照
void otaVersionController(AsyncWebServerRequest* request);

// POST /api/ota/start — 触发在线升级（下载 + 写入 + 重启）
void otaStartController(AsyncWebServerRequest* request);

// POST /api/ota/upload 的 onRequest 回调（上传完成后发送最终响应）
void otaUploadCompleteController(AsyncWebServerRequest* request);

// POST /api/ota/upload 的 onUpload 回调（逐块写入 Flash）
void otaUploadChunkController(AsyncWebServerRequest* request,
                              const String& filename,
                              size_t index,
                              uint8_t* data,
                              size_t len,
                              bool final);

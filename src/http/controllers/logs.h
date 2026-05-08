#pragma once
#include <ESPAsyncWebServer.h>

// GET /api/logs?n=100
// 返回最近 n 条日志（纯文本，默认全量，优先读 LittleFS 文件）。
void logsController(AsyncWebServerRequest* request);

// DELETE /api/logs
// 清空 LittleFS 日志文件。
void logsDeleteController(AsyncWebServerRequest* request);

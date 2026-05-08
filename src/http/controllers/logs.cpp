#include "logs.h"
#include "../../logger/logger.h"
#include <LittleFS.h>
#include <memory>

void logsController(AsyncWebServerRequest* request) {
  int n = Logger::BUF_LINES;
  if (request->hasParam("n")) {
    n = request->getParam("n")->value().toInt();
    if (n <= 0) n = Logger::BUF_LINES;
  }

  // 优先：从 LittleFS 文件流式发送（chunked），避免把整个文件载入内存
  if (LittleFS.exists(Logger::FILE_PATH)) {
    File f = LittleFS.open(Logger::FILE_PATH, "r");
    if (f) {
      size_t sz = f.size();
      size_t readMax = (size_t)n * Logger::LINE_LEN;
      if (sz > readMax) {
        f.seek(sz - readMax);
        f.readStringUntil('\n');  // 跳过可能被截断的首行
      }
      // shared_ptr 持有 File 生命周期，随 lambda 自动释放
      auto fp = std::make_shared<File>(f);
      auto* response = request->beginChunkedResponse(
        "text/plain; charset=utf-8",
        [fp](uint8_t* buffer, size_t maxLen, size_t /*index*/) mutable -> size_t {
          if (!fp->available()) { fp->close(); return 0; }
          return fp->read(buffer, maxLen);
        }
      );
      request->send(response);
      return;
    }
  }

  // 回退：内存缓冲（数据量小，String 拼接可接受）
  request->send(200, "text/plain; charset=utf-8", Logger::dump(n));
}

void logsDeleteController(AsyncWebServerRequest* request) {
  Logger::clearFile();
  request->send(200, "application/json", "{\"ok\":true}");
}

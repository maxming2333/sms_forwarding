#include "logs.h"
#include "../../logger/logger.h"

void logsController(AsyncWebServerRequest* request) {
  int n = Logger::DUMP_DEFAULT_LINES;
  if (request->hasParam("n")) {
    n = request->getParam("n")->value().toInt();
    if (n <= 0) n = Logger::DUMP_DEFAULT_LINES;
  }

  auto fp = Logger::dumpStream(n);
  if (!fp || !*fp) {
    request->send(200, "text/plain; charset=utf-8", "");
    return;
  }
  // lambda 按值捕获 shared_ptr：响应期间引用计数 >= 1，句柄不会提前关闭；
  // lambda 随 response 析构时 shared_ptr 引用计数归零，File 析构自动 close()。
  auto* response = request->beginChunkedResponse(
    "text/plain; charset=utf-8",
    [fp](uint8_t* buf, size_t maxLen, size_t) -> size_t {
      if (!fp->available()) { fp->close(); return 0; }
      return fp->read(buf, maxLen);
    }
  );
  request->send(response);
}

void logsDeleteController(AsyncWebServerRequest* request) {
  Logger::clearFile();
  request->send(200, "application/json", "{\"ok\":true}");
}

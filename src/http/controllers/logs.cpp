#include "logs.h"
#include "../../logger/logger.h"

void logsController(AsyncWebServerRequest* request) {
  int n = Logger::BUF_LINES;
  if (request->hasParam("n")) {
    n = request->getParam("n")->value().toInt();
    if (n <= 0) n = Logger::BUF_LINES;
  }
  String body = Logger::dump(n);
  request->send(200, "text/plain; charset=utf-8", body);
}

void logsDeleteController(AsyncWebServerRequest* request) {
  Logger::clearFile();
  request->send(200, "application/json", "{\"ok\":true}");
}

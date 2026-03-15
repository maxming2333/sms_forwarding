#include "WebServer.h"
#include "ApiHandlers.h"
#include "config/AppConfig.h"

WebServer server(80);

void webServerInit() {
  // ── SPA pages (all served with the same Vue bundle) ──────────────────────
  server.on("/",         HTTP_GET,  handleWebApp);
  server.on("/tools",    HTTP_GET,  handleWebApp);
  server.on("/api-docs", HTTP_GET,  handleWebApp);

  // ── JSON API ──────────────────────────────────────────────────────────────
  server.on("/api/status",    HTTP_GET,  handleGetStatus);
  server.on("/api/config",    HTTP_GET,  handleGetConfig);
  server.on("/api/config",    HTTP_POST, handlePostConfig);
  server.on("/api/sendsms",   HTTP_POST, handleSendSms);
  server.on("/api/query",     HTTP_GET,  handleQuery);
  server.on("/api/flight",    HTTP_GET,  handleFlightMode);
  server.on("/api/at",        HTTP_GET,  handleATCommand);
  server.on("/api/ping",      HTTP_POST, handlePing);
  server.on("/api/test_push", HTTP_POST, handleTestPush);

  // ── Legacy compatibility ──────────────────────────────────────────────────
  server.on("/save",       HTTP_POST, handlePostConfig);
  server.on("/sendsms",    HTTP_POST, handleSendSms);
  server.on("/query",      HTTP_GET,  handleQuery);
  server.on("/flight",     HTTP_GET,  handleFlightMode);
  server.on("/at",         HTTP_GET,  handleATCommand);
  server.on("/ping",       HTTP_POST, handlePing);
  server.on("/test_push",  HTTP_POST, handleTestPush);
  server.on("/sms",        HTTP_GET,  handleWebApp);  // legacy redirect

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP服务器已启动");
}


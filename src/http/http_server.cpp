#include "http_server.h"
#include "controllers/save.h"
#include "controllers/tools.h"
#include "controllers/config.h"
#include "controllers/status.h"
#include "controllers/health.h"
#include "controllers/soc.h"
#include "controllers/wifi.h"
#include "controllers/blacklist.h"
#include "controllers/ota.h"
#include "config/config.h"
#include "logger.h"
#include <LittleFS.h>

// Auth whitelist: routes that require NO authentication
static const char* const AUTH_WHITELIST[] = {"/api/health", nullptr};

static AsyncAuthenticationMiddleware g_authMiddleware;

static AsyncMiddlewareFunction g_conditionalAuth([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
  const char* url = req->url().c_str();
  for (int i = 0; AUTH_WHITELIST[i] != nullptr; ++i) {
    if (strcmp(url, AUTH_WHITELIST[i]) == 0) { next(); return; }
  }
  if (!g_authMiddleware.allowed(req)) {
    req->requestAuthentication(AsyncAuthType::AUTH_BASIC, "SMS Forwarding", "请输入账号密码");
    return;
  }
  next();
});

void refreshAuthCredentials() {
  g_authMiddleware.setUsername(config.webUser.c_str());
  g_authMiddleware.setPassword(config.webPass.c_str());
  g_authMiddleware.generateHash();
  LOG("HTTP", "认证凭证已更新");
}

void setupHttpServer(AsyncWebServer& server) {
  g_authMiddleware.setUsername(config.webUser.c_str());
  g_authMiddleware.setPassword(config.webPass.c_str());
  g_authMiddleware.setAuthType(AsyncAuthType::AUTH_BASIC);
  g_authMiddleware.setRealm("SMS Forwarding");
  g_authMiddleware.generateHash();

  server.addMiddleware(&g_conditionalAuth);

  // API routes (new)
  server.on("/api/config/export", HTTP_GET, configExportController);
  server.on("/api/config/import", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    configImportController);
  server.on("/api/config/detail",  HTTP_GET,  configController);
  server.on("/api/status",  HTTP_GET,  statusController);
  server.on("/api/health",  HTTP_GET,  healthController);
  server.on("/api/soc",     HTTP_GET,  socController);

  // WiFi configuration API
  server.on("/api/wifi", HTTP_GET, wifiGetController);
  server.on("/api/wifi", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    wifiPostController);

  // Blacklist API
  server.on("/api/blacklist", HTTP_GET, blacklistGetController);
  server.on("/api/blacklist", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    blacklistPostController);

  // Configuration save
  server.on("/api/save", HTTP_POST, saveController);
  server.on("/api/reboot", HTTP_POST, saveRebootController);

  // Tool handlers
  server.on("/sendsms", HTTP_POST, sendSmsController);
  server.on("/ping",    HTTP_POST, pingController);
  server.on("/query",   HTTP_GET,  queryController);
  server.on("/flight",  HTTP_GET,  flightModeController);
  server.on("/at",      HTTP_GET,  atCommandController);

  // Config reset API
  server.on("/api/tools/reset-token", HTTP_GET, resetTokenController);
  server.on("/api/tools/reset", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    resetConfigController);

  // OTA upgrade API
  server.on("/api/ota/status",  HTTP_GET,  otaStatusController);
  server.on("/api/ota/version", HTTP_GET,  otaVersionController);
  server.on("/api/ota/start",   HTTP_POST, otaStartController);
  server.on("/api/ota/upload",  HTTP_POST,
    otaUploadCompleteController,
    otaUploadChunkController,
    nullptr);

  // Suppress LittleFS error logs for common browser auto-requests
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(204);
  });

  // Static pages — served from LittleFS as gzip, browser decompresses automatically
  server.on("/tools", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* resp = request->beginResponse(LittleFS, "/tools.html.gz", "text/html");
    resp->addHeader("Content-Encoding", "gzip");
    request->send(resp);
  });
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* resp = request->beginResponse(LittleFS, "/index.html.gz", "text/html");
    resp->addHeader("Content-Encoding", "gzip");
    request->send(resp);
  });

  // All unmatched routes → 404 (no LittleFS lookup, no VFS error logs)
  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not Found");
  });

  server.begin();

  LOG("HTTP", "HTTP服务器已启动");
}

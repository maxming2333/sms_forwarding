#include "tools.h"
#include "config/config.h"
#include "logger.h"
#include <Preferences.h>
#include "sms/sms.h"
#include "sim/sim_dispatcher.h"
#include "wifi/wifi_manager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>

// ── Coredump ──────────────────────────────────────────────────
// RTC 内存：panic 重启后保留，断电清零。记录最后已知 wall-clock 供崩溃时间估算。
RTC_DATA_ATTR static time_t s_rtcLastKnownTime = 0;
// 最后一次崩溃时间（panic 重启时从 RTC 写入 NVS，断电后从 NVS 加载）
static time_t s_crashTime = 0;

// 扫描 coredump 分区末尾，返回实际使用字节数（0 = 分区为空）
static size_t scanCoredumpUsedSize(const esp_partition_t* part) {
  uint8_t buf[256];
  for (size_t offset = part->size; offset > 0;) {
    size_t chunk = (offset >= sizeof(buf)) ? sizeof(buf) : offset;
    offset -= chunk;
    if (esp_partition_read(part, offset, buf, chunk) != ESP_OK) return 0;
    for (int i = static_cast<int>(chunk) - 1; i >= 0; --i) {
      if (buf[i] != 0xFF) return offset + static_cast<size_t>(i) + 1;
    }
  }
  return 0;
}

static const esp_partition_t* findCoredumpPartition() {
  return esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA,
    ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
    nullptr
  );
}

// ── CSRF Token（内存态，非持久化）──────────────────────────────
static String g_resetToken = "";

// ── shared helpers ──────────────────────────────────────────────────
// sendATCommand: 通过 SimCommandDispatcher 串行发送 AT 指令并返回响应字符串
static String sendATCommand(const char* cmd, unsigned long timeoutMs) {
  String resp;
  simSendCommand(cmd, static_cast<uint32_t>(timeoutMs), &resp, false);
  return resp;
}

static void sendJsonResponse(AsyncWebServerRequest* request, bool success, const String& message) {
  AsyncJsonResponse* resp = new AsyncJsonResponse();
  JsonObject root = resp->getRoot();
  root["success"] = success;
  root["message"] = message;
  resp->setLength();
  request->send(resp);
}

// ---------- handlers ----------

void sendSmsController(AsyncWebServerRequest* request) {
  String phone   = request->hasParam("phone",   true) ? request->getParam("phone",   true)->value() : "";
  String content = request->hasParam("content", true) ? request->getParam("content", true)->value() : "";
  phone.trim(); content.trim();

  if (phone.length() == 0)   { sendJsonResponse(request, false, "错误：请输入目标号码"); return; }
  if (content.length() == 0) { sendJsonResponse(request, false, "错误：请输入短信内容"); return; }

  LOG("HTTP", "网页端发送短信请求，目标: %s", phone.c_str());

  bool success = sendSMSPDU(phone.c_str(), content.c_str());

  sendJsonResponse(request, success, success ? "短信发送成功！" : "短信发送失败，请检查模组状态");
}

void pingController(AsyncWebServerRequest* request) {
  LOG("HTTP", "网页端发起Ping请求");

  sendATCommand("AT+CGACT=1,1", 10000);
  delay(500);

  // AT+MPING 为异步多行响应，通过 Serial1 直接收取（reader task 此时已阻塞在调用方等待）
  while (Serial1.available()) Serial1.read();
  Serial1.println("AT+MPING=\"8.8.8.8\",30,1");

  unsigned long start = millis();
  String resp;
  bool gotPingResult = false, gotError = false;
  String pingResultMsg;

  while (millis() - start < 35000) {
    while (Serial1.available()) {
      char c = Serial1.read(); resp += c;
      if (resp.indexOf("+CME ERROR") >= 0 || resp.indexOf("ERROR") >= 0) {
        gotError = true; pingResultMsg = "模组返回错误"; break;
      }
      int mpingIdx = resp.indexOf("+MPING:");
      if (mpingIdx >= 0) {
        int lineEnd = resp.indexOf('\n', mpingIdx);
        if (lineEnd >= 0) {
          String mpingLine = resp.substring(mpingIdx, lineEnd); mpingLine.trim();
          int colonIdx = mpingLine.indexOf(':');
          if (colonIdx >= 0) {
            String params = mpingLine.substring(colonIdx + 1); params.trim();
            int commaIdx = params.indexOf(',');
            String resultStr = (commaIdx >= 0) ? params.substring(0, commaIdx) : params;
            resultStr.trim();
            int result = resultStr.toInt();
            gotPingResult = true;
            bool pingOk = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);
            if (pingOk) {
              pingResultMsg = "Ping成功";
              int idx1 = params.indexOf(',');
              if (idx1 >= 0) {
                String rest = params.substring(idx1 + 1);
                String ip;
                int idx2;
                if (rest.startsWith("\"")) { int qe = rest.indexOf('\"', 1); ip = rest.substring(1, qe); idx2 = rest.indexOf(',', qe); }
                else { idx2 = rest.indexOf(','); ip = rest.substring(0, idx2); }
                if (idx2 >= 0) {
                  rest = rest.substring(idx2 + 1);
                  int idx3 = rest.indexOf(',');
                  if (idx3 >= 0) {
                    rest = rest.substring(idx3 + 1);
                    int idx4 = rest.indexOf(',');
                    String timeStr = (idx4 >= 0) ? rest.substring(0, idx4) : rest;
                    String ttlStr  = (idx4 >= 0) ? rest.substring(idx4 + 1) : "N/A";
                    timeStr.trim(); ttlStr.trim();
                    pingResultMsg = "目标: " + ip + ", 延迟: " + timeStr + "ms, TTL: " + ttlStr;
                  }
                }
              }
            } else {
              pingResultMsg = "Ping超时或目标不可达 (错误码: " + String(result) + ")";
            }
            break;
          }
        }
      }
    }
    if (gotError || gotPingResult) break;
    delay(10);
  }

  sendATCommand("AT+CGACT=0,1", 5000);

  if (gotPingResult && pingResultMsg.indexOf("延迟") >= 0) {
    sendJsonResponse(request, true,  pingResultMsg);
  } else if (gotError || gotPingResult) {
    sendJsonResponse(request, false, pingResultMsg);
  } else {
    sendJsonResponse(request, false, "操作超时，未收到Ping结果");
  }
}

void queryController(AsyncWebServerRequest* request) {
  String type = request->hasParam("type") ? request->getParam("type")->value() : "";
  bool success = false;
  String message;

  if (type == "ati") {
    String resp = sendATCommand("ATI", 2000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      String mfr, model, ver;
      int ls = 0, ln = 0;
      for (int i = 0; i <= (int)resp.length(); i++) {
        if (resp.charAt(i) == '\n' || i == (int)resp.length()) {
          String line = resp.substring(ls, i); line.trim();
          if (line.length() > 0 && line != "ATI" && line != "OK") {
            ln++;
            if (ln == 1) mfr = line;
            else if (ln == 2) model = line;
            else if (ln == 3) ver = line;
          }
          ls = i + 1;
        }
      }
      message = "<table class='info-table'>";
      message += "<tr><td>制造商</td><td>" + mfr + "</td></tr>";
      message += "<tr><td>模组型号</td><td>" + model + "</td></tr>";
      message += "<tr><td>固件版本</td><td>" + ver + "</td></tr>";
      message += "</table>";
    } else { message = "查询失败"; }
  }
  else if (type == "signal") {
    String resp = sendATCommand("AT+CESQ", 2000);
    if (resp.indexOf("+CESQ:") >= 0) {
      success = true;
      int idx = resp.indexOf("+CESQ:");
      String params = resp.substring(idx + 6);
      int endIdx = params.indexOf('\r'); if (endIdx < 0) endIdx = params.indexOf('\n');
      if (endIdx > 0) params = params.substring(0, endIdx); params.trim();
      String values[6]; int vi = 0, sp = 0;
      for (int i = 0; i <= (int)params.length() && vi < 6; i++) {
        if (i == (int)params.length() || params.charAt(i) == ',') {
          values[vi] = params.substring(sp, i); values[vi].trim(); vi++; sp = i + 1;
        }
      }
      int rsrp = values[5].toInt();
      String rsrpStr = (rsrp == 99 || rsrp == 255) ? "未知" : (String(-140 + rsrp) + " dBm");
      int rsrq = values[4].toInt();
      String rsrqStr = (rsrq == 99 || rsrq == 255) ? "未知" : (String(-19.5f + rsrq * 0.5f, 1) + " dB");
      message = "<table class='info-table'>";
      message += "<tr><td>信号强度 (RSRP)</td><td>" + rsrpStr + "</td></tr>";
      message += "<tr><td>信号质量 (RSRQ)</td><td>" + rsrqStr + "</td></tr>";
      message += "<tr><td>原始数据</td><td>" + params + "</td></tr>";
      message += "</table>";
    } else { message = "查询失败"; }
  }
  else if (type == "siminfo") {
    success = true; message = "<table class='info-table'>";
    String resp = sendATCommand("AT+CIMI", 2000);
    String imsi = "未知";
    if (resp.indexOf("OK") >= 0) {
      int s = resp.indexOf('\n'), e = resp.indexOf('\n', s + 1);
      if (e < 0) e = resp.indexOf('\r', s + 1);
      if (e > s) { imsi = resp.substring(s + 1, e); imsi.trim(); if (imsi == "OK" || imsi.length() < 10) imsi = "未知"; }
    }
    message += "<tr><td>IMSI</td><td>" + imsi + "</td></tr>";
    resp = sendATCommand("AT+ICCID", 2000);
    String iccid = "未知";
    if (resp.indexOf("+ICCID:") >= 0) { int idx = resp.indexOf("+ICCID:"); String tmp = resp.substring(idx + 7); int ei = tmp.indexOf('\r'); if (ei < 0) ei = tmp.indexOf('\n'); if (ei > 0) iccid = tmp.substring(0, ei); iccid.trim(); }
    message += "<tr><td>ICCID</td><td>" + iccid + "</td></tr>";
    resp = sendATCommand("AT+CNUM", 2000);
    String phoneNum = "未存储或不支持";
    if (resp.indexOf("+CNUM:") >= 0) { int idx = resp.indexOf(",\""); if (idx >= 0) { int ei = resp.indexOf("\"", idx + 2); if (ei > idx) phoneNum = resp.substring(idx + 2, ei); } }
    message += "<tr><td>本机号码</td><td>" + phoneNum + "</td></tr>";
    message += "</table>";
  }
  else if (type == "network") {
    success = true; message = "<table class='info-table'>";
    String resp = sendATCommand("AT+CEREG?", 2000);
    String regStatus = "未知";
    if (resp.indexOf("+CEREG:") >= 0) {
      int idx = resp.indexOf("+CEREG:"); String tmp = resp.substring(idx + 7); int ci = tmp.indexOf(',');
      if (ci >= 0) { int s = tmp.substring(ci + 1, ci + 2).toInt(); const char* names[] = {"未注册，未搜索","已注册，本地网络","未注册，正在搜索","注册被拒绝","未知","已注册，漫游"}; regStatus = (s >= 0 && s <= 5) ? names[s] : "状态码:" + String(s); }
    }
    message += "<tr><td>网络注册</td><td>" + regStatus + "</td></tr>";
    resp = sendATCommand("AT+COPS?", 2000);
    String oper = "未知";
    if (resp.indexOf("+COPS:") >= 0) { int idx = resp.indexOf(",\""); if (idx >= 0) { int ei = resp.indexOf("\"", idx + 2); if (ei > idx) oper = resp.substring(idx + 2, ei); } }
    message += "<tr><td>运营商</td><td>" + oper + "</td></tr>";
    resp = sendATCommand("AT+CGACT?", 2000);
    message += "<tr><td>数据连接</td><td>" + String(resp.indexOf("+CGACT: 1,1") >= 0 ? "已激活" : "未激活") + "</td></tr>";
    resp = sendATCommand("AT+CGDCONT?", 2000);
    String apn = "未知";
    if (resp.indexOf("+CGDCONT:") >= 0) { int idx = resp.indexOf(",\""); if (idx >= 0) { idx = resp.indexOf(",\"", idx + 2); if (idx >= 0) { int ei = resp.indexOf("\"", idx + 2); if (ei > idx) apn = resp.substring(idx + 2, ei); if (apn.length() == 0) apn = "(自动)"; } } }
    message += "<tr><td>APN</td><td>" + apn + "</td></tr>";
    message += "</table>";
  }
  else if (type == "wifi") {
    success = true; message = "<table class='info-table'>";
    message += "<tr><td>连接状态</td><td>" + String(WiFi.isConnected() ? "已连接" : "未连接") + "</td></tr>";
    String ssid = WiFi.SSID(); if (ssid.length() == 0) ssid = "未知";
    message += "<tr><td>当前SSID</td><td>" + ssid + "</td></tr>";
    int rssi = WiFi.RSSI();
    String rssiStr = String(rssi) + " dBm";
    if (rssi >= -50) rssiStr += " (信号极好)";
    else if (rssi >= -60) rssiStr += " (信号很好)";
    else if (rssi >= -70) rssiStr += " (信号良好)";
    else if (rssi >= -80) rssiStr += " (信号一般)";
    else if (rssi >= -90) rssiStr += " (信号较弱)";
    else rssiStr += " (信号很差)";
    message += "<tr><td>信号强度</td><td>" + rssiStr + "</td></tr>";
    message += "<tr><td>IP地址</td><td>" + wifiManagerGetIP() + "</td></tr>";
    message += "<tr><td>网关</td><td>" + WiFi.gatewayIP().toString() + "</td></tr>";
    message += "<tr><td>子网掩码</td><td>" + WiFi.subnetMask().toString() + "</td></tr>";
    message += "<tr><td>DNS服务器</td><td>" + WiFi.dnsIP().toString() + "</td></tr>";
    message += "<tr><td>MAC地址</td><td>" + WiFi.macAddress() + "</td></tr>";
    message += "<tr><td>WiFi信道</td><td>" + String(WiFi.channel()) + "</td></tr>";
    message += "</table>";
  }
  else { message = "未知的查询类型"; }

  sendJsonResponse(request, success, message);
}

void flightModeController(AsyncWebServerRequest* request) {
  String action = request->hasParam("action") ? request->getParam("action")->value() : "";
  bool success = false;
  String message;

  if (action == "query") {
    String resp = sendATCommand("AT+CFUN?", 2000);
    if (resp.indexOf("+CFUN:") >= 0) {
      success = true;
      int idx = resp.indexOf("+CFUN:"); int mode = resp.substring(idx + 6).toInt();
      String modeStr, icon;
      if (mode == 0)      { modeStr = "最小功能模式（关机）"; icon = "🔴"; }
      else if (mode == 1) { modeStr = "全功能模式（正常）";   icon = "🟢"; }
      else if (mode == 4) { modeStr = "飞行模式（射频关闭）"; icon = "✈️"; }
      else                { modeStr = "未知模式 (" + String(mode) + ")"; icon = "❓"; }
      message = "<table class='info-table'>";
      message += "<tr><td>当前状态</td><td>" + icon + " " + modeStr + "</td></tr>";
      message += "<tr><td>CFUN值</td><td>" + String(mode) + "</td></tr>";
      message += "</table>";
    } else { message = "查询失败"; }
  }
  else if (action == "toggle") {
    String resp = sendATCommand("AT+CFUN?", 2000);
    if (resp.indexOf("+CFUN:") >= 0) {
      int idx = resp.indexOf("+CFUN:"); int cur = resp.substring(idx + 6).toInt();
      int newMode = (cur == 1) ? 4 : 1;
      String setResp = sendATCommand(("AT+CFUN=" + String(newMode)).c_str(), 5000);
      if (setResp.indexOf("OK") >= 0) {
        success = true;
        message = (newMode == 4) ? "已开启飞行模式 ✈️<br>模组射频已关闭，无法收发短信" : "已关闭飞行模式 🟢<br>模组恢复正常工作";
      } else { message = "切换失败: " + setResp; }
    } else { message = "无法获取当前状态"; }
  }
  else if (action == "on") {
    String resp = sendATCommand("AT+CFUN=4", 5000);
    if (resp.indexOf("OK") >= 0) { success = true; message = "已开启飞行模式 ✈️"; } else { message = "开启失败: " + resp; }
  }
  else if (action == "off") {
    String resp = sendATCommand("AT+CFUN=1", 5000);
    if (resp.indexOf("OK") >= 0) { success = true; message = "已关闭飞行模式 🟢"; } else { message = "关闭失败: " + resp; }
  }
  else { message = "未知操作"; }

  sendJsonResponse(request, success, message);
}

void atCommandController(AsyncWebServerRequest* request) {
  String cmd = request->hasParam("cmd") ? request->getParam("cmd")->value() : "";
  if (cmd.length() == 0) { sendJsonResponse(request, false, "错误：指令不能为空"); return; }

  LOG("HTTP", "网页端发送AT指令: %s", cmd.c_str());
  String resp = sendATCommand(cmd.c_str(), 5000);
  LOG("HTTP", "模组响应: %s", resp.c_str());

  if (resp.length() > 0) {
    sendJsonResponse(request, true,  resp);
  } else {
    sendJsonResponse(request, false, "超时或无响应");
  }
}

// ── 重置 CSRF token 生成端点 ────────────────────────────────
void resetTokenController(AsyncWebServerRequest* request) {
  uint32_t rnd = esp_random();
  char buf[9];
  snprintf(buf, sizeof(buf), "%08x", rnd);
  g_resetToken = String(buf);

  AsyncJsonResponse* resp = new AsyncJsonResponse();
  resp->getRoot()["token"] = g_resetToken;
  resp->setLength();
  request->send(resp);
}

// ── 重置配置端点 ────────────────────────────────────────────
void resetConfigController(AsyncWebServerRequest* request, uint8_t* data,
                           size_t len, size_t index, size_t total) {
  if (index + len < total) return;

  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok
      || !doc.is<JsonObject>()) {
    request->send(400, "application/json",
      "{\"ok\":false,\"error\":\"请求格式错误\"}");
    return;
  }

  const char* token = doc["token"] | "";
  if (g_resetToken.length() == 0 || strcmp(token, g_resetToken.c_str()) != 0) {
    request->send(403, "application/json",
      "{\"ok\":false,\"error\":\"token无效或已过期，请重新获取\"}");
    return;
  }

  g_resetToken = "";
  resetConfig();

  request->send(200, "application/json", "{\"ok\":true,\"message\":\"配置已重置，设备将在2秒后自动重启\"}");
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP.restart();
    vTaskDelete(nullptr);
  }, "restart", 2048, nullptr, 1, nullptr);
}

// ── 重启设备端点 ─────────────────────────────────────────────
void rebootController(AsyncWebServerRequest* request, uint8_t* data,
                      size_t len, size_t index, size_t total) {
  if (index + len < total) return;

  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok
      || !doc.is<JsonObject>()) {
    request->send(400, "application/json",
      "{\"ok\":false,\"error\":\"请求格式错误\"}");
    return;
  }

  const char* token = doc["token"] | "";
  if (g_resetToken.length() == 0 || strcmp(token, g_resetToken.c_str()) != 0) {
    request->send(403, "application/json",
      "{\"ok\":false,\"error\":\"token无效或已过期，请重新获取\"}");
    return;
  }

  g_resetToken = "";
  LOG("HTTP", "网页端触发设备重启");

  request->send(200, "application/json", "{\"ok\":true,\"message\":\"设备将在2秒后重启\"}");
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP.restart();
    vTaskDelete(nullptr);
  }, "reboot", 2048, nullptr, 1, nullptr);
}

void exportCoreDumpController(AsyncWebServerRequest* request) {
  const esp_partition_t* part = findCoredumpPartition();

  if (part == nullptr) {
    request->send(404, "application/json",
      "{\"ok\":false,\"error\":\"未找到 coredump 分区\"}");
    return;
  }

  size_t usedSize = scanCoredumpUsedSize(part);

  if (usedSize == 0) {
    request->send(404, "application/json",
      "{\"ok\":false,\"error\":\"未发现可导出的 coredump（分区为空）\"}");
    return;
  }

  LOG("HTTP", "导出 coredump: used=%u bytes, part=%s", static_cast<unsigned>(usedSize), part->label);

  AsyncWebServerResponse* resp = request->beginChunkedResponse(
    "application/octet-stream",
    [part, usedSize](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
      if (index >= usedSize) return 0;

      size_t left = usedSize - index;
      size_t toRead = left < maxLen ? left : maxLen;
      if (esp_partition_read(part, index, buffer, toRead) != ESP_OK) {
        return 0;
      }
      return toRead;
    }
  );

  String cdFilename;
  if (s_crashTime > 0) {
    char timeBuf[20];
    struct tm tmInfo;
    gmtime_r(&s_crashTime, &tmInfo);
    strftime(timeBuf, sizeof(timeBuf), "%Y%m%dT%H%M%S", &tmInfo);
    cdFilename = getDeviceName() + "-coredump-" + timeBuf + ".bin";
  } else {
    cdFilename = getDeviceName() + "-coredump-unknown.bin";
  }
  resp->addHeader("Content-Disposition", "attachment; filename=" + cdFilename);
  resp->addHeader("Cache-Control", "no-store");
  resp->addHeader("X-CoreDump-Size", String(usedSize));
  request->send(resp);
}

void coredumpUpdateLastKnownTime(time_t t) {
  s_rtcLastKnownTime = t;
  // NVS 写入限流：仅当时间变化超过 60 秒才刷新，减少 flash 写入
  static time_t s_lastNvsWrite = 0;
  if (t - s_lastNvsWrite >= 60) {
    Preferences prefs;
    if (prefs.begin("sms_config", false)) {
      prefs.putLong("cdLastTs", (long)t);
      prefs.end();
    }
    s_lastNvsWrite = t;
  }
}

void coredumpInit() {
  Preferences prefs;
  prefs.begin("sms_config", false);

  // 断电重启时 RTC 归零，从 NVS 恢复最后已知时间
  if (s_rtcLastKnownTime == 0) {
    long saved = prefs.getLong("cdLastTs", 0);
    if (saved > 0) s_rtcLastKnownTime = (time_t)saved;
  }

  // 若系统时钟仍在 1970 附近，用 NVS 保存的近似时间先行恢复（NTP/NITZ 同步后精确覆盖）
  if (time(nullptr) < 1577836800L && s_rtcLastKnownTime > 1577836800L) {
    struct timeval tv;
    tv.tv_sec  = s_rtcLastKnownTime;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    LOG("Time", "从 NVS 恢复系统时间（近似）: %ld", (long)s_rtcLastKnownTime);
  }

  // 从 NVS 加载上次崩溃时间
  long savedCrash = prefs.getLong("cdCrashTs", 0);
  if (savedCrash > 0) s_crashTime = (time_t)savedCrash;

  // panic 重启：RTC 时间即为崩溃前最后已知时间，写入 NVS 使其与 coredump 文件对应。
  // 连续崩溃时 coredump 文件被覆盖，此处同步覆盖崩溃时间，保持二者一致。
  if (esp_reset_reason() == ESP_RST_PANIC && s_rtcLastKnownTime > 0) {
    s_crashTime = s_rtcLastKnownTime;
    prefs.putLong("cdCrashTs", (long)s_crashTime);
    LOG("Coredump", "检测到 panic 重启，崩溃时间: %ld", (long)s_crashTime);
  }

  prefs.end();
}

bool coredumpHasData() {
  const esp_partition_t* part = findCoredumpPartition();
  if (!part) return false;
  return scanCoredumpUsedSize(part) > 0;
}

time_t coredumpGetCrashTime() {
  return s_crashTime;
}

void coredumpInfoController(AsyncWebServerRequest* request) {
  const esp_partition_t* part = findCoredumpPartition();
  if (!part) {
    request->send(200, "application/json", "{\"hasCoredump\":false}");
    return;
  }

  size_t usedSize = scanCoredumpUsedSize(part);

  AsyncJsonResponse* resp = new AsyncJsonResponse();
  JsonObject root = resp->getRoot();
  root["hasCoredump"] = (usedSize > 0);
  if (usedSize > 0) {
    root["size"]      = (unsigned int)usedSize;
    root["crashTime"] = (long long)s_crashTime;
  }
  resp->setLength();
  request->send(resp);
}

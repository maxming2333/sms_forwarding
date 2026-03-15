#include "ApiHandlers.h"
#include "WebServer.h"
#include "WebContent.h"
#include "config/AppConfig.h"
#include "sim/SimManager.h"
#include "sms/SmsSender.h"
#include "push/PushManager.h"
#include "email/EmailNotifier.h"
#include "wifi/WifiManager.h"
#include "utils/Utils.h"
#include <WiFi.h>

// ── Auth ──────────────────────────────────────────────────────────────────────
bool checkAuth() {
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS Forwarding", "请输入管理员账号密码");
    return false;
  }
  return true;
}

// ── Helper: always send Connection:close to prevent keep-alive hang ───────────
static void sendClose(int code, const char* contentType, const String& content) {
  server.sendHeader("Connection", "close");
  server.send(code, contentType, content);
}

static void sendCloseP(int code, const char* contentType, PGM_P content) {
  server.sendHeader("Connection", "close");
  server.send_P(code, contentType, content);
}

// ── SPA ───────────────────────────────────────────────────────────────────────
void handleWebApp() {
  if (!checkAuth()) return;
  sendCloseP(200, "text/html", WEB_HTML);
}

// ── 404 / favicon / unknown routes ───────────────────────────────────────────
void handleNotFound() {
  server.sendHeader("Connection", "close");
  server.send(404, "text/plain", "Not Found");
}

// ── GET /api/status ───────────────────────────────────────────────────────────
void handleGetStatus() {
  if (!checkAuth()) return;
  String ip = WiFi.status() == WL_CONNECTED
              ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String json = "{";
  json += "\"ip\":\""              + ip + "\",";
  json += "\"devicePhone\":\""     + jsonEscape(devicePhoneNumber) + "\",";
  json += "\"simPresent\":"        + String(simPresent       ? "true" : "false") + ",";
  json += "\"simInitialized\":"    + String(simInitialized   ? "true" : "false") + ",";
  json += "\"configValid\":"       + String(configValid      ? "true" : "false") + ",";
  json += "\"timeSynced\":"        + String(timeSynced       ? "true" : "false");
  json += "}";
  sendClose(200, "application/json", json);
}

// ── GET /api/config ───────────────────────────────────────────────────────────
void handleGetConfig() {
  if (!checkAuth()) return;
  String json = "{";
  json += "\"smtpServer\":\""  + jsonEscape(config.smtpServer)  + "\",";
  json += "\"smtpPort\":"      + String(config.smtpPort)         + ",";
  json += "\"smtpUser\":\""    + jsonEscape(config.smtpUser)     + "\",";
  json += "\"smtpPass\":\""    + jsonEscape(config.smtpPass)     + "\",";
  json += "\"smtpSendTo\":\""  + jsonEscape(config.smtpSendTo)   + "\",";
  json += "\"adminPhone\":\""  + jsonEscape(config.adminPhone)   + "\",";
  json += "\"webUser\":\""     + jsonEscape(config.webUser)      + "\",";
  json += "\"webPass\":\""     + jsonEscape(config.webPass)      + "\",";
  json += "\"wifiSSID\":\""    + jsonEscape(config.wifiSSID)     + "\",";
  json += "\"wifiPass\":\""    + jsonEscape(config.wifiPass)     + "\",";
  json += "\"numberBlackList\":\"" + jsonEscape(config.numberBlackList) + "\",";
  json += "\"autoRebootEnabled\":" + String(config.autoRebootEnabled ? "true" : "false") + ",";
  json += "\"autoRebootTime\":\""  + jsonEscape(config.autoRebootTime) + "\",";
  json += "\"pushChannels\":[";
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (i) json += ",";
    const PushChannel& c = config.pushChannels[i];
    json += "{";
    json += "\"enabled\":"      + String(c.enabled ? "true" : "false") + ",";
    json += "\"type\":"         + String((int)c.type)                  + ",";
    json += "\"name\":\""       + jsonEscape(c.name)                   + "\",";
    json += "\"url\":\""        + jsonEscape(c.url)                    + "\",";
    json += "\"key1\":\""       + jsonEscape(c.key1)                   + "\",";
    json += "\"key2\":\""       + jsonEscape(c.key2)                   + "\",";
    json += "\"customBody\":\"" + jsonEscape(c.customBody)             + "\",";
    json += "\"customCallBody\":\"" + jsonEscape(c.customCallBody)     + "\"";
    json += "}";
  }
  json += "]}";
  sendClose(200, "application/json", json);
}

// ── POST /api/config (also /save) ────────────────────────────────────────────
void handlePostConfig() {
  if (!checkAuth()) return;

  auto arg = [](const String& k) { return server.arg(k); };

  String newUser = arg("webUser"); if (!newUser.length()) newUser = DEFAULT_WEB_USER;
  String newPass = arg("webPass"); if (!newPass.length()) newPass = DEFAULT_WEB_PASS;
  config.webUser = newUser;
  config.webPass = newPass;

  config.smtpServer  = arg("smtpServer");
  config.smtpPort    = arg("smtpPort").toInt(); if (!config.smtpPort) config.smtpPort = 465;
  config.smtpUser    = arg("smtpUser");
  config.smtpPass    = arg("smtpPass");
  config.smtpSendTo  = arg("smtpSendTo");
  config.adminPhone  = arg("adminPhone");
  config.numberBlackList = arg("numberBlackList");

  // Scheduled reboot
  String rebootEn = arg("autoRebootEnabled");
  config.autoRebootEnabled = (rebootEn == "true" || rebootEn == "on" || rebootEn == "1");
  String rebootTime = arg("autoRebootTime");
  config.autoRebootTime = rebootTime.length() > 0 ? rebootTime : "03:00";

  String oldSSID = config.wifiSSID, oldPass = config.wifiPass;
  config.wifiSSID = arg("wifiSSID");
  config.wifiPass = arg("wifiPass");
  bool wifiChanged = (config.wifiSSID != oldSSID || config.wifiPass != oldPass);

  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    config.pushChannels[i].enabled    = arg("push" + idx + "en") == "on"
                                     || arg("push" + idx + "en") == "true"
                                     || arg("push" + idx + "en") == "1";
    config.pushChannels[i].type       = (PushType)arg("push" + idx + "type").toInt();
    config.pushChannels[i].url        = arg("push" + idx + "url");
    config.pushChannels[i].name       = arg("push" + idx + "name");
    config.pushChannels[i].key1       = arg("push" + idx + "key1");
    config.pushChannels[i].key2       = arg("push" + idx + "key2");
    config.pushChannels[i].customBody     = arg("push" + idx + "body");
    config.pushChannels[i].customCallBody = arg("push" + idx + "cbody");
    if (!config.pushChannels[i].name.length())
      config.pushChannels[i].name = "通道" + String(i + 1);
  }

  saveConfig();
  configValid = isConfigValid();

  if (configValid) emailNotify("短信转发器配置已更新",
    ("设备配置已更新\n设备地址: " + getDeviceUrl()).c_str());

  sendClose(200, "application/json",
    "{\"success\":true,\"message\":\"配置保存成功\",\"wifiChanged\":"
    + String(wifiChanged ? "true" : "false") + "}");

  if (wifiChanged) { delay(1500); ESP.restart(); }
}

// ── POST /api/sendsms ─────────────────────────────────────────────────────────
void handleSendSms() {
  if (!checkAuth()) return;
  String phone   = server.arg("phone");   phone.trim();
  String content = server.arg("content"); content.trim();
  if (!phone.length() || !content.length()) {
    sendClose(400, "application/json", "{\"success\":false,\"message\":\"号码和内容不能为空\"}");
    return;
  }
  bool ok = smsSend(phone.c_str(), content.c_str());
  sendClose(200, "application/json",
    "{\"success\":" + String(ok ? "true" : "false") +
    ",\"message\":\"" + (ok ? "短信发送成功！" : "短信发送失败，请检查模组状态") + "\"}");
}

// ── GET /api/query?type=… ─────────────────────────────────────────────────────
void handleQuery() {
  if (!checkAuth()) return;
  String type = server.arg("type");
  bool   ok   = false;
  String msg;

  if (type == "ati") {
    String r = sendATCommand("ATI", 2000);
    if (r.indexOf("OK") >= 0) {
      ok = true;
      String mfr, model, ver; int n = 0;
      int s = 0;
      for (int i = 0; i < (int)r.length(); i++) {
        if (r.charAt(i) == '\n' || i == (int)r.length() - 1) {
          String l = r.substring(s, i); l.trim();
          if (l.length() && l != "ATI" && l != "OK") {
            n++; if (n==1) mfr=l; else if (n==2) model=l; else if (n==3) ver=l;
          }
          s = i + 1;
        }
      }
      msg = "<table class='info-table'><tr><td>制造商</td><td>" + mfr
          + "</td></tr><tr><td>型号</td><td>" + model
          + "</td></tr><tr><td>固件版本</td><td>" + ver + "</td></tr></table>";
    } else { msg = "查询失败"; }
  }
  else if (type == "signal") {
    String r = sendATCommand("AT+CESQ", 2000);
    if (r.indexOf("+CESQ:") >= 0) {
      ok = true;
      int idx = r.indexOf("+CESQ"); String p = r.substring(idx + 6);
      int end = p.indexOf('\r'); if (end < 0) end = p.indexOf('\n');
      if (end > 0) p = p.substring(0, end); p.trim();
      // parse RSRP (field 6)
      String vals[6]; int vi=0, ss=0;
      for (int i = 0; i <= (int)p.length() && vi < 6; i++) {
        if (i == (int)p.length() || p.charAt(i) == ',') {
          vals[vi] = p.substring(ss, i); vals[vi].trim(); vi++; ss = i+1;
        }
      }
      int rsrp = vals[5].toInt();
      String rsrpStr = (rsrp==99||rsrp==255) ? "未知" :
        (String(-140 + rsrp) + " dBm");
      msg = "<table class='info-table'><tr><td>信号强度(RSRP)</td><td>" + rsrpStr
          + "</td></tr><tr><td>原始数据</td><td>" + p + "</td></tr></table>";
    } else { msg = "查询失败"; }
  }
  else if (type == "siminfo") {
    ok = true; msg = "<table class='info-table'>";
    // IMSI
    String r = sendATCommand("AT+CIMI", 2000);
    String imsi = "未知";
    if (r.indexOf("OK") >= 0) {
      int a = r.indexOf('\n');
      if (a >= 0) { int b = r.indexOf('\n', a+1); if (b<0) b=r.indexOf('\r',a+1);
        if (b>a) { imsi=r.substring(a+1,b); imsi.trim(); if (imsi=="OK"||imsi.length()<10) imsi="未知"; }}
    }
    msg += "<tr><td>IMSI</td><td>" + imsi + "</td></tr>";
    // ICCID
    r = sendATCommand("AT+ICCID", 2000); String iccid = "未知";
    if (r.indexOf("+ICCID:") >= 0) {
      int a = r.indexOf("+ICCID:"); String t = r.substring(a+7);
      int e = t.indexOf('\r'); if (e<0) e=t.indexOf('\n');
      if (e>0) iccid=t.substring(0,e); iccid.trim();
    }
    msg += "<tr><td>ICCID</td><td>" + iccid + "</td></tr>";
    // Own number
    r = sendATCommand("AT+CNUM", 2000); String num = "未存储或不支持";
    if (r.indexOf("+CNUM:") >= 0) {
      int a = r.indexOf(",\""); if (a>=0) { int b=r.indexOf('"',a+2); if (b>a) { num=r.substring(a+2,b);
        if (devicePhoneNumber=="未知号码" && num.length()>5) devicePhoneNumber=num; }}
    }
    msg += "<tr><td>本机号码</td><td>" + num + "</td></tr></table>";
  }
  else if (type == "network") {
    ok = true; msg = "<table class='info-table'>";
    String r = sendATCommand("AT+CEREG?", 2000); String reg = "未知";
    if (r.indexOf("+CEREG:") >= 0) {
      int i=r.indexOf("+CEREG:"); String t=r.substring(i+7); int c=t.indexOf(',');
      if (c>=0) { int s=t.substring(c+1,c+2).toInt();
        const char* st[]={"未注册,未搜索","已注册,本地","未注册,搜索中","注册被拒","未知","已注册,漫游"};
        reg = (s>=0&&s<=5) ? String(st[s]) : ("状态码:"+String(s)); }
    }
    msg += "<tr><td>网络注册</td><td>" + reg + "</td></tr>";
    r = sendATCommand("AT+COPS?", 2000); String oper = "未知";
    if (r.indexOf("+COPS:") >= 0) {
      int a=r.indexOf(",\""); if (a>=0){ int b=r.indexOf('"',a+2); if(b>a) oper=r.substring(a+2,b);}
    }
    msg += "<tr><td>运营商</td><td>" + oper + "</td></tr>";
    r = sendATCommand("AT+CGACT?", 2000);
    msg += "<tr><td>数据连接</td><td>" + String(r.indexOf("+CGACT: 1,1")>=0?"已激活":"未激活") + "</td></tr></table>";
  }
  else if (type == "wifi") {
    ok = true; msg = "<table class='info-table'>";
    msg += "<tr><td>连接状态</td><td>" + String(WiFi.isConnected()?"已连接":"未连接") + "</td></tr>";
    msg += "<tr><td>SSID</td><td>"     + WiFi.SSID() + "</td></tr>";
    msg += "<tr><td>IP地址</td><td>"   + WiFi.localIP().toString() + "</td></tr>";
    msg += "<tr><td>信号RSSI</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
    msg += "<tr><td>MAC地址</td><td>"  + WiFi.macAddress() + "</td></tr></table>";
  }
  else { msg = "未知查询类型"; }

  sendClose(200, "application/json",
    "{\"success\":" + String(ok?"true":"false") + ",\"message\":\"" + msg + "\"}");
}

// ── GET /api/flight?action=… ──────────────────────────────────────────────────
void handleFlightMode() {
  if (!checkAuth()) return;
  String action = server.arg("action");
  bool   ok = false;
  String msg;

  if (action == "query") {
    String r = sendATCommand("AT+CFUN?", 2000);
    if (r.indexOf("+CFUN:") >= 0) {
      ok = true;
      int i = r.indexOf("+CFUN:"), mode = r.substring(i+6).toInt();
      String icon = (mode==1)?"🟢":(mode==4)?"✈️":"🔴";
      String label = (mode==1)?"全功能(正常)":(mode==4)?"飞行模式":"最小功能";
      msg = "<table class='info-table'><tr><td>当前状态</td><td>" + icon + " " + label + "</td></tr>"
            "<tr><td>CFUN值</td><td>" + String(mode) + "</td></tr></table>";
    } else { msg = "查询失败"; }
  }
  else if (action == "toggle" || action == "on" || action == "off") {
    int newMode = 1;
    if (action == "toggle") {
      String r = sendATCommand("AT+CFUN?", 2000);
      if (r.indexOf("+CFUN:") >= 0) {
        int i = r.indexOf("+CFUN:"), cur = r.substring(i+6).toInt();
        newMode = (cur == 1) ? 4 : 1;
      } else { msg = "无法获取当前状态"; goto done; }
    } else { newMode = (action == "on") ? 4 : 1; }
    {
      String setR = sendATCommand(("AT+CFUN=" + String(newMode)).c_str(), 5000);
      if (setR.indexOf("OK") >= 0) {
        ok = true;
        msg = newMode==4 ? "已开启飞行模式 ✈️" : "已关闭飞行模式 🟢";
      } else { msg = "切换失败: " + setR; }
    }
  }
  else { msg = "未知操作"; }

  done:
  sendClose(200, "application/json",
    "{\"success\":" + String(ok?"true":"false") + ",\"message\":\"" + msg + "\"}");
}

// ── GET /api/at?cmd=… ────────────────────────────────────────────────────────
void handleATCommand() {
  if (!checkAuth()) return;
  String cmd = server.arg("cmd");
  if (!cmd.length()) {
    sendClose(400, "application/json", "{\"success\":false,\"message\":\"指令不能为空\"}");
    return;
  }
  Serial.println("[AT] 网页指令: " + cmd);
  String r = sendATCommand(cmd.c_str(), 5000);
  bool ok = r.length() > 0;
  sendClose(200, "application/json",
    "{\"success\":" + String(ok?"true":"false") + ",\"message\":\"" + jsonEscape(ok ? r : "超时或无响应") + "\"}");
}

// ── POST /api/ping ────────────────────────────────────────────────────────────
void handlePing() {
  if (!checkAuth()) return;
  Serial.println("[Ping] 开始Ping测试");
  while (Serial1.available()) Serial1.read();
  sendATCommand("AT+CGACT=1,1", 10000);
  while (Serial1.available()) Serial1.read();
  delay(500);

  Serial1.println("AT+MPING=\"8.8.8.8\",30,1");
  unsigned long t = millis();
  String resp;
  bool gotResult = false, gotError = false;
  String resultMsg;

  while (millis() - t < 35000) {
    while (Serial1.available()) {
      char c = Serial1.read(); resp += c; Serial.print(c);
      if (resp.indexOf("+CME ERROR") >= 0 || (resp.indexOf("ERROR") >= 0)) {
        gotError = true; resultMsg = "模组返回错误"; break;
      }
      int mi = resp.indexOf("+MPING:");
      if (mi >= 0) {
        int le = resp.indexOf('\n', mi);
        if (le >= 0) {
          String line = resp.substring(mi, le); line.trim();
          int ci = line.indexOf(':'); if (ci < 0) continue;
          String params = line.substring(ci + 1); params.trim();
          int comma = params.indexOf(',');
          String resultStr = comma >= 0 ? params.substring(0, comma) : params;
          resultStr.trim();
          int res = resultStr.toInt();
          gotResult = true;
          bool success = (res == 0 || res == 1) || (comma >= 0 && params.length() > 5);
          if (success) {
            // parse ip,len,time,ttl
            if (comma >= 0) {
              String rest = params.substring(comma + 1);
              if (rest.startsWith("\"")) {
                int qe = rest.indexOf('"', 1);
                rest = rest.substring(qe + 2);
              } else {
                int c2 = rest.indexOf(','); if (c2 >= 0) rest = rest.substring(c2 + 1);
              }
              int c3 = rest.indexOf(','), c4 = c3 >= 0 ? rest.indexOf(',', c3+1) : -1;
              String tstr = c3 >= 0 ? (c4 >= 0 ? rest.substring(c3+1, c4) : rest.substring(c3+1)) : "";
              tstr.trim();
              resultMsg = "Ping成功，延迟: " + tstr + "ms";
            } else { resultMsg = "Ping成功"; }
          } else {
            resultMsg = "Ping超时或目标不可达 (错误码: " + String(res) + ")";
          }
          break;
        }
      }
    }
    if (gotError || gotResult) break;
    delay(10);
  }
  Serial.println("\n[Ping] 完成");
  sendATCommand("AT+CGACT=0,1", 5000);

  bool suc = gotResult && resultMsg.indexOf("成功") >= 0;
  if (!gotResult && !gotError) resultMsg = "操作超时，未收到Ping结果";
  sendClose(200, "application/json",
    "{\"success\":" + String(suc?"true":"false") + ",\"message\":\"" + jsonEscape(resultMsg) + "\"}");
}

// ── POST /api/test_push ───────────────────────────────────────────────────────
void handleTestPush() {
  if (!checkAuth()) return;
  PushChannel tc;
  tc.enabled    = true;
  tc.type       = (PushType)server.arg("type").toInt();
  tc.url        = server.arg("url");
  tc.key1       = server.arg("key1");
  tc.key2       = server.arg("key2");
  tc.customBody = server.arg("body");
  tc.name       = "测试通道";

  String ts = "测试时间";
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm ti; localtime_r(&now, &ti);
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    ts = String(buf);
  }

  Serial.println("[TestPush] 类型=" + String((int)tc.type));
  int code = pushOne(tc, "测试发送者",
    "🧪 这是一条来自【短信转发器】的测试消息，如果收到说明推送通道配置正确！",
    ts.c_str(), devicePhoneNumber.c_str());

  bool ok = (code >= 200 && code < 300) || code == 0;
  String resp = ok
    ? "测试消息发送成功！(HTTP " + String(code) + ")"
    : (code == -1 ? "配置不完整，请检查必填项"
                  : "请求失败 (" + String(code) + ")，请检查配置");
  sendClose(200, "application/json",
    "{\"success\":" + String(ok?"true":"false") + ",\"message\":\"" + resp + "\"}");
}


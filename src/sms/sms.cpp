#include "sms.h"
#include "config/config.h"
#include "push/push.h"
#include "../logger/logger.h"
#include "phone_utils.h"
#include "sim/sim_dispatcher.h"
#include <pdulib.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// 队列项类型：PDU（短信） vs USSD（运营商交互上报）。
// 二者均需在 sms_proc 任务上下文执行推送，避免阻塞 SIM reader task。
enum class SmsItemKind : uint8_t { PDU = 0, USSD = 1 };

struct PduQueueItem {
    SmsItemKind kind;
    char        data[PDU_MAX_LEN + 1];
};

static QueueHandle_t s_pduQueue = nullptr;

static PDU pdu = PDU(4096);
static ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

// ---------- helpers ----------

static bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  String s = String(sender);
  String a = config.adminPhone;
  if (s.startsWith("+86")) s = s.substring(3);
  if (a.startsWith("+86")) a = a.substring(3);
  return s.equals(a);
}

static String assembleConcatSms(int slot) {
  String result;
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

static void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse         = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender        = "";
  concatBuffer[slot].timestamp     = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text  = "";
  }
}

static int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 匹配已有槽位：refNumber + sender + totalParts 三者相同才视为同一条长短信
  // 加入 totalParts 可区分同一发送者、相同参考号但总段数不同的并发长短信
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse &&
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].totalParts == totalParts &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse         = true;
      concatBuffer[i].refNumber     = refNumber;
      concatBuffer[i].sender        = String(sender);
      concatBuffer[i].totalParts    = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text  = "";
      }
      return i;
    }
  }
  // 找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  LOG("SMS", "长短信缓存已满，覆盖最老的槽位");
  concatBuffer[oldestSlot].inUse         = true;
  concatBuffer[oldestSlot].refNumber     = refNumber;
  concatBuffer[oldestSlot].sender        = String(sender);
  concatBuffer[oldestSlot].totalParts    = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text  = "";
  }
  return oldestSlot;
}

// forward declaration
static void processSmsContent(const char* sender, const char* text, const char* timestamp, const MsgTypeInfo& msgType);

// 过滤 pdulib 可能遗留的 UCS-2 对齐填充字节等控制字符（0x00-0x1F，保留 \t \n \r）
// 根因：带空 UDH（UDHL=0）的 SMS 在 UCS-2 内容前需 1 字节对齐，部分运营商填充 0x01（非标）
static String sanitizeSmsText(const char* text) {
  String out;
  for (const char* p = text; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
    out += (char)c;
  }
  return out;
}

// ---------- multipart SMS helpers ----------

static bool hasMultibyte(const char* s) {
  while (*s) {
    if ((unsigned char)*s >= 0x80) return true;
    s++;
  }
  return false;
}

static int countUcs2CharsRaw(const char* s) {
  int count = 0;
  while (*s) {
    unsigned char c = (unsigned char)*s;
    if      (c < 0x80) { s += 1; count += 1; }
    else if (c < 0xE0) { s += 2; count += 1; }
    else if (c < 0xF0) { s += 3; count += 1; }
    else               { s += 4; count += 2; } // emoji 等，UCS-2 代理对
  }
  return count;
}

// 返回 UTF-8 字符串中前 maxUcs2 个 UCS-2 字符所占的字节数
static int ucs2ByteLen(const char* s, int maxUcs2) {
  int count = 0, i = 0;
  while (s[i]) {
    unsigned char c = (unsigned char)s[i];
    int bl, w;
    if      (c < 0x80) { bl = 1; w = 1; }
    else if (c < 0xE0) { bl = 2; w = 1; }
    else if (c < 0xF0) { bl = 3; w = 1; }
    else               { bl = 4; w = 2; }
    if (count + w > maxUcs2) break;
    count += w;
    i += bl;
  }
  return i;
}

// 发送单条 PDU（可携带长短信参数，csms/numParts/partNum 全为 0 表示普通短信）
static bool sendOnePDU(const char* phoneNumber, const char* message, unsigned short csms, unsigned char numParts, unsigned char partNum) {
  pdu.setSCAnumber();
  int pduLen = pdu.encodePDU(phoneNumber, message, csms, numParts, partNum);
  if (pduLen < 0) {
    LOG("SMS", "PDU编码失败，错误码: %d", pduLen);
    return false;
  }
  if (pdu.getOverflow()) {
    LOG("SMS", "PDU编码溢出！内容长度超出 pdulib 限制");
    return false;
  }
  LOG("SMS", "PDU长度=%d，PDU前16字符: %.16s", pduLen, pdu.getSMS());

  String cmgsCmd = "AT+CMGS="; cmgsCmd += pduLen;
  if (!SimDispatcher::pauseReader()) {
    LOG("SMS", "无法暂停 SIM reader，取消发送");
    return false;
  }
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmgsCmd);

  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    esp_task_wdt_reset();
    if (Serial1.available()) {
      char c = Serial1.read();
      if (c == '>') { gotPrompt = true; break; }
    }
  }
  if (!gotPrompt) {
    SimDispatcher::resumeReader();
    LOG("SMS", "未收到>提示符");
    return false;
  }

  // getSMS() 末尾已含 CTRL+Z (0x1A)，直接发送，无需再追加
  Serial1.print(pdu.getSMS());

  start = millis();
  String resp;
  bool cmgsSeen = false;
  unsigned long cmgsSeenAt = 0;

  while (millis() - start < 30000) {
    esp_task_wdt_reset();
    while (Serial1.available()) {
      char c = Serial1.read(); resp += c;
      if (!cmgsSeen && resp.indexOf("+CMGS:") >= 0) {
        cmgsSeen    = true;
        cmgsSeenAt  = millis();
      }
      // OK 是事务完成的最终标志
      if (resp.indexOf("OK") >= 0) {
        SimDispatcher::resumeReader();
        LOG("SMS", "短信发送成功");
        return true;
      }
      // 没有 +CMGS: 就出现 ERROR，才是真正失败
      if (!cmgsSeen && resp.indexOf("ERROR") >= 0) {
        SimDispatcher::resumeReader();
        LOG("SMS", "短信发送失败，响应: %s", resp.c_str());
        return false;
      }
    }
    // +CMGS: 已确认但 2s 内没收到 OK（modem 已入队）→ 视为成功
    if (cmgsSeen && millis() - cmgsSeenAt >= 2000) {
      SimDispatcher::resumeReader();
      LOG("SMS", "短信发送成功（+CMGS已确认）");
      return true;
    }
  }
  SimDispatcher::resumeReader();
  LOG("SMS", "短信发送超时，已收到: %s", resp.c_str());
  return false;
}

bool Sms::sendPDU(const char* phoneNumber, const char* message) {
  LOG("SMS", "准备发送短信到 %s", phoneNumber);

  bool ucs2      = hasMultibyte(message);
  int totalChars = countUcs2CharsRaw(message);
  int singleMax  = ucs2 ? 70  : 160;
  // pdulib 使用 7 字节 UDH（IEI=8，2字节引用号）= 8 septets
  // GSM-7: 160 - 8 = 152；UCS-2: (140 - 7) / 2 = 66
  int partMax    = ucs2 ? 66  : 152;

  if (totalChars <= singleMax) {
    bool ok = sendOnePDU(phoneNumber, message, 0, 0, 0);
    return ok;
  }

  // 长短信：拆分为多段发送
  int numParts = (totalChars + partMax - 1) / partMax;
  static uint8_t csmsRef = 1;
  uint8_t ref = csmsRef++;
  if (csmsRef == 0) csmsRef = 1; // 避免 ref=0（库要求非零）

  LOG("SMS", "长短信，共 %d 段 (ref=%d, 编码=%s)", numParts, ref, ucs2 ? "UCS-2" : "GSM-7");

  const char* ptr = message;
  for (int part = 1; part <= numParts; part++) {
    int byteLen = ucs2ByteLen(ptr, partMax);
    String chunk = String(ptr).substring(0, byteLen);
    LOG("SMS", "发送第 %d/%d 段", part, numParts);
    if (!sendOnePDU(phoneNumber, chunk.c_str(), ref, (uint8_t)numParts, (uint8_t)part)) {
      LOG("SMS", "第 %d 段发送失败，中止", part);
      return false;
    }
    ptr += byteLen;
  }
  return true;
}

// forward declaration
static void processSmsContent(const char* sender, const char* text, const char* timestamp, const MsgTypeInfo& msgType);

static void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text); cmd.trim();
  LOG("SMS", "处理管理员命令: %s", cmd.c_str());

  if (cmd.startsWith("SMS:")) {
    int fc = cmd.indexOf(':');
    int sc = cmd.indexOf(':', fc + 1);
    if (sc > fc + 1) {
      String targetPhone = cmd.substring(fc + 1, sc); targetPhone.trim();
      String smsContent  = cmd.substring(sc + 1);     smsContent.trim();
      bool ok = Sms::sendPDU(targetPhone.c_str(), smsContent.c_str());
      String subject = ok ? "短信发送成功" : "短信发送失败";
      String body = "命令: " + cmd + "\n目标号码: " + targetPhone + "\n结果: " + (ok ? "成功" : "失败");
      LOG("SMS", "%s: %s", subject.c_str(), body.c_str());
    } else {
      LOG("SMS", "SMS命令格式错误");
    }
  } else if (cmd.equals("RESET")) {
    LOG("SMS", "收到RESET命令，即将重启");
    delay(2000);
    ESP.restart();
  } else {
    LOG("SMS", "未知命令: %s", cmd.c_str());
  }
}

// 将 PDU DCS 字节转换为人类可读的短信类型标签
// 参考 GSM 03.38:
//   Data coding / message class group (bits 7-4 = 1111): 0xF0-0xFF，Class 0-3
//   General data coding group (bits 7-6 = 00): bit 4 指示类别位是否有效
static String smsClassLabel(int dcs) {
  // 包含 class 信息的两种分组：0xF0-0xFF（bits7-4=1111）或 0x10-0x3F（bits7-6=00 且 bit4=1）
  bool hasClass = ((dcs & 0xF0) == 0xF0) ||
                  ((dcs & 0xC0) == 0x00 && (dcs & 0x10));
  if (!hasClass) return "普通短信";
  switch (dcs & 0x03) {
    case 0:  return "Class 0（即显短信）";
    case 1:  return "Class 1";
    case 2:  return "Class 2（SIM存储）";
    case 3:  return "Class 3";
    default: return "普通短信";
  }
}

// ---------- WAP Push / 8-bit 数据消息解析 ----------

// 读取 WSP uintvar（续读标志位7，低7位有效），*nc = 消耗字节数
static uint32_t wspReadUintVar(const uint8_t* buf, int len, int* nc) {
  uint32_t val = 0; int i = 0;
  for (; i < 5 && i < len; i++) {
    val = (val << 7) | (buf[i] & 0x7F);
    if (!(buf[i] & 0x80)) { i++; break; }
  }
  if (nc) *nc = i;
  return val;
}

// 读取 MMS Value-length（0-30 直接长度，31=Length-quote+uintvar），*nc = 消耗字节数
static uint32_t mmsReadValueLen(const uint8_t* buf, int len, int* nc) {
  if (len <= 0) { if (nc) *nc = 0; return 0; }
  if (buf[0] == 0x1F) {
    int consumed;
    uint32_t val = wspReadUintVar(buf + 1, len - 1, &consumed);
    if (nc) *nc = 1 + consumed;
    return val;
  }
  if (nc) *nc = 1;
  return buf[0];
}

// 读取 null 结尾的字符串，*nc = 消耗字节数（含 null）
static String wspReadStr(const uint8_t* buf, int len, int* nc) {
  String s; int i = 0;
  for (; i < len && buf[i] != 0; i++) s += (char)buf[i];
  if (i < len) i++;
  if (nc) *nc = i;
  return s;
}

// 扫描字节流，提取所有长度 >= minLen 的连续可打印字符串（ASCII + UTF-8）
// 用于 WBXML 等二进制格式的 best-effort 内容提取
static String wspScanStrings(const uint8_t* buf, int len, int minLen = 4) {
  String result; int i = 0;
  while (i < len) {
    uint8_t c = buf[i];
    if (!((c >= 0x20 && c < 0x7F) || c >= 0xC2)) { i++; continue; }
    String seg; int j = i;
    while (j < len) {
      uint8_t b = buf[j];
      if      (b >= 0x20 && b < 0x7F)                                         { seg += (char)b; j++; }
      else if (b >= 0xC2 && b <= 0xDF && j+1 < len && (buf[j+1]&0xC0)==0x80) { seg += (char)b; seg += (char)buf[j+1]; j += 2; }
      else if (b >= 0xE0 && b <= 0xEF && j+2 < len)                           { seg += (char)b; seg += (char)buf[j+1]; seg += (char)buf[j+2]; j += 3; }
      else break;
    }
    if ((int)seg.length() >= minLen) {
      if (result.length() > 0) result += "\n";
      result += seg;
    }
    i = (j > i) ? j : i + 1;
  }
  return result;
}

// 解析 MMS 通知 PDU (m-notification-ind)，提取 From / Subject / 大小 / 下载地址
static String parseMmsNotificationBody(const uint8_t* body, int len) {
  String from, subject, location;
  int32_t msgSize = -1;
  int p = 0;
  while (p < len) {
    uint8_t field = body[p++];
    if (p >= len) break;
    switch (field) {
      // 固定单字节 short-integer 值字段
      case 0x8C: // X-Mms-Message-Type
      case 0x8E: // X-Mms-MMS-Version
      case 0x86: // X-Mms-Delivery-Report
      case 0x98: // X-Mms-Reply-Charging
        p++;
        break;
      case 0x8D: { // X-Mms-Transaction-Id: text string
        int n; wspReadStr(body + p, len - p, &n); p += n;
        break;
      }
      case 0x83: { // X-Mms-Content-Location: text string (URI)
        int n; location = wspReadStr(body + p, len - p, &n); p += n;
        break;
      }
      case 0x8A: { // X-Mms-Message-Class: short-integer or text
        if (body[p] & 0x80) p++;
        else { int n; wspReadStr(body + p, len - p, &n); p += n; }
        break;
      }
      case 0x8F: { // X-Mms-Message-Size: Long-integer（长度字节 + N 字节大端整数）
        uint8_t numBytes = body[p++];
        if (numBytes <= 8 && p + numBytes <= len) {
          uint32_t sz = 0;
          for (int i = 0; i < numBytes; i++) sz = (sz << 8) | body[p++];
          msgSize = (int32_t)sz;
        } else p += numBytes;
        break;
      }
      case 0x88: { // X-Mms-Expiry: Value-length + token + Long-integer
        int consumed; uint32_t vlen = mmsReadValueLen(body + p, len - p, &consumed);
        p += consumed + (int)vlen;
        break;
      }
      case 0x96: { // Subject: Encoded-string-value
        uint8_t v = body[p];
        if (v > 0 && v <= 0x1F) {
          // Value-length + [charset] + text
          int consumed; uint32_t vlen = mmsReadValueLen(body + p, len - p, &consumed);
          p += consumed;
          int end = p + (int)vlen; if (end > len) end = len;
          // 跳过 charset（short-int: bit7=1; 否则 uintvar）
          if (p < end) {
            if (body[p] & 0x80) p++;
            else { int nc; wspReadUintVar(body + p, end - p, &nc); p += nc; }
          }
          int n; subject = wspReadStr(body + p, end - p, &n);
          p = end;
        } else {
          int n; subject = wspReadStr(body + p, len - p, &n); p += n;
        }
        break;
      }
      case 0x89: { // From: Value-length + (0x80 address-present + Encoded-string-value | 0x81 insert-address)
        int consumed; uint32_t vlen = mmsReadValueLen(body + p, len - p, &consumed);
        p += consumed;
        int end = p + (int)vlen; if (end > len) end = len;
        if (p < end && body[p] == 0x80) { // address-present-token
          p++;
          uint8_t sv = (p < end) ? body[p] : 0;
          if (sv > 0 && sv <= 0x1F) {
            // Value-length + charset + text
            int nc; uint32_t svlen = mmsReadValueLen(body + p, end - p, &nc);
            p += nc;
            int send = p + (int)svlen; if (send > end) send = end;
            if (p < send && (body[p] & 0x80)) p++; // skip short-int charset
            int n; from = wspReadStr(body + p, send - p, &n);
          } else {
            int n; from = wspReadStr(body + p, end - p, &n);
          }
        }
        p = end;
        break;
      }
      default: {
        // 未知字段：尝试跳过（short-int 1字节，否则按文本跳过），失败则停止
        if (body[p] & 0x80) p++;
        else { int n; wspReadStr(body + p, len - p, &n); p += n; }
        break;
      }
    }
  }
  String result;
  if (from.length() > 0)    result += "发件人: " + from + "\n";
  if (subject.length() > 0) result += "主题: " + subject + "\n";
  if (msgSize > 0) {
    char sz[32];
    if (msgSize >= 1024) snprintf(sz, sizeof(sz), "大小: %ldKB\n", (long)(msgSize / 1024));
    else                 snprintf(sz, sizeof(sz), "大小: %ldB\n",  (long)msgSize);
    result += sz;
  }
  if (location.length() > 0) result += "下载地址: " + location;
  result.trim();
  return result.length() > 0 ? result : "【彩信通知，字段解析失败】";
}

// 解析 WSP Push PDU，输出 typeLabel（outLabel）和消息内容（outContent）
// WSP Push: Transaction-ID(1) + PDU-Type(1) + Headers-Length(uintvar) + Content-Type + Headers + Body
static void parseWspMessage(const uint8_t* wsp, int wspLen, String& outLabel, String& outContent) {
  outLabel   = "WAP Push";
  outContent = "【内容无法解析】";
  if (wspLen < 3) return;
  int p = 0;
  p++; // Transaction ID
  uint8_t pduType = wsp[p++];
  if (pduType != 0x06 && pduType != 0x07) {
    char buf[40]; snprintf(buf, sizeof(buf), "WAP Push（PDU类型=0x%02X）", pduType);
    outLabel = buf; return;
  }
  // Headers-Length（uintvar）：Content-Type + Additional-Headers 的总字节数
  int consumed;
  uint32_t headersLen = wspReadUintVar(wsp + p, wspLen - p, &consumed);
  p += consumed;
  int bodyStart = p + (int)headersLen; // body 在所有 headers 之后
  if (p >= wspLen) return;

  // Content-Type 三种编码形式
  uint8_t wellKnown = 0xFF;
  String  typeStr;
  uint8_t ct0 = wsp[p];
  if (ct0 & 0x80) {
    wellKnown = ct0 & 0x7F; p++;
  } else if (ct0 > 0 && ct0 <= 0x1F) {
    uint32_t vLen = ct0;
    if (ct0 == 0x1F) { p++; vLen = wspReadUintVar(wsp+p, wspLen-p, &consumed); p += consumed; }
    else              { p++; }
    if (p < wspLen) {
      uint8_t mt = wsp[p];
      if (mt & 0x80) { wellKnown = mt & 0x7F; }
      else           { int n; typeStr = wspReadStr(wsp + p, (int)vLen, &n); }
    }
  } else {
    int n; typeStr = wspReadStr(wsp + p, wspLen - p, &n);
  }

  if (bodyStart > wspLen) bodyStart = wspLen;
  const uint8_t* body = wsp + bodyStart;
  int bodyLen = wspLen - bodyStart;

  if (wellKnown != 0xFF) {
    switch (wellKnown) {
      case 0x02: // text/html
        outLabel   = "WAP Push：HTML";
        outContent = wspScanStrings(body, bodyLen);
        if (outContent.length() == 0) outContent = "【HTML内容，无法提取文本】";
        break;
      case 0x03: { // text/plain
        outLabel = "WAP Push：文本";
        String s; for (int i = 0; i < bodyLen && body[i] != 0; i++) s += (char)body[i];
        outContent = s.length() > 0 ? s : "【空文本内容】";
        break;
      }
      case 0x2E: // text/vnd.wap.si（服务指示，WBXML）
        outLabel   = "WAP Push：服务指示（SI）";
        outContent = wspScanStrings(body, bodyLen);
        if (outContent.length() == 0) outContent = "【SI消息，内容无法提取】";
        break;
      case 0x2F: // text/vnd.wap.sl（服务加载，WBXML）
        outLabel   = "WAP Push：服务加载（SL）";
        outContent = wspScanStrings(body, bodyLen);
        if (outContent.length() == 0) outContent = "【SL消息，URL无法提取】";
        break;
      case 0x35: // application/vnd.wap.connectivity-wbxml（OMA CP）
        outLabel   = "WAP Push：设备配置（OMA CP）";
        outContent = "【运营商推送网络配置，内容为二进制格式】";
        break;
      case 0x3E: // application/vnd.wap.mms-message（彩信通知）
        outLabel   = "WAP Push：彩信通知";
        outContent = parseMmsNotificationBody(body, bodyLen);
        break;
      case 0x44: // application/vnd.syncml.notification
        outLabel   = "WAP Push：SyncML通知";
        outContent = "【SyncML同步通知，内容为二进制格式】";
        break;
      default: {
        char buf[48]; snprintf(buf, sizeof(buf), "WAP Push（well-known=0x%02X）", wellKnown);
        outLabel   = buf;
        outContent = wspScanStrings(body, bodyLen);
        if (outContent.length() == 0) outContent = "【内容无法解析】";
        break;
      }
    }
  } else {
    outLabel   = typeStr.length() > 0 ? ("WAP Push：" + typeStr) : "WAP Push（未知类型）";
    outContent = wspScanStrings(body, bodyLen);
    if (outContent.length() == 0) {
      char buf[64]; snprintf(buf, sizeof(buf), "【%s，内容无法解析】", typeStr.length() > 0 ? typeStr.c_str() : "未知类型");
      outContent = buf;
    }
  }
}

// 从 SMS-DELIVER PDU hex 串中提取 8-bit UD 载荷和 UDH 目标端口
// SMS-DELIVER 格式: SCA + PDU-TYPE + OA + PID + DCS + SCTS(7) + UDL + UD
// 返回 false 表示 PDU 结构解析失败
static bool extractRawUd8bit(const String& hexPdu, uint8_t* udBuf, int& udLen, uint16_t& destPort) {
  destPort = 0;
  udLen    = 0;
  int hexLen = hexPdu.length();
  if (hexLen < 4 || (hexLen & 1) != 0) return false;
  int binLen = hexLen / 2;
  if (binLen > 200) return false;

  uint8_t bin[200];
  for (int i = 0; i < binLen; i++) {
    auto h = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return 0;
    };
    bin[i] = (h(hexPdu.charAt(i * 2)) << 4) | h(hexPdu.charAt(i * 2 + 1));
  }

  int p = 0;
  // SCA（Service Centre Address）：长度字节 + SCA 内容
  if (p >= binLen) return false;
  p += 1 + bin[p];
  // TP-PDU-TYPE：bit 6 = UDHI（User Data Header Indicator）
  if (p >= binLen) return false;
  bool udhi = (bin[p++] >> 6) & 1;
  // Originating Address：长度（半字节数）+ TON/NPI(1) + 编码字节
  if (p >= binLen) return false;
  int oa_nibbles = bin[p++];
  p += 1 + (oa_nibbles + 1) / 2;
  // PID(1) + DCS(1)（已由 pdulib 获取，直接跳过）
  p += 2;
  // SCTS（时间戳，固定 7 字节）
  p += 7;
  // UDL（8-bit 模式下为字节数）
  if (p >= binLen) return false;
  int udl = bin[p++];
  if (p + udl > binLen) return false;

  int bodyStart = p;
  int bodyLen   = udl;

  if (udhi && udl > 0) {
    if (p >= binLen) return false;
    int udhl   = bin[p];           // UDH 内容长度（不含自身）
    if (1 + udhl > udl) return false;
    int udhEnd = p + 1 + udhl;
    if (udhEnd > binLen) return false;
    // 遍历 IEI 记录，查找 Application Port Addressing
    int q = p + 1;
    while (q + 1 < udhEnd && q + 1 < binLen) {
      uint8_t iei     = bin[q++];
      uint8_t iei_len = bin[q++];
      if (q + iei_len > binLen) break;
      if (iei == 0x05 && iei_len >= 4) {
        // 16-bit Application Port: dest_hi, dest_lo, src_hi, src_lo
        destPort = ((uint16_t)bin[q] << 8) | bin[q + 1];
      } else if (iei == 0x04 && iei_len >= 2) {
        // 8-bit Application Port: dest, src
        destPort = bin[q];
      }
      q += iei_len;
    }
    bodyStart = udhEnd;
    bodyLen   = udl - (1 + udhl);
    if (bodyLen < 0) bodyLen = 0;
  }

  int copyLen = bodyLen < 160 ? bodyLen : 160;
  if (bodyStart < 0 || bodyStart + copyLen > binLen) return false;
  memcpy(udBuf, bin + bodyStart, copyLen);
  udLen = copyLen;
  return true;
}

// 处理 8-bit 编码的 SMS（WAP Push / 应用端口短信 / 其他数据消息）
static void handleRawDataSms(const String& hexPdu, int dcs, const char* sender, const char* timestamp) {
  String classInfo;
  if (((dcs & 0xF0) == 0xF0 || ((dcs & 0xC0) == 0x00 && (dcs & 0x10))) && (dcs & 0x03) == 0)
    classInfo = "Class 0（即显短信）";

  uint8_t  udBuf[160];
  int      udLen    = 0;
  uint16_t destPort = 0;

  if (!extractRawUd8bit(hexPdu, udBuf, udLen, destPort)) {
    LOG("SMS", "8-bit PDU 手动解析失败 (DCS=0x%02X)", dcs);
    String label = classInfo.length() > 0 ? (classInfo + "·数据消息") : "数据消息";
    processSmsContent(sender, "【内容无法解析】", timestamp, MsgTypeInfo(MSG_TYPE_SMS, label));
    return;
  }

  if (destPort == 2948 || destPort == 2949) {
    // WAP Push 标准端口：2948(0x0B84) / 2949(0x0B85)
    String wapLabel, wapContent;
    parseWspMessage(udBuf, udLen, wapLabel, wapContent);
    String label = classInfo.length() > 0 ? (wapLabel + "（" + classInfo + "）") : wapLabel;
    LOG("SMS", "WAP Push 端口=%u 类型=%s", destPort, label.c_str());
    processSmsContent(sender, wapContent.c_str(), timestamp, MsgTypeInfo(MSG_TYPE_SMS, label));
  } else if (destPort != 0) {
    char buf[48]; snprintf(buf, sizeof(buf), "应用端口消息（端口=%u）", destPort);
    String label = classInfo.length() > 0 ? (classInfo + "·" + String(buf)) : String(buf);
    String content = wspScanStrings(udBuf, udLen);
    if (content.length() == 0) content = "【二进制内容，无法显示】";
    LOG("SMS", "应用端口短信 端口=%u DCS=0x%02X", destPort, dcs);
    processSmsContent(sender, content.c_str(), timestamp, MsgTypeInfo(MSG_TYPE_SMS, label));
  } else {
    String label = classInfo.length() > 0 ? (classInfo + "·8-bit数据") : "8-bit数据消息";
    String content = wspScanStrings(udBuf, udLen);
    if (content.length() == 0) content = "【二进制内容，无法显示】";
    LOG("SMS", "8-bit 数据消息（无端口）DCS=0x%02X", dcs);
    processSmsContent(sender, content.c_str(), timestamp, MsgTypeInfo(MSG_TYPE_SMS, label));
  }
}

static void processSmsContent(const char* sender, const char* text, const char* timestamp, const MsgTypeInfo& msgType) {
  LOG("SMS", "=== 处理短信内容 ===");
  LOG("SMS", "发送者: %s", sender);
  LOG("SMS", "时间戳: %s", timestamp);
  LOG("SMS", "内容: %s", text);

  // 黑名单检查
  if (phoneMatchesBlacklist(String(sender))) {
    LOG("SMS", "黑名单拦截短信，号码: %s", sender);
    return;
  }

  if (isAdmin(sender)) {
    String smsText = String(text); smsText.trim();
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      return;
    }
  }

  Push::send(String(sender), String(text), String(timestamp), msgType);
}

static bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

static void processPduLine(const String& line) {
  if (!isHexString(line)) {
    LOG("SMS", "收到非PDU数据，忽略");
    return;
  }

  LOG("SMS", "收到PDU数据: %s", line.c_str());

  if (!pdu.decodePDU(line.c_str())) {
    LOG("SMS", "PDU解析失败！");
    return;
  }

  LOG("SMS", "PDU解析成功: 发送者=%s 时间=%s 内容=%s",
          pdu.getSender(), pdu.getTimeStamp(), pdu.getText());

  String classLabel = smsClassLabel(pdu.getDCS());
  LOG("SMS", "短信类型: %s (DCS=0x%02X)", classLabel.c_str(), pdu.getDCS());

  // 8-bit data encoding → WAP Push / 应用端口短信 / 其他数据消息
  // pdulib 对 8-bit 载荷无法正确解码，绕过文本解码路径单独处理
  if ((pdu.getDCS() & DCS_ALPHABET_MASK) == DCS_8BIT_ALPHABET_MASK) {
    handleRawDataSms(line, pdu.getDCS(), pdu.getSender(), pdu.getTimeStamp());
    return;
  }

  int* concatInfo = pdu.getConcatInfo();
  int refNumber   = concatInfo[0];
  int partNumber  = concatInfo[1];
  int totalParts  = concatInfo[2];

  LOG("SMS", "长短信信息: 参考号=%d 当前=%d 总计=%d", refNumber, partNumber, totalParts);

  if (totalParts > 1 && partNumber > 0) {
    if (totalParts > MAX_CONCAT_PARTS) {
      LOG("SMS", "长短信总段数 %d 超过缓存上限 %d，丢弃", totalParts, MAX_CONCAT_PARTS);
      return;
    }
    LOG("SMS", "收到长短信分段 %d/%d", partNumber, totalParts);
    int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
    int partIndex = partNumber - 1;
    if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
      if (!concatBuffer[slot].parts[partIndex].valid) {
        concatBuffer[slot].parts[partIndex].valid = true;
        concatBuffer[slot].parts[partIndex].text  = sanitizeSmsText(pdu.getText());
        concatBuffer[slot].receivedParts++;
        if (concatBuffer[slot].receivedParts == 1) {
          concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
        }
        LOG("SMS", "已缓存分段 %d，当前已收到 %d/%d", partNumber, concatBuffer[slot].receivedParts, totalParts);
      } else {
        LOG("SMS", "分段 %d 已存在，跳过", partNumber);
      }
    }
    if (concatBuffer[slot].receivedParts >= totalParts) {
      LOG("SMS", "长短信已收齐，开始合并转发");
      String fullText = assembleConcatSms(slot);
      processSmsContent(concatBuffer[slot].sender.c_str(),
                        fullText.c_str(),
                        concatBuffer[slot].timestamp.c_str(),
                        MsgTypeInfo(MSG_TYPE_SMS, classLabel));
      clearConcatSlot(slot);
    }
  } else {
    String smsText = sanitizeSmsText(pdu.getText());
    processSmsContent(pdu.getSender(), smsText.c_str(), pdu.getTimeStamp(), MsgTypeInfo(MSG_TYPE_SMS, classLabel));
  }
}

// ---------- public API ----------

void Sms::initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse         = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text  = "";
    }
  }
}

void Sms::handleCMTHeader() {
  LOG("SMS", "检测到+CMT，等待PDU数据...");
}

// 解析并推送 USSD 消息（在 sms_proc 任务上下文执行，不可在 reader task 调用）。
static void processUssdLine(const String& line) {
  LOG("SMS", "处理 USSD 消息: %s", line.c_str());

  // 格式: +CUSD: <n>,"<str>",<dcs>  或  +CUSD: <n>
  int colon = line.indexOf(':');
  if (colon < 0) return;
  String payload = line.substring(colon + 1);
  payload.trim();

  // 提取 n（第一个参数）
  int n = -1;
  if (payload.length() > 0 && payload.charAt(0) >= '0' && payload.charAt(0) <= '9') {
    n = payload.charAt(0) - '0';
  }

  // 提取消息内容字符串（引号内）
  String content;
  int q1 = payload.indexOf('"');
  int q2 = (q1 >= 0) ? payload.indexOf('"', q1 + 1) : -1;
  if (q1 >= 0 && q2 > q1) {
    content = payload.substring(q1 + 1, q2);
  }

  // 提取 dcs（第三个参数）
  int dcs = -1;
  if (q2 > 0) {
    int comma = payload.indexOf(',', q2 + 1);
    if (comma >= 0) {
      String dcsStr = payload.substring(comma + 1);
      dcsStr.trim();
      dcs = dcsStr.toInt();
    }
  }

  // UCS-2 编码（dcs=72 或 dcs=8）：hex 串转 UTF-8
  if ((dcs == 72 || dcs == 8) && content.length() > 0 && (content.length() % 4) == 0) {
    String decoded;
    bool isHex = true;
    for (unsigned int i = 0; i < content.length(); i++) {
      char c = content.charAt(i);
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
        isHex = false; break;
      }
    }
    if (isHex) {
      // 将 4 字符 hex 段解码为 UCS-2，再转 UTF-8
      for (unsigned int i = 0; i + 3 < content.length(); i += 4) {
        auto h = [](char c) -> uint16_t {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          return 0;
        };
        uint16_t cp = (h(content.charAt(i)) << 12) | (h(content.charAt(i+1)) << 8)
                    | (h(content.charAt(i+2)) <<  4) |  h(content.charAt(i+3));
        if (cp < 0x0080) {
          decoded += (char)cp;
        } else if (cp < 0x0800) {
          decoded += (char)(0xC0 | (cp >> 6));
          decoded += (char)(0x80 | (cp & 0x3F));
        } else {
          decoded += (char)(0xE0 | (cp >> 12));
          decoded += (char)(0x80 | ((cp >> 6) & 0x3F));
          decoded += (char)(0x80 | (cp & 0x3F));
        }
      }
      content = decoded;
    }
  }

  String statusLabel;
  switch (n) {
    case 0: statusLabel = "USSD"; break;
    case 1: statusLabel = "USSD（需要回复）"; break;
    case 2: statusLabel = "USSD（会话已终止）"; break;
    default: statusLabel = "USSD"; break;
  }

  if (content.length() == 0) {
    LOG("SMS", "USSD 无消息内容，n=%d", n);
    return;
  }

  processSmsContent("运营商", content.c_str(), "", MsgTypeInfo(MSG_TYPE_SMS, statusLabel));
}

// +CUSD: <n>[,<str>[,<dcs>]] —— URC 入口（在 SIM reader task 上下文）
// 关键约束：本函数不可同步发起推送（HTTPS 阻塞 5–30s 会让 reader task
// 错过后续 +CMT/RING URC，且黑名单检查可能导致死锁）。仅入队，立即返回。
void Sms::handleUSSD(const String& line) {
  if (s_pduQueue == nullptr) {
    LOG("SMS", "USSD 到达但 sms_proc 未启动，丢弃");
    return;
  }
  if (line.length() > PDU_MAX_LEN) {
    LOG("SMS", "USSD 行过长（%u），丢弃", (unsigned)line.length());
    return;
  }
  PduQueueItem* item = new PduQueueItem();
  if (!item) { LOG("SMS", "USSD 队列项分配失败"); return; }
  item->kind = SmsItemKind::USSD;
  strncpy(item->data, line.c_str(), PDU_MAX_LEN);
  item->data[PDU_MAX_LEN] = '\0';
  if (xQueueSend(s_pduQueue, &item, 0) != pdTRUE) {
    delete item;
    LOG("SMS", "SMS 队列已满，丢弃 USSD");
  }
}

void Sms::handlePDU(const String& line) {
  // 仅入队，立即返回，不在 sim_reader 任务栈上执行任何解码逻辑
  if (s_pduQueue == nullptr) return;
  if (line.length() > PDU_MAX_LEN) {
    LOG("SMS", "PDU 数据行过长（%u），丢弃", (unsigned)line.length());
    return;
  }
  PduQueueItem* item = new PduQueueItem();
  if (!item) { LOG("SMS", "PDU 队列项分配失败"); return; }
  item->kind = SmsItemKind::PDU;
  strncpy(item->data, line.c_str(), PDU_MAX_LEN);
  item->data[PDU_MAX_LEN] = '\0';
  BaseType_t sent = xQueueSend(s_pduQueue, &item, 0);
  if (sent != pdTRUE) {
    delete item;
    LOG("SMS", "PDU 队列已满，丢弃");
  }
}

// ---------- sms_proc 任务 ----------

static void smsProcTask(void*) {
  for (;;) {
    PduQueueItem* item = nullptr;
    if (xQueueReceive(s_pduQueue, &item, portMAX_DELAY) == pdTRUE && item != nullptr) {
      if (item->kind == SmsItemKind::USSD) {
        processUssdLine(String(item->data));
      } else {
        processPduLine(String(item->data));
      }
      delete item;
    }
    esp_task_wdt_reset();
  }
}

void Sms::startProcTask() {
  s_pduQueue = xQueueCreate(SMS_QUEUE_DEPTH, sizeof(PduQueueItem*));
  if (!s_pduQueue) {
    LOG("SMS", "PDU 队列创建失败");
    return;
  }
  xTaskCreate(smsProcTask, "sms_proc", SMS_PROC_TASK_STACK, nullptr, SMS_PROC_TASK_PRIO, nullptr);
}

void Sms::checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS)) {
      LOG("SMS", "[告警] 长短信超时，丢弃不完整消息（参考号=%d，已收到=%d/%d）", concatBuffer[i].refNumber, concatBuffer[i].receivedParts, concatBuffer[i].totalParts);
      clearConcatSlot(i);
    }
  }
}

#include "SmsReceiver.h"
#include "SmsSender.h"        // also brings in pdulib.h + PDU pdu declaration
#include "config/AppConfig.h"
#include "push/PushManager.h"
#include "email/EmailNotifier.h"
#include "sim/SimManager.h"
#include "utils/Utils.h"
#include <time.h>
// NOTE: do NOT include <pdulib.h> directly here — SmsSender.h already pulls it in,
//       and pdulib lacks include guards, causing struct redefinition errors.

extern PDU pdu;  // defined in SmsSender.cpp

// ── Long-SMS concat buffer ────────────────────────────────────────────────────
struct SmsPart {
  bool   valid;
  String text;
};
struct ConcatSms {
  bool          inUse;
  int           refNumber;
  String        sender;
  String        timestamp;
  int           totalParts;
  int           receivedParts;
  unsigned long firstPartTime;
  SmsPart       parts[MAX_CONCAT_PARTS];
};
static ConcatSms concatBuf[MAX_CONCAT_MESSAGES];

// ── Buffer helpers ────────────────────────────────────────────────────────────
static void clearSlot(int i) {
  concatBuf[i].inUse        = false;
  concatBuf[i].receivedParts = 0;
  concatBuf[i].sender       = "";
  concatBuf[i].timestamp    = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuf[i].parts[j].valid = false;
    concatBuf[i].parts[j].text  = "";
  }
}

static int findOrCreateSlot(int ref, const char* sndr, int total) {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++)
    if (concatBuf[i].inUse && concatBuf[i].refNumber == ref
        && concatBuf[i].sender.equals(sndr)) return i;
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuf[i].inUse) {
      clearSlot(i);
      concatBuf[i].inUse         = true;
      concatBuf[i].refNumber     = ref;
      concatBuf[i].sender        = String(sndr);
      concatBuf[i].totalParts    = total;
      concatBuf[i].firstPartTime = millis();
      return i;
    }
  }
  // evict oldest
  int old = 0;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++)
    if (concatBuf[i].firstPartTime < concatBuf[old].firstPartTime) old = i;
  Serial.println("[SmsRx] ⚠️ 长短信缓存已满，覆盖最老槽位");
  clearSlot(old);
  concatBuf[old].inUse         = true;
  concatBuf[old].refNumber     = ref;
  concatBuf[old].sender        = String(sndr);
  concatBuf[old].totalParts    = total;
  concatBuf[old].firstPartTime = millis();
  return old;
}

static String assembleSlot(int i) {
  String r;
  for (int j = 0; j < concatBuf[i].totalParts; j++) {
    r += concatBuf[i].parts[j].valid
         ? concatBuf[i].parts[j].text
         : ("[缺失分段" + String(j + 1) + "]");
  }
  return r;
}

// ── Admin command processing ──────────────────────────────────────────────────
static bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  String s = String(sender);
  String a = config.adminPhone;
  if (s.startsWith("+86")) s = s.substring(3);
  if (a.startsWith("+86")) a = a.substring(3);
  return s.equals(a);
}

static void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  Serial.println("[SmsRx] 管理员命令: " + cmd);

  if (cmd.startsWith("SMS:")) {
    int c1 = cmd.indexOf(':');
    int c2 = cmd.indexOf(':', c1 + 1);
    if (c2 > c1 + 1) {
      String phone   = cmd.substring(c1 + 1, c2); phone.trim();
      String content = cmd.substring(c2 + 1);     content.trim();
      bool ok = smsSend(phone.c_str(), content.c_str());
      String body = "命令: " + cmd + "\n目标: " + phone + "\n结果: " + (ok ? "成功" : "失败");
      emailNotify(ok ? "短信发送成功" : "短信发送失败", body.c_str());
    } else {
      emailNotify("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  } else if (cmd.equals("RESET")) {
    emailNotify("重启命令已执行", "收到RESET命令，即将重启设备...");
    resetModule();
    Serial.println("[SmsRx] 重启ESP32...");
    delay(1000);
    ESP.restart();
  } else {
    Serial.println("[SmsRx] 未知命令: " + cmd);
  }
}

// ── Final SMS processing ──────────────────────────────────────────────────────
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  Serial.println("[SmsRx] === 短信内容 ===");
  Serial.println("  发送者: " + String(sender));
  Serial.println("  时间戳: " + String(timestamp));
  Serial.println("  内容:   " + String(text));

  if (isInNumberBlackList(sender)) {
    Serial.println("[SmsRx] 🚫 发送者在黑名单，忽略");
    return;
  }

  if (isAdmin(sender)) {
    String t = String(text); t.trim();
    if (t.startsWith("SMS:") || t.equals("RESET")) {
      processAdminCommand(sender, text);
      return;
    }
  }

  pushAll(sender, text, timestamp);
  String subject = String("短信") + sender + "," + text;
  String body    = String("来自：") + sender + "，时间：" + timestamp + "，内容：" + text;
  emailNotify(subject.c_str(), body.c_str());
}

// ── Incoming call processing ──────────────────────────────────────────────────
static unsigned long lastCallNotifyTime = 0;
static String        lastCallNumber     = "";

void processIncomingCall(const char* caller) {
  // Build timestamp string from local time (NTP, UTC+8)
  String timeStr = "未知时间";
  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[20]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    timeStr = String(buf);
  }

  Serial.println("[SmsRx] === 来电通知 ===");
  Serial.println("  来电号码: " + String(caller));
  Serial.println("  时间:     " + timeStr);

  pushCallAll(caller, timeStr.c_str());

  String fmtCaller = formatPhoneNumber(caller);
  String subject   = "来电提醒: " + fmtCaller;
  String body      = "收到来电\n来电号码: " + fmtCaller + "\n时间: " + timeStr;
  emailNotify(subject.c_str(), body.c_str());
}

// ── Serial line reader ────────────────────────────────────────────────────────
static String readSerialLine() {
  static char buf[SERIAL_BUFFER_SIZE];
  static int  pos = 0;
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      buf[pos] = 0;
      String r = String(buf);
      pos = 0;
      return r;
    } else if (c != '\r') {
      if (pos < SERIAL_BUFFER_SIZE - 1) buf[pos++] = c;
      else pos = 0;
    }
  }
  return "";
}

static bool isHexString(const String& s) {
  if (s.length() == 0) return false;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

// ── URC + PDU parser ──────────────────────────────────────────────────────────
static enum { IDLE, WAIT_PDU } urcState = IDLE;

static void checkURC() {
  String line = readSerialLine();
  if (line.length() == 0) return;
  Serial.println("Debug> " + line);

  if (urcState == IDLE) {
    if (line.startsWith("+CMT:")) {
      urcState = WAIT_PDU;
    }
    // ── Incoming call URC: +CLCC: idx,dir,4(ringing),mode,mpty,"number",type,""
    else if (line.startsWith("+CLCC:") && line.indexOf(",4,") >= 0) {
      Serial.println("[SmsRx] 检测到来电: " + line);
      // Parse caller number from the first quoted field
      int q1 = line.indexOf('"');
      int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
      String callerNumber = "未知号码";
      if (q1 >= 0 && q2 > q1) {
        callerNumber = line.substring(q1 + 1, q2);
        // type=145 means international format (add + prefix if missing)
        String afterQ = line.substring(q2 + 1);
        int comma = afterQ.indexOf(',');
        String typeStr = (comma >= 0) ? afterQ.substring(1, comma) : afterQ.substring(1);
        typeStr.trim();
        if (typeStr.toInt() == 145 && !callerNumber.startsWith("+"))
          callerNumber = "+" + callerNumber;
      }
      // Dedup: same number within 30 s → skip
      unsigned long now = millis();
      if (callerNumber != lastCallNumber || (now - lastCallNotifyTime) >= CALL_NOTIFY_INTERVAL_MS) {
        lastCallNumber     = callerNumber;
        lastCallNotifyTime = now;
        processIncomingCall(callerNumber.c_str());
      } else {
        Serial.println("[SmsRx] 来电防重复，跳过");
      }
    }
    else if (line.indexOf("+CPIN:") >= 0) {
      bool ready = (line.indexOf("READY") >= 0 && line.indexOf("NOT") < 0);
      if (ready && !simInitialized) {
        Serial.println("[SmsRx] URC SIM就绪，触发初始化");
        simPresent = true;
        devicePhoneNumber = "未知号码";
        delay(500);
        initSIMDependent();
      } else if (!ready && simPresent) {
        Serial.println("[SmsRx] URC SIM不可用");
        simPresent = false; simInitialized = false;
        devicePhoneNumber = "未知号码";
      }
    } else if (line.startsWith("^SIMST:") || line.startsWith("+SIMCARD:")) {
      bool ins = (line.indexOf(":1") >= 0 || line.indexOf(": 1") >= 0);
      if (ins && !simInitialized) {
        Serial.println("[SmsRx] URC SIM插入");
        simPresent = true; devicePhoneNumber = "未知号码";
        delay(500); initSIMDependent();
      } else if (!ins && simPresent) {
        Serial.println("[SmsRx] URC SIM拔出");
        simPresent = false; simInitialized = false;
        devicePhoneNumber = "未知号码";
      }
    }
  } else { // WAIT_PDU
    if (line.length() == 0) return;
    if (!isHexString(line)) {
      Serial.println("[SmsRx] 非PDU数据，返回IDLE");
      urcState = IDLE;
      return;
    }
    Serial.println("[SmsRx] PDU数据: " + line);
    if (!pdu.decodePDU(line.c_str())) {
      Serial.println("[SmsRx] ❌ PDU解析失败");
    } else {
      int* ci       = pdu.getConcatInfo();
      int  ref      = ci[0], part = ci[1], total = ci[2];
      Serial.printf("[SmsRx] 长短信: ref=%d part=%d/%d\n", ref, part, total);

      if (total > 1 && part > 0) {
        int slot = findOrCreateSlot(ref, pdu.getSender(), total);
        int idx  = part - 1;
        if (idx >= 0 && idx < MAX_CONCAT_PARTS && !concatBuf[slot].parts[idx].valid) {
          concatBuf[slot].parts[idx].valid = true;
          concatBuf[slot].parts[idx].text  = String(pdu.getText());
          concatBuf[slot].receivedParts++;
          if (concatBuf[slot].receivedParts == 1)
            concatBuf[slot].timestamp = String(pdu.getTimeStamp());
          Serial.printf("[SmsRx]   缓存分段%d，已收%d/%d\n", part, concatBuf[slot].receivedParts, total);
        }
        if (concatBuf[slot].receivedParts >= total) {
          Serial.println("[SmsRx] ✅ 长短信已收齐");
          String full = assembleSlot(slot);
          processSmsContent(concatBuf[slot].sender.c_str(), full.c_str(), concatBuf[slot].timestamp.c_str());
          clearSlot(slot);
        }
      } else {
        processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
      }
    }
    urcState = IDLE;
  }
}

// ── Timeout check ─────────────────────────────────────────────────────────────
static void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuf[i].inUse && now - concatBuf[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
      Serial.println("[SmsRx] ⏰ 长短信超时，强制转发");
      String full = assembleSlot(i);
      processSmsContent(concatBuf[i].sender.c_str(), full.c_str(), concatBuf[i].timestamp.c_str());
      clearSlot(i);
    }
  }
}

// ── Public API ────────────────────────────────────────────────────────────────
void smsReceiverInit() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) clearSlot(i);
}

void smsReceiverTick() {
  checkURC();
  checkConcatTimeout();
  // Pass-through: host serial → modem
  if (Serial.available()) Serial1.write(Serial.read());
}


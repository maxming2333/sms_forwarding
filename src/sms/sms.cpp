#include "sms.h"
#include "config/config.h"
#include "push/push.h"
#include "logger.h"
#include "phone_utils.h"
#include "sim/sim_dispatcher.h"
#include <pdulib.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// ---------- SMS 处理任务配置 ----------

// PDU 队列深度：SIM 模组 CNMI=2,2 模式下 AT URIs 每条短信最多缓冲 5 段，
// 队列容量 16 足以应对突发的多条长短信
static constexpr int  SMS_QUEUE_DEPTH      = 16;
// sms_proc 任务栈：包含 pdulib decode（gsm7bit[160]），多次 LOG（char msg[256] 各一帧），
// 以及 assembleConcatSms/sendPushNotification 等，分配 8192 字节留足余量
static constexpr int  SMS_PROC_TASK_STACK  = 8192;
static constexpr int  SMS_PROC_TASK_PRIO   = 2;

// PDU 字符串最大长度（SIM 模组单条 PDU hex 串最长 ~340 字符）
static constexpr int  PDU_MAX_LEN          = 400;

struct PduQueueItem {
    char pdu[PDU_MAX_LEN + 1];
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
  simPauseReader();
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
    simResumeReader();
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
        simResumeReader();
        LOG("SMS", "短信发送成功");
        return true;
      }
      // 没有 +CMGS: 就出现 ERROR，才是真正失败
      if (!cmgsSeen && resp.indexOf("ERROR") >= 0) {
        simResumeReader();
        LOG("SMS", "短信发送失败，响应: %s", resp.c_str());
        return false;
      }
    }
    // +CMGS: 已确认但 2s 内没收到 OK（modem 已入队）→ 视为成功
    if (cmgsSeen && millis() - cmgsSeenAt >= 2000) {
      simResumeReader();
      LOG("SMS", "短信发送成功（+CMGS已确认）");
      return true;
    }
  }
  simResumeReader();
  LOG("SMS", "短信发送超时，已收到: %s", resp.c_str());
  return false;
}

bool sendSMSPDU(const char* phoneNumber, const char* message) {
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
      bool ok = sendSMSPDU(targetPhone.c_str(), smsContent.c_str());
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
// 参考 GSM 03.38 §4: General data coding group (bits 7-6 = 00)
// bit 4 (0x10): 1 = 消息类型位有意义; bits 1-0: 消息类别
static String smsClassLabel(int dcs) {
  // 仅处理 General data coding group (bits 7-6 = 00)
  if ((dcs & 0xC0) != 0x00) return "普通短信";
  // bit 4 未置位表示无类别信息
  if (!(dcs & 0x10)) return "普通短信";
  switch (dcs & 0x03) {
    case 0: return "Class 0（即显短信）";
    case 1: return "Class 1";
    case 2: return "Class 2（SIM存储）";
    case 3: return "Class 3";
    default: return "普通短信";
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

  sendPushNotification(String(sender), String(text), String(timestamp), msgType);
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

  int* concatInfo = pdu.getConcatInfo();
  int refNumber   = concatInfo[0];
  int partNumber  = concatInfo[1];
  int totalParts  = concatInfo[2];

  LOG("SMS", "长短信信息: 参考号=%d 当前=%d 总计=%d", refNumber, partNumber, totalParts);

  if (totalParts > 1 && partNumber > 0) {
    LOG("SMS", "收到长短信分段 %d/%d", partNumber, totalParts);
    int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
    int partIndex = partNumber - 1;
    if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
      if (!concatBuffer[slot].parts[partIndex].valid) {
        concatBuffer[slot].parts[partIndex].valid = true;
        concatBuffer[slot].parts[partIndex].text  = String(pdu.getText());
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
                        makeMsgType(MSG_TYPE_SMS, classLabel));
      clearConcatSlot(slot);
    }
  } else {
    processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp(), MsgTypeInfo(MSG_TYPE_SMS, classLabel));
  }
}

// ---------- public API ----------

void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse         = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text  = "";
    }
  }
}

void smsHandleCMTHeader() {
  LOG("SMS", "检测到+CMT，等待PDU数据...");
}

void smsHandlePDU(const String& line) {
  // 仅入队，立即返回，不在 sim_reader 任务栈上执行任何解码逻辑
  if (s_pduQueue == nullptr) return;
  if (line.length() > PDU_MAX_LEN) {
    LOG("SMS", "PDU 数据行过长（%u），丢弃", (unsigned)line.length());
    return;
  }
  PduQueueItem* item = new PduQueueItem();
  if (!item) { LOG("SMS", "PDU 队列项分配失败"); return; }
  strncpy(item->pdu, line.c_str(), PDU_MAX_LEN);
  item->pdu[PDU_MAX_LEN] = '\0';
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
      processPduLine(String(item->pdu));
      delete item;
    }
    esp_task_wdt_reset();
  }
}

void smsStartProcTask() {
  s_pduQueue = xQueueCreate(SMS_QUEUE_DEPTH, sizeof(PduQueueItem*));
  if (!s_pduQueue) {
    LOG("SMS", "PDU 队列创建失败");
    return;
  }
  xTaskCreate(smsProcTask, "sms_proc", SMS_PROC_TASK_STACK,
              nullptr, SMS_PROC_TASK_PRIO, nullptr);
}

void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS)) {
      LOG("SMS", "[告警] 长短信超时，丢弃不完整消息（参考号=%d，已收到=%d/%d）",
              concatBuffer[i].refNumber, concatBuffer[i].receivedParts, concatBuffer[i].totalParts);
      clearConcatSlot(i);
    }
  }
}

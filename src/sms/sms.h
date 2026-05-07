#pragma once
#include <Arduino.h>

// 拼接短信参数
constexpr int          MAX_CONCAT_PARTS    = 10;        // 单条拼接短信最多多少分片
constexpr unsigned long CONCAT_TIMEOUT_MS  = 300000;    // 拼接超时（5 分钟），过期则丢弃未收齐的分片
constexpr int          MAX_CONCAT_MESSAGES = 5;         // 同时进行中的拼接短信数量上限

// ---------- SMS 处理任务配置 ----------

// PDU 队列深度：SIM 模组 CNMI=2,2 模式下 AT URIs 每条短信最多缓冲 5 段，
// 队列容量 16 足以应对突发的多条长短信
constexpr int  SMS_QUEUE_DEPTH      = 16;
// sms_proc 任务栈：包含 pdulib decode（gsm7bit[160]），多次 LOG（char msg[256] 各一帧），
// 以及 assembleConcatSms/Push::send 等，分配 8192 字节留足余量
constexpr int  SMS_PROC_TASK_STACK  = 8192;
constexpr int  SMS_PROC_TASK_PRIO   = 2;

// PDU 字符串最大长度（SIM 模组单条 PDU hex 串最长 ~340 字符）
constexpr int  PDU_MAX_LEN          = 400;

// 单个拼接分片
struct SmsPart {
  bool   valid;    // 是否已收到
  String text;     // 分片解码后的文本
};

// 一条拼接短信的状态
struct ConcatSms {
  bool          inUse;             // 槽位是否在用
  int           refNumber;         // UDH 中的 reference number（区分不同拼接短信）
  String        sender;            // 发送方号码
  String        timestamp;         // 第一片的 SCTS 时间戳
  int           totalParts;        // UDH 中的分片总数
  int           receivedParts;     // 已接收的分片数
  unsigned long firstPartTime;     // 收到第一片的本地时间，用于超时判断
  SmsPart       parts[MAX_CONCAT_PARTS];
};

// SMS 业务层：负责短信收发、PDU 拼接、转发推送。
// URC 入口由 Sim 类路由过来；底层 AT 调度走 SimDispatcher。
class Sms {
public:
  // 初始化拼接缓冲区（清零所有 ConcatSms 槽位）
  static void initConcatBuffer();

  // 检查并丢弃超时未收齐的拼接短信（loop 周期调用）
  static void checkConcatTimeout();

  // 通过 PDU 模式发送短信；支持中文 / 长短信自动拆分。
  // 返回 true 表示模组已接收（不代表对端送达）。
  static bool sendPDU(const char* phoneNumber, const char* message);

  // 启动 SMS 处理任务（独立 FreeRTOS 任务，避免 URC 回调链路过长阻塞 Reader Task）。
  // 必须在 Sim::startReaderTask() 之前调用。
  static void startProcTask();

  // URC 路由入口（由 SIM Reader Task 上下文调用）
  static void handleCMTHeader();              // +CMT: 短信头到达
  static void handlePDU(const String& line);  // PDU 内容行
  static void handleUSSD(const String& line); // +CUSD: USSD 应答
};

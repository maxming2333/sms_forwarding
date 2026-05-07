#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ---------- Dispatcher 常量 ----------

// 命令队列容量；溢出时 sendCommand() 会返回 false（极少触发，通常并发不会超过 2~3）
constexpr int          SIM_CMD_QUEUE_SIZE        = 16;
// Reader Task 栈大小；包含 PDU 解析 + URC 回调链路开销，4096 字节实测充足
constexpr uint32_t     SIM_READER_TASK_STACK     = 4096;
// Reader Task 优先级（高于普通 loop，避免长 HTTP 处理时阻塞 SIM URC）
constexpr UBaseType_t  SIM_READER_TASK_PRIORITY  = 3;
constexpr size_t        SIM_RESP_BUF_SIZE          = 256;
constexpr size_t        SIM_LINE_BUF_MAX           = 512;
constexpr unsigned long SIM_TIMEOUT_DRAIN_QUIET_MS = 300;

// 单条 AT 命令上下文（调用方栈上分配，仅入队指针；同步信号量在槽内创建）
struct SimCmdSlot {
  char              cmd[64];        // AT 命令字符串
  unsigned long     timeoutMs;      // 超时时间
  char              respBuf[256];   // 响应缓冲（截断时仍保留前 255 字节）
  SemaphoreHandle_t doneSem;        // 完成信号量（Reader Task → 调用方）
  bool              isOk;           // OK / ERROR
  bool              priority;       // 优先命令（插队到队头）
};

// URC 类型枚举（dispatcher 只做最小解析，业务字段交回上层）
enum class SimUrcType : uint8_t {
  RING       = 0,    // 来电响铃
  CLIP       = 1,    // 来电号码
  CMT        = 2,    // 文本短信头
  CMT_PDU    = 3,    // PDU 短信内容行
  CPIN_READY = 4,    // SIM 卡就绪
  SIM_REMOVE = 5,    // SIM 卡拔出
  CUSD       = 6,    // USSD 应答
};

// URC 回调签名：dispatcher 在 Reader Task 上下文回调
using SimUrcCallback = void (*)(SimUrcType type, const String& line);

// SimDispatcher：纯通讯层。
// 职责：
//   - 持有 Serial1 的读写权；任何对 SIM 模组的字节级访问都必须经此类
//   - FIFO 命令队列 + Reader Task 模型，串行化所有 AT 调度，避免响应混淆
//   - URC 解析与回调分发（不含业务逻辑）
//   - 对外提供 pauseReader/resumeReader 以便特殊场景（如 OTA 期）独占 UART
// **不含**任何业务字段（号码/状态/运营商等），那些归 Sim 类。
class SimDispatcher {
public:
  // 注册 URC 回调；必须在 start() 之前调用。当前仅支持单回调。
  static void   registerUrcCallback(SimUrcCallback cb);

  // 创建队列 + 启动 Reader Task；应在 Sim::init() 成功后调用一次。
  static void   start();

  // start() 是否已成功执行（队列与任务均已就绪）。
  static bool   running();

  // 发送 AT 命令并等待响应。线程安全（队列 + 二值信号量）。
  // 返回 true 表示 OK；false 表示 ERROR/超时/队列满。start() 之前调用必失败。
  // - outResp：可选输出，截取到 respBuf 容量内的响应文本（含状态行）
  // - prio：true 时插入队头，用于关键控制命令
  static bool   sendCommand(const char* cmd, unsigned long timeoutMs,
                            String* outResp = nullptr, bool prio = false);

  // 暂停 Reader Task，调用方可直接 Serial1.read/write（必须配对 resumeReader）。
  // 返回 false 表示在 timeoutMs 内未能确认 Reader Task 让出 UART。
  static bool   pauseReader(unsigned long timeoutMs = 10000);
  static void   resumeReader();
};

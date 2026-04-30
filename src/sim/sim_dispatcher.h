/**
 * @file sim_dispatcher.h
 * @brief SIM 指令调度器 — 公共 API 契约
 *
 * 该模块提供统一的 SIM AT 指令异步调度接口，封装：
 *   - FreeRTOS 队列（FIFO，容量 SIM_CMD_QUEUE_SIZE）
 *   - 每条指令独立的二值信号量（xSemaphoreTake 阻塞等待）
 *   - 单一 SIM reader task 独占 Serial1 读写
 *   - 来电相关指令的优先插队（priority = true）
 *
 * 使用模式（调用方在任意 FreeRTOS task 或 loop() 中）:
 * @code
 *   String resp;
 *   bool ok = simSendCommand("AT+CNUM", 3000, &resp);
 *   if (ok) { ... 解析 resp ... }
 * @endcode
 *
 * 线程安全性:
 *   simSendCommand() 是线程安全的（通过队列和二值信号量同步）。
 *   Serial1 读写仅在 SIM reader task 内部执行，无竞争。
 *
 * 生命周期:
 *   simDispatcherStart() 必须在 simInit() 完成后调用（由 sim.cpp 的 simStartReaderTask() 封装）。
 *   setup() 阶段（simStartReaderTask() 调用之前）不可调用 simSendCommand()。
 */

#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ---------- 具名常量 ----------

/** FreeRTOS 队列最大深度（条数）。超出时 simSendCommand() 立即返回 false。 */
constexpr int SIM_CMD_QUEUE_SIZE = 16;

/** SIM reader task 栈大小（字节）。
 *  reader task 只负责 Serial1 读取和 URC 入队，SMS 解码已卸载到 sms_proc 任务，
 *  栈用量固定（不随 SMS 长度增长），4096 字节足够。 */
constexpr uint32_t SIM_READER_TASK_STACK = 4096;

/** SIM reader task FreeRTOS 优先级（高于 loop() 的优先级 1）。 */
constexpr UBaseType_t SIM_READER_TASK_PRIORITY = 3;

// ---------- 指令槽（内部数据结构，暴露以允许栈上分配） ----------

/**
 * @brief 单条 SIM 指令的完整上下文。
 *        调用方在栈上分配，通过指针入队。
 *        reader task 填充 respBuf/isOk 后 xSemaphoreGive(doneSem)。
 */
struct SimCmdSlot {
    char              cmd[64];      ///< AT 指令字符串（含 \0）
    unsigned long     timeoutMs;    ///< 等待响应的超时（毫秒）
    char              respBuf[256]; ///< reader task 填入的原始响应（含 OK/ERROR）
    SemaphoreHandle_t doneSem;      ///< 完成信号量（调用方 Take，reader task Give）
    bool              isOk;        ///< 响应是否以 OK 结束
    bool              priority;    ///< true = 插队到队首（用于 RING/CLIP 优先处理）
};

// ---------- URC 回调类型 ----------

/** URC 事件类型，由 reader task 识别后传给注册回调。 */
enum class SimUrcType : uint8_t {
    RING       = 0,  ///< "RING" URC
    CLIP       = 1,  ///< "+CLIP:" URC
    CMT        = 2,  ///< "+CMT:" URC（短信到达头部）
    CMT_PDU    = 3,  ///< CMT 之后的 PDU 数据行
    CPIN_READY = 4,  ///< "+CPIN: READY"
    SIM_REMOVE = 5,  ///< "+CPIN: NOT INSERTED" / "+SIMCARD:0"
    CUSD       = 6,  ///< "+CUSD:" USSD 消息上报
};

/** URC 回调函数类型。line 为原始 URC 行内容（不含 CR/LF）。 */
using SimUrcCallback = void (*)(SimUrcType type, const String& line);

// ---------- 初始化与 URC 注册 ----------

/**
 * @brief 注册 URC 回调，在 simDispatcherStart() 之前调用。
 *        reader task 识别到对应 URC 时调用此回调（在 reader task 上下文中）。
 *        只允许注册一个回调（简化实现）。
 */
void simRegisterUrcCallback(SimUrcCallback cb);

/**
 * @brief 创建 FreeRTOS 队列并启动 SIM reader task。
 *        内部启动函数，由 sim.cpp 的 simStartReaderTask() 调用，只调用一次。
 */
void simDispatcherStart();

// ---------- 命令调度 ----------

/**
 * @brief 发送 AT 指令并等待响应（阻塞调用）。
 *
 * @param cmd        AT 指令字符串（不含 \r\n，函数内部自动追加）
 * @param timeoutMs  等待 OK/ERROR 的超时（毫秒）
 * @param outResp    [可选] 若非 nullptr，填入原始响应文本（截断至 255 字节）
 * @param prio       true = 插队到队首（用于来电优先处理）
 * @return           true 表示收到 OK，false 表示 ERROR/超时/队列满
 *
 * @note 在 simStartReaderTask() 调用之前不可调用此函数。
 */
bool simSendCommand(const char* cmd,
                    unsigned long timeoutMs,
                    String* outResp = nullptr,
                    bool prio = false);

// ---------- 实时本机号码查询 ----------

/**
 * @brief 通过 AT+CNUM 实时查询本机 SIM 卡号码。
 *
 * @param timeoutMs  AT 指令超时（毫秒，默认 3000）
 * @return           本机号码字符串；查询失败时返回 "未知号码"
 *
 * @note 此函数阻塞调用方直到查询完成或超时。
 *       应在推送内容组装阶段调用一次，不缓存结果。
 */
String simQueryPhoneNumber(unsigned long timeoutMs = 3000);

// ---------- Reader task 控制（供直接操作 Serial1 的代码使用） ----------

/**
 * @brief 挂起 SIM reader task，使调用方可独占访问 Serial1。
 *        必须与 simResumeReader() 配对使用。
 *        在 simDispatcherStart() 之前调用无效。
 */
bool simPauseReader(unsigned long timeoutMs = 10000);

/**
 * @brief 恢复 SIM reader task（与 simPauseReader() 配对）。
 */
void simResumeReader();

/**
 * @brief 返回 dispatcher 队列是否已创建（即 simDispatcherStart() 是否已调用）。
 *        可用于区分 setup 阶段（直接 Serial1）和运行阶段（队列路由）。
 */
bool simDispatcherRunning();

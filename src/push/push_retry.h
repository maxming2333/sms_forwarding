#pragma once
#include <Arduino.h>
#include "push.h"

// 重试队列上限；超过则丢弃最早的任务（避免长时间无网导致内存爆炸）
constexpr int           PUSH_RETRY_QUEUE_MAX     = 50;
// 特殊 channelIndex 值：表示"全链路重试"，即重新走 Push::send() 的全部启用渠道
// （而非单独某一个渠道）。用于"等待 SIM 号码就绪"等场景。
constexpr int           PUSH_RETRY_FULL_CHAIN    = -1;
// 重试节奏：每 5 秒 tick 一次队列
constexpr unsigned long PUSH_RETRY_INTERVAL_MS   = 5000;

// 号码等待相关常量
constexpr unsigned long WAITING_NUMBER_CHECK_MS   = 30000;
constexpr unsigned long WAITING_NUMBER_TIMEOUT_MS = 300000;

// 重试原因
enum class RetryReason : uint8_t {
  SEND_FAILED    = 0,    // 渠道发送失败（网络错误/HTTP 非 2xx 等）
  WAITING_NUMBER = 1     // 推送时本机号码尚未就绪，等待 SIM 号码后再发送
};

// 单条重试任务：完整携带渲染上下文，便于队列异步执行
struct PushRetryTask {
  int           channelIndex;     // 目标渠道索引；PUSH_RETRY_FULL_CHAIN 表示全链路
  String        sender;
  String        message;
  String        timestamp;
  MsgTypeInfo   msgType;
  RetryReason   reason    = RetryReason::SEND_FAILED;
  unsigned long enqueueMs = 0;    // 入队时间，用于过期 / 节流
};

// PushRetry：失败/等待型推送的重试调度器（队列 + 周期 tick）。
class PushRetry {
public:
  // 初始化（清空队列）
  static void init();

  // 入队失败任务（默认 reason = SEND_FAILED）
  static void enqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType);

  // 入队任务（指定原因，例如 WAITING_NUMBER）
  static void enqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType, RetryReason reason);

  // loop 周期调用：按 PUSH_RETRY_INTERVAL_MS 节流处理队列。
  static void tick();
};

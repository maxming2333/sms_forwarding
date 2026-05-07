#include "push_retry.h"
#include "push.h"
#include "sim/sim.h"
#include "logger.h"
#include <queue>

// 内部重试条目：包装 PushRetryTask + 下次重试时间戳
struct RetryEntry {
  PushRetryTask     task;
  unsigned long     nextRetryMs;
};

// std::queue 非线程安全，但 PushRetry 的所有操作（enqueue/tick）均在主循环
// 任务中执行（经由 PushQueue::tick → Push::sendNow，或 PushRetry::tick → Push::sendNow），
// 因此无并发访问，不需要互斥锁。
static std::queue<RetryEntry> s_retryQueue;

void PushRetry::init() {
  while (!s_retryQueue.empty()) {
    s_retryQueue.pop();
  }
}

void PushRetry::enqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType) {
  PushRetry::enqueue(channelIndex, sender, message, timestamp, msgType, RetryReason::SEND_FAILED);
}

void PushRetry::enqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType, RetryReason reason) {
  if (s_retryQueue.size() >= PUSH_RETRY_QUEUE_MAX) {
    LOG("Retry", "重试队列已满（%d 条），丢弃最旧条目", PUSH_RETRY_QUEUE_MAX);
    s_retryQueue.pop();
  }
  RetryEntry entry;
  entry.task.channelIndex  = channelIndex;
  entry.task.sender        = sender;
  entry.task.message       = message;
  entry.task.timestamp     = timestamp;
  entry.task.msgType       = msgType;
  entry.task.reason        = reason;
  entry.task.enqueueMs     = millis();
  entry.nextRetryMs        = millis() + PUSH_RETRY_INTERVAL_MS;
  s_retryQueue.push(entry);
}

void PushRetry::tick() {
  if (s_retryQueue.empty()) return;
  RetryEntry& front = s_retryQueue.front();
  const PushRetryTask& t = front.task;

  if (t.reason == RetryReason::WAITING_NUMBER) {
    // 5 分钟超时：强制发出
    if (millis() - t.enqueueMs > WAITING_NUMBER_TIMEOUT_MS) {
      LOG("Retry", "等待超时，强制发送，通道索引 %d", t.channelIndex);
      if (t.channelIndex == PUSH_RETRY_FULL_CHAIN) {
          Push::executeChain(t.sender, t.message, t.timestamp, t.msgType);
      } else {
        String forceSender = t.sender + " [接收者未知]";
        Push::executeChannel(t.channelIndex, forceSender, t.message, t.timestamp, t.msgType);
      }
      s_retryQueue.pop();
      return;
    }
    // 号码已就绪：立即发出
    if (Sim::isNumberReady()) {
      if (t.channelIndex == PUSH_RETRY_FULL_CHAIN) {
        LOG("Retry", "号码就绪，重新执行完整推送链");
          Push::executeChain(t.sender, t.message, t.timestamp, t.msgType);
        s_retryQueue.pop();
        return;
      }
      LOG("Retry", "号码就绪，立即发送，通道索引 %d", t.channelIndex);
      bool ok = Push::executeChannel(t.channelIndex, t.sender, t.message, t.timestamp, t.msgType);
      if (ok) {
        LOG("Retry", "号码就绪发送成功，通道索引 %d，出队", t.channelIndex);
      } else {
        LOG("Retry", "号码就绪但发送失败，通道索引 %d，转为普通重试", t.channelIndex);
        front.task.reason = RetryReason::SEND_FAILED;
        front.nextRetryMs = millis() + PUSH_RETRY_INTERVAL_MS;
        return;
      }
      s_retryQueue.pop();
      return;
    }
    // 号码尚未就绪：30s 后再检查
    if (millis() >= front.nextRetryMs) {
      front.nextRetryMs = millis() + WAITING_NUMBER_CHECK_MS;
      LOG("Retry", "等待本机号码就绪，30s 后再检查，通道索引 %d", t.channelIndex);
    }
    return;
  }

  // RetryReason::SEND_FAILED — 保持原有逻辑
  if (millis() < front.nextRetryMs) return;
  bool ok = Push::executeChannel(t.channelIndex, t.sender, t.message, t.timestamp, t.msgType);
  if (ok) {
    LOG("Retry", "重试成功，通道索引 %d，出队", t.channelIndex);
    s_retryQueue.pop();
  } else {
    LOG("Retry", "重试失败，通道索引 %d，下次重试 %lu ms 后", t.channelIndex, PUSH_RETRY_INTERVAL_MS);
    front.nextRetryMs = millis() + PUSH_RETRY_INTERVAL_MS;
  }
}

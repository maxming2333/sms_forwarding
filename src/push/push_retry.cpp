#include "push_retry.h"
#include "push.h"
#include "sim/sim.h"
#include "logger.h"
#include <queue>

// 号码等待相关常量
constexpr unsigned long WAITING_NUMBER_CHECK_MS   = 30000;
constexpr unsigned long WAITING_NUMBER_TIMEOUT_MS = 300000;

// 内部重试条目：包装 PushRetryTask + 下次重试时间戳
struct RetryEntry {
  PushRetryTask     task;
  unsigned long     nextRetryMs;
};

static std::queue<RetryEntry> s_retryQueue;

void pushRetryInit() {
  while (!s_retryQueue.empty()) {
    s_retryQueue.pop();
  }
}

void pushRetryEnqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, MsgType msgType) {
  pushRetryEnqueue(channelIndex, sender, message, timestamp, msgType, RetryReason::SEND_FAILED);
}

void pushRetryEnqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, MsgType msgType, RetryReason reason) {
  if (s_retryQueue.size() >= PUSH_RETRY_QUEUE_MAX) {
    LOG("Retry", "重试队列已满（%d 条），丢弃最旧条目", PUSH_RETRY_QUEUE_MAX);
    s_retryQueue.pop();
  }
  RetryEntry entry;
  entry.task.channelIndex = channelIndex;
  entry.task.sender       = sender;
  entry.task.message      = message;
  entry.task.timestamp    = timestamp;
  entry.task.msgType      = msgType;
  entry.task.reason       = reason;
  entry.task.enqueueMs    = millis();
  entry.nextRetryMs       = millis() + PUSH_RETRY_INTERVAL_MS;
  s_retryQueue.push(entry);
}

void pushRetryTick() {
  if (s_retryQueue.empty()) return;
  RetryEntry& front = s_retryQueue.front();
  const PushRetryTask& t = front.task;

  if (t.reason == RetryReason::WAITING_NUMBER) {
    // 5 分钟超时：强制发出
    if (millis() - t.enqueueMs > WAITING_NUMBER_TIMEOUT_MS) {
      LOG("Retry", "等待超时，强制发送，通道索引 %d", t.channelIndex);
      String forceSender = t.sender + " [接收者未知]";
      sendPushChannel(t.channelIndex, forceSender, t.message, t.timestamp, t.msgType);
      s_retryQueue.pop();
      return;
    }
    // 号码已就绪：立即发出
    if (simIsNumberReady()) {
      LOG("Retry", "号码就绪，立即发送，通道索引 %d", t.channelIndex);
      // 保存发送参数，因为下方可能修改队列导致引用失效
      int      chIdx     = t.channelIndex;
      String   sender    = t.sender;
      String   message   = t.message;
      String   timestamp = t.timestamp;
      MsgType  msgType   = t.msgType;
      bool ok = sendPushChannel(chIdx, sender, message, timestamp, msgType);
      if (ok) {
        LOG("Retry", "号码就绪发送成功，通道索引 %d，出队", chIdx);
        s_retryQueue.pop();
        // 故障转移模式：清除同组其他等待任务，避免后续通道被重复发送
        if (config.pushStrategy == PUSH_STRATEGY_FAILOVER) {
          size_t remaining = s_retryQueue.size();
          for (size_t j = 0; j < remaining; j++) {
            RetryEntry e = std::move(s_retryQueue.front());
            s_retryQueue.pop();
            if (e.task.reason == RetryReason::WAITING_NUMBER &&
                e.task.sender    == sender    &&
                e.task.message   == message   &&
                e.task.timestamp == timestamp) {
              LOG("Retry", "故障转移模式：已成功，丢弃同组等待任务，通道索引 %d", e.task.channelIndex);
            } else {
              s_retryQueue.push(e);
            }
          }
        }
      } else {
        LOG("Retry", "号码就绪但发送失败，通道索引 %d，转为普通重试", chIdx);
        front.task.reason = RetryReason::SEND_FAILED;
        front.nextRetryMs = millis() + PUSH_RETRY_INTERVAL_MS;
        return;
      }
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
  bool ok = sendPushChannel(t.channelIndex, t.sender, t.message, t.timestamp, t.msgType);
  if (ok) {
    LOG("Retry", "重试成功，通道索引 %d，出队", t.channelIndex);
    s_retryQueue.pop();
  } else {
    LOG("Retry", "重试失败，通道索引 %d，下次重试 %lu ms 后", t.channelIndex, PUSH_RETRY_INTERVAL_MS);
    front.nextRetryMs = millis() + PUSH_RETRY_INTERVAL_MS;
  }
}

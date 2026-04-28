#pragma once
#include <Arduino.h>
#include "push.h"

// 推送重试队列上限（超出时丢弃最旧条目）
constexpr int PUSH_RETRY_QUEUE_MAX = 50;

// 特殊通道索引：代表整条推送链（重新完整执行 sendPushNotification）
constexpr int PUSH_RETRY_FULL_CHAIN = -1;

// 重试间隔（毫秒）
constexpr unsigned long PUSH_RETRY_INTERVAL_MS = 5000;

// 重试原因枚举
enum class RetryReason : uint8_t {
  SEND_FAILED    = 0,  // 发送失败，需要重试
  WAITING_NUMBER = 1   // 本机号码未知，等待号码就绪
};

// 重试任务描述
struct PushRetryTask {
  int           channelIndex;
  String        sender;
  String        message;
  String        timestamp;
  MsgTypeInfo   msgType;
  RetryReason   reason    = RetryReason::SEND_FAILED;
  unsigned long enqueueMs = 0;
};

void pushRetryInit();
void pushRetryEnqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType);
void pushRetryEnqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType, RetryReason reason);
void pushRetryTick();

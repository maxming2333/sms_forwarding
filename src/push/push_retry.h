#pragma once
#include <Arduino.h>
#include "config/config.h"

// 推送重试队列上限（超出时丢弃最旧条目）
constexpr int PUSH_RETRY_QUEUE_MAX = 50;

// 重试间隔（毫秒）
constexpr unsigned long PUSH_RETRY_INTERVAL_MS = 5000;

// 重试任务描述
struct PushRetryTask {
  int    channelIndex;
  String sender;
  String message;
  String timestamp;
  MsgType msgType;
};

void pushRetryInit();
void pushRetryEnqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, MsgType msgType);
void pushRetryTick();

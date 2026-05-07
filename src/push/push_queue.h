#pragma once
#include <Arduino.h>
#include "push.h"

// 推送发送队列：将所有 Push::send() 调用串行化，避免密集推送时
// TLS/TCP 资源竞争（典型场景：来电通知与即显短信几乎同时触发）。
//
// 设计原则：
//   - enqueue() 使用 FreeRTOS 队列，线程安全，可从任意任务调用；
//   - tick() 仅在主循环调用，每次出队一条消息并调用 Push::executeChain()；
//   - 不引入人工 delay，push_channels.cpp 内的 HTTP_COOLDOWN_MS 已处理
//     连续请求的 TLS/TCP 资源释放问题；
//   - PushRetry（失败重试）直接调用 Push::executeChain()，不经此队列，
//     因为重试链路已有 5 秒节流且仅在主循环运行。

// 两条消息之间的最小间隔（ms）。
// push_channels.cpp 的 HTTP_COOLDOWN_MS（500ms）只保证单次 HTTP 请求之间的间隔；
// 此値在“推送链”粒度上提供额外保护，确保上一条消息的所有 TLS/TCP 连接
// 已完全归还资源后，再开始下一条消息的推送链。
constexpr unsigned long PUSH_QUEUE_INTERVAL_MS = 3000;


// 队列最大容量；超出时丢弃最旧条目（防止内存泄漏）
constexpr int PUSH_QUEUE_MAX = 20;

class PushQueue {
public:
  // 初始化队列（在 setup() 中，PushRetry::init() 之后调用）
  static void init();

  // 入队（线程安全；队满时自动丢弃最旧条目并记录日志）
  static void enqueue(const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType);

  // 主循环调用：非阻塞出队一条消息，调用 Push::executeChain() 执行
  static void tick();
};

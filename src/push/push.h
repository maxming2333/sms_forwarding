#pragma once
#include <Arduino.h>
#include "config/config.h"

// 消息类型描述：由调用方构造，按 `type` 路由到对应渲染分支。
// `typeLabel` 仅在 MSG_TYPE_SMS 时使用，例如 "普通短信" / "STK 推送" 等子分类。
struct MsgTypeInfo {
  MsgType type = MSG_TYPE_SMS;
  String  typeLabel;

  MsgTypeInfo() = default;
  MsgTypeInfo(MsgType t) : type(t) {}
  MsgTypeInfo(MsgType t, const String& cls) : type(t), typeLabel(cls) {}

  // 渲染为人类可读字符串（用于推送标题等场景）
  String toString() const {
    if (type == MSG_TYPE_CALL) {
      return "来电";
    }
    if (type == MSG_TYPE_SIM) {
      return "SIM事件";
    }
    if (type == MSG_TYPE_SMS) {
      return typeLabel.length() > 0 ? ("短信：" + typeLabel) : "短信";
    }
    // 未知 MsgType 兜底，保留原值便于排查
    char typeBuf[32];
    snprintf(typeBuf, sizeof(typeBuf), "未知消息(type=%d)", (int)type);
    return typeLabel.length() > 0 ? (String(typeBuf) + "：" + typeLabel) : String(typeBuf);
  }
};

// Push 顶层入口：负责并发派发到所有启用的推送渠道。
// 单条消息会按通道顺序串行调用各渠道；失败会进入 PushRetry 队列重试。
class Push {
public:
  // 向所有启用的渠道派发一条消息；入队至 PushQueue 串行执行。
  static void send(const String& sender, const String& message,
                   const String& timestamp, const MsgTypeInfo& msgType);

  // 直接执行推送链（不经过 PushQueue），由 PushQueue::tick()/PushRetry::tick() 调用。
  // 调用方须确保不在 sms_proc 等后台任务中直接调用（避免阻塞 SIM reader）。
  static void executeChain(const String& sender, const String& message,
                      const String& timestamp, const MsgTypeInfo& msgType);

  // 单渠道推送（通常由重试队列回调）。返回 true 表示该渠道成功。
  static bool executeChannel(int channelIdx, const String& sender, const String& message,
                          const String& timestamp, const MsgTypeInfo& msgType);
};

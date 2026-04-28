#pragma once
#include <Arduino.h>
#include "config/config.h"

// 消息类型描述结构体：调用方构建后直接传入 push 函数
// 内部通过 type 做路由判断，smsClass 仅在 MSG_TYPE_SMS 时有意义
struct MsgTypeInfo {
  MsgType type     = MSG_TYPE_SMS;
  String  smsClass;  // 可选，如 "Class 0（即显短信）"

  MsgTypeInfo() = default;
  MsgTypeInfo(MsgType t) : type(t) {}
  MsgTypeInfo(MsgType t, const String& cls) : type(t), smsClass(cls) {}

  // 序列化为人类可读字符串（供模板占位符 {message_type} 使用）
  String toString() const {
    if (type == MSG_TYPE_CALL) return "来电";
    if (type == MSG_TYPE_SIM)  return "SIM事件";
    return smsClass.length() > 0 ? ("短信：" + smsClass) : "短信";
  }
};

void sendPushNotification(const String& sender, const String& message, const String& timestamp, const MsgTypeInfo& msgType);
bool sendPushChannel(int channelIdx, const String& sender, const String& message, const String& timestamp, const MsgTypeInfo& msgType);

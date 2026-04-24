#pragma once
#include <Arduino.h>

struct MessageContext {
  String from;         // [RENAMED from sender]
  String message;
  String timestamp;
  String date;
  String deviceId;
  String carrier;
  String to;           // [RENAMED from simNumber]
  String simSlot;
  String signal;
  String remark;       // [NEW] 设备备注
  String triggerType;  // [NEW] 触发类型
  String uptime;       // [NEW] 设备运行时长
  String channelName;  // [NEW] 当前推送通道名称
  String channelType;  // [NEW] 当前推送类型（中文标签）
};

// 格式化运行时长（ms → 人类可读字符串）
String formatUptime(unsigned long ms);

// 获取设备唯一 ID（首次调用时读取并缓存）
String msgContextGetDeviceId();

// 渲染消息模板：替换所有已识别占位符，未识别占位符原样保留
String renderTemplate(const String& tmpl, const MessageContext& ctx);

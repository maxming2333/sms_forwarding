#include "msg_context.h"
#include <Arduino.h>
#include "wifi/wifi_manager.h"

String msgContextGetDeviceId() {
  return getDeviceId();
}

String formatUptime(unsigned long ms) {
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours   = minutes / 60;
  unsigned long days    = hours   / 24;

  if (ms < 60000UL) {
    return "不足1分钟";
  } else if (ms < 3600000UL) {
    return String(minutes) + "分钟";
  } else if (ms < 86400000UL) {
    unsigned long h = hours;
    unsigned long m = minutes % 60;
    return String(h) + "小时" + String(m) + "分";
  } else {
    unsigned long d = days;
    unsigned long h = hours % 24;
    return String(d) + "天" + String(h) + "小时";
  }
}

String renderTemplate(const String& tmpl, const MessageContext& ctx) {
  String result = tmpl;
  // 新增占位符（空值兜底为空字符串）
  result.replace("{remark}",        ctx.remark);
  result.replace("{trigger_type}",  ctx.triggerType);
  result.replace("{uptime}",        ctx.uptime);
  result.replace("{channel_name}",  ctx.channelName);
  result.replace("{channel_type}",  ctx.channelType);
  // {from} 及向后兼容别名 {sender}
  result.replace("{from}",          ctx.from.length()      > 0 ? ctx.from      : "未知");
  result.replace("{sender}",        ctx.from.length()      > 0 ? ctx.from      : "未知");
  result.replace("{message}",       ctx.message);
  result.replace("{timestamp}",     ctx.timestamp.length() > 0 ? ctx.timestamp : "0");
  result.replace("{date}",          ctx.date.length()      > 0 ? ctx.date      : "未知");
  result.replace("{device_id}",     ctx.deviceId.length()  > 0 ? ctx.deviceId  : "未知");
  result.replace("{carrier}",       ctx.carrier.length()   > 0 ? ctx.carrier   : "未知");
  // {to} 及向后兼容别名 {sim_number}
  result.replace("{to}",            ctx.to.length()        > 0 ? ctx.to        : "未知");
  result.replace("{sim_number}",    ctx.to.length()        > 0 ? ctx.to        : "未知");
  result.replace("{sim_slot}",      ctx.simSlot.length()   > 0 ? ctx.simSlot   : "SIM1");
  result.replace("{signal}",        ctx.signal.length()    > 0 ? ctx.signal    : "未知");
  return result;
}

// SMS forward  url field holds the target phone number
#include "PushChannels.h"
#include "utils/Utils.h"
#include "sms/SmsSender.h"

int pushSmsForward(const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev) {
  String phone = ch.url;
  phone.trim();
  String content = "📱短信通知\n发送者: " + formatPhoneNumber(sender)
                 + "\n接收卡号: "          + String(dev)
                 + "\n内容: "             + String(msg)
                 + "\n时间: "             + formatTimestamp(ts);
  Serial.println("[PushSmsForward] 转发至: " + phone);
  return smsSend(phone.c_str(), content.c_str()) ? 0 : -2;
}


#include "push.h"
#include "push_channels.h"
#include "push_retry.h"
#include "msg_context.h"
#include "time/time_module.h"
#include "sim/sim.h"
#include "sim/sim_dispatcher.h"
#include "logger.h"
#include <WiFi.h>
#include <time.h>

void sendPushNotification(const String& sender, const String& message, const String& timestamp, MsgType msgType) {
  LOG("Push", "=== 开始多通道推送 ===");
  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  // 构建消息上下文（每次推送时构建一次，所有通道共享）
  MessageContext ctx;
  ctx.sender    = sender;
  ctx.message   = message;
  ctx.timestamp = timestamp;
  ctx.date      = timeModuleGetDateStr();
  ctx.deviceId  = msgContextGetDeviceId();
  ctx.carrier   = simGetCarrier();
  ctx.simNumber = simQueryPhoneNumber(3000);
  ctx.simSlot   = "SIM1";
  ctx.signal    = simGetSignal();

  for (int i = 0; i < config.pushCount; i++) {
    const PushChannel& ch = config.pushChannels[i];
    if (!isPushChannelValid(ch)) continue;

    // HTTP 类通道（type 1–11）在 WiFi 未连接时跳过
    if (ch.type >= PUSH_TYPE_POST_JSON && ch.type <= PUSH_TYPE_WECHAT_WORK && !wifiOk) {
      LOG("Push", "WiFi未连接，跳过HTTP通道: %s", ch.name.c_str());
      continue;
    }

    // SMS 通道在 SIM 事件时跳过（避免循环）
    if (ch.type == PUSH_TYPE_SMS && msgType == MSG_TYPE_SIM) {
      LOG("Push", "SIM事件跳过SMS通道: %s", ch.name.c_str());
      continue;
    }

    String name = ch.name.length() > 0 ? ch.name : ("通道" + String(ch.type));
    LOG("Push", "发送到推送通道: %s", name.c_str());

    // 渲染自定义消息格式（非空时替换内置默认格式）
    String renderedBody = ch.customBody.length() > 0 ? renderTemplate(ch.customBody, ctx) : "";

    bool ok = false;
    switch (ch.type) {
      case PUSH_TYPE_POST_JSON:  ok = sendPostJson(ch, sender, message, timestamp, renderedBody);  break;
      case PUSH_TYPE_BARK:       ok = sendBark(ch, sender, message, timestamp, renderedBody);       break;
      case PUSH_TYPE_GET:        ok = sendGet(ch, sender, message, timestamp, renderedBody);        break;
      case PUSH_TYPE_DINGTALK:   ok = sendDingtalk(ch, sender, message, timestamp, renderedBody);   break;
      case PUSH_TYPE_PUSHPLUS:   ok = sendPushPlus(ch, sender, message, timestamp, renderedBody);   break;
      case PUSH_TYPE_SERVERCHAN: ok = sendServerChan(ch, sender, message, timestamp, renderedBody); break;
      case PUSH_TYPE_CUSTOM:     ok = sendCustom(ch, sender, message, timestamp, renderedBody);     break;
      case PUSH_TYPE_FEISHU:     ok = sendFeishu(ch, sender, message, timestamp, renderedBody);     break;
      case PUSH_TYPE_GOTIFY:     ok = sendGotify(ch, sender, message, timestamp, renderedBody);     break;
      case PUSH_TYPE_TELEGRAM:   ok = sendTelegram(ch, sender, message, timestamp, renderedBody);   break;
      case PUSH_TYPE_WECHAT_WORK: ok = sendWechatWork(ch, sender, message, timestamp, renderedBody); break;
      case PUSH_TYPE_SMS:        ok = sendSmsPush(ch, sender, message, timestamp, renderedBody);    break;
      default:
        LOG("Push", "未知推送类型: %d", (int)ch.type);
        break;
    }

    if (config.pushStrategy == PUSH_STRATEGY_FAILOVER) {
      if (ok) {
        LOG("Push", "故障转移模式：通道 %s 成功，停止", name.c_str());
        break;
      }
      LOG("Push", "故障转移模式：通道 %s 失败，继续下一个", name.c_str());
    } else {
      // 广播模式：继续所有通道
      delay(100);
    }

    // T021: 失败且开启重试时入队
    if (!ok && ch.retryOnFail) {
      pushRetryEnqueue(i, sender, message, timestamp, msgType);
      LOG("Push", "[Retry] 通道 %s 失败，已加入重试队列", name.c_str());
    }
  }
  LOG("Push", "=== 多通道推送完成 ===");
}

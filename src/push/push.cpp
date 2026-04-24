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

// 内部辅助：按通道配置分发单次推送（不含跳过判断）
// ctx 用于渲染 key1/key2 占位符
static bool _sendOneChannel(const PushChannel& ch, const MessageContext& ctx,
                             const String& sender, const String& message,
                             const String& timestamp, const String& renderedBody) {
  // 在通道副本中渲染 key1/key2（不修改原通道配置）
  PushChannel rendered = ch;
  rendered.key1 = renderTemplate(ch.key1, ctx);
  rendered.key2 = renderTemplate(ch.key2, ctx);

  bool ok = false;
  switch (rendered.type) {
    case PUSH_TYPE_POST_JSON:   ok = sendPostJson(rendered, sender, message, timestamp, renderedBody);    break;
    case PUSH_TYPE_BARK:        ok = sendBark(rendered, sender, message, timestamp, renderedBody);         break;
    case PUSH_TYPE_GET:         ok = sendGet(rendered, sender, message, timestamp, renderedBody);          break;
    case PUSH_TYPE_DINGTALK:    ok = sendDingtalk(rendered, sender, message, timestamp, renderedBody);     break;
    case PUSH_TYPE_PUSHPLUS:    ok = sendPushPlus(rendered, sender, message, timestamp, renderedBody);     break;
    case PUSH_TYPE_SERVERCHAN:  ok = sendServerChan(rendered, sender, message, timestamp, renderedBody);   break;
    case PUSH_TYPE_CUSTOM:      ok = sendCustom(rendered, sender, message, timestamp, renderedBody);       break;
    case PUSH_TYPE_FEISHU:      ok = sendFeishu(rendered, sender, message, timestamp, renderedBody);       break;
    case PUSH_TYPE_GOTIFY:      ok = sendGotify(rendered, sender, message, timestamp, renderedBody);       break;
    case PUSH_TYPE_TELEGRAM:    ok = sendTelegram(rendered, sender, message, timestamp, renderedBody);     break;
    case PUSH_TYPE_WECHAT_WORK: ok = sendWechatWork(rendered, sender, message, timestamp, renderedBody);   break;
    case PUSH_TYPE_SMS:         ok = sendSmsPush(rendered, sender, message, timestamp, renderedBody);      break;
    default:
      LOG("Push", "未知推送类型: %d", (int)rendered.type);
      break;
  }
  return ok;
}

// 构建消息上下文（内部辅助）
static const char* pushTypeLabel(PushType t) {
  switch (t) {
    case PUSH_TYPE_POST_JSON:   return "POST JSON格式";
    case PUSH_TYPE_BARK:        return "Bark 服务";
    case PUSH_TYPE_GET:         return "GET请求";
    case PUSH_TYPE_DINGTALK:    return "钉钉机器人";
    case PUSH_TYPE_PUSHPLUS:    return "PushPlus";
    case PUSH_TYPE_SERVERCHAN:  return "Server酱";
    case PUSH_TYPE_CUSTOM:      return "POST 文本";
    case PUSH_TYPE_FEISHU:      return "飞书机器人";
    case PUSH_TYPE_GOTIFY:      return "Gotify";
    case PUSH_TYPE_TELEGRAM:    return "Telegram Bot";
    case PUSH_TYPE_WECHAT_WORK: return "企业微信机器人";
    case PUSH_TYPE_SMS:         return "SMS 短信";
    default:                    return "未知";
  }
}

static MessageContext buildMsgContext(const String& sender, const String& message, const String& timestamp, const String& triggerType) {
  MessageContext ctx;
  ctx.from        = sender;
  ctx.message     = message;
  ctx.timestamp   = timestamp;
  ctx.date        = timeModuleGetDateStr();
  ctx.deviceId    = msgContextGetDeviceId();
  ctx.carrier     = simGetCarrier();
  ctx.to          = simGetPhoneNum();
  ctx.simSlot     = "SIM1";
  ctx.signal      = simGetSignal();
  ctx.remark      = config.remark;
  ctx.triggerType = triggerType;
  ctx.uptime      = formatUptime(millis());
  return ctx;
}

// 单通道推送：含跳过判断、构建消息上下文，供重试队列调用
bool sendPushChannel(int channelIdx, const String& sender, const String& message, const String& timestamp, MsgType msgType) {
  if (channelIdx < 0 || channelIdx >= config.pushCount) return false;
  const PushChannel& ch = config.pushChannels[channelIdx];
  if (!isPushChannelValid(ch)) return false;

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (ch.type >= PUSH_TYPE_POST_JSON && ch.type <= PUSH_TYPE_WECHAT_WORK && !wifiOk) return false;
  if (ch.type == PUSH_TYPE_SMS && msgType == MSG_TYPE_SIM) return false;

  MessageContext ctx = buildMsgContext(sender, message, timestamp, msgType == MSG_TYPE_CALL ? "来电" : msgType == MSG_TYPE_SIM ? "SIM事件" : "短信");
  ctx.channelName = ch.name;
  ctx.channelType    = pushTypeLabel(ch.type);
  String renderedBody = ch.customBody.length() > 0 ? renderTemplate(ch.customBody, ctx) : "";
  return _sendOneChannel(ch, ctx, sender, message, timestamp, renderedBody);
}

void sendPushNotification(const String& sender, const String& message, const String& timestamp, MsgType msgType) {
  // T015: 推送前检查本机号码是否就绪
  if (!simIsNumberReady()) {
    LOG("Push", "本机号码未知，所有通道入队等待号码就绪");
    for (int i = 0; i < config.pushCount; i++) {
      const PushChannel& ch = config.pushChannels[i];
      if (!isPushChannelValid(ch)) continue;
      if (ch.type == PUSH_TYPE_SMS && msgType == MSG_TYPE_SIM) continue;
      pushRetryEnqueue(i, sender, message, timestamp, msgType, RetryReason::WAITING_NUMBER);
    }
    return;
  }

  LOG("Push", "=== 开始多通道推送 ===");

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool anyAction = false;

  MessageContext ctx = buildMsgContext(sender, message, timestamp,
    msgType == MSG_TYPE_CALL ? "来电" : msgType == MSG_TYPE_SIM ? "SIM事件" : "短信");

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

    anyAction = true;
    String name = ch.name.length() > 0 ? ch.name : ("通道" + String(ch.type));
    LOG("Push", "发送到推送通道: %s", name.c_str());

    // 渲染自定义消息格式（非空时替换内置默认格式）
    ctx.channelName = ch.name;
    ctx.channelType    = pushTypeLabel(ch.type);
    String renderedBody = ch.customBody.length() > 0 ? renderTemplate(ch.customBody, ctx) : "";

    bool ok = _sendOneChannel(ch, ctx, sender, message, timestamp, renderedBody);

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

    // 失败且开启重试时入队
    if (!ok && ch.retryOnFail) {
      pushRetryEnqueue(i, sender, message, timestamp, msgType);
      LOG("Push", "[Retry] 通道 %s 失败，已加入重试队列", name.c_str());
    }
  }

  if (!anyAction) {
    LOG("Push", "未配置任何有效推送通道或未满足前置条件，跳过推送");
  }

  LOG("Push", "=== 多通道推送完成 ===");
}

#include "push.h"
#include "push_channels.h"
#include "push_retry.h"
#include "push_queue.h"
#include "msg_context.h"
#include "time/time_sync.h"
#include "sim/sim.h"
#include "sim/sim_dispatcher.h"
#include "wifi/wifi_manager.h"
#include "../logger/logger.h"
#include <WiFi.h>
#include <time.h>

// 过滤控制字符（0x00-0x1F，保留 \t \n \r），防止 ArduinoJson 序列化出非法 JSON
static String sanitizeText(const String& s) {
  String out; out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s.charAt(i);
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
    out += (char)c;
  }
  return out;
}

// 内部辅助：按通道配置分发单次推送（不含跳过判断）
// ctx 用于渲染 key1/key2 占位符
static bool _sendOneChannel(const PushChannel& ch, const MessageContext& ctx,
                             const String& sender, const String& message,
                             const String& timestamp, const String& renderedBody) {
  // 在通道副本中渲染 key1/key2（不修改原通道配置）
  PushChannel rendered = ch;
  rendered.key1 = MsgContext::render(ch.key1, ctx);
  rendered.key2 = MsgContext::render(ch.key2, ctx);

  bool ok = false;
  switch (rendered.type) {
    case PUSH_TYPE_POST_JSON:   ok = PushChannels::sendPostJson(rendered, sender, message, timestamp, renderedBody);    break;
    case PUSH_TYPE_BARK:        ok = PushChannels::sendBark(rendered, sender, message, timestamp, renderedBody);         break;
    case PUSH_TYPE_GET:         ok = PushChannels::sendGet(rendered, sender, message, timestamp, renderedBody);          break;
    case PUSH_TYPE_DINGTALK:    ok = PushChannels::sendDingtalk(rendered, sender, message, timestamp, renderedBody);     break;
    case PUSH_TYPE_PUSHPLUS:    ok = PushChannels::sendPushPlus(rendered, sender, message, timestamp, renderedBody);     break;
    case PUSH_TYPE_SERVERCHAN:  ok = PushChannels::sendServerChan(rendered, sender, message, timestamp, renderedBody);   break;
    case PUSH_TYPE_CUSTOM:      ok = PushChannels::sendCustom(rendered, sender, message, timestamp, renderedBody);       break;
    case PUSH_TYPE_FEISHU:      ok = PushChannels::sendFeishu(rendered, sender, message, timestamp, renderedBody);       break;
    case PUSH_TYPE_GOTIFY:      ok = PushChannels::sendGotify(rendered, sender, message, timestamp, renderedBody);       break;
    case PUSH_TYPE_TELEGRAM:    ok = PushChannels::sendTelegram(rendered, sender, message, timestamp, renderedBody);     break;
    case PUSH_TYPE_WECHAT_WORK: ok = PushChannels::sendWechatWork(rendered, sender, message, timestamp, renderedBody);   break;
    case PUSH_TYPE_SMS:         ok = PushChannels::sendSmsPush(rendered, sender, message, timestamp, renderedBody);      break;
    default:
      LOG("PUSH", "未知推送类型: %d", (int)rendered.type);
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

static MessageContext buildMsgContext(const String& sender, const String& message, const String& timestamp, const String& messageType) {
  MessageContext ctx;
  ctx.from        = sender;
  ctx.message     = message;
  ctx.timestamp   = timestamp;
  ctx.date        = TimeSync::dateStr();
  ctx.deviceId    = WifiManager::deviceId();
  ctx.carrier     = Sim::carrier();
  ctx.to          = Sim::phoneNum();
  ctx.simSlot     = "SIM1";
  ctx.signal      = Sim::signal();
  ctx.remark      = config.remark;
  ctx.uptime      = MsgContext::formatUptime(millis());
  ctx.deviceName  = WifiManager::deviceName();
  ctx.messageType = messageType;
  return ctx;
}

// 单通道推送：含跳过判断、构建消息上下文，供重试队列调用
bool Push::executeChannel(int channelIdx, const String& sender, const String& message, const String& timestamp, const MsgTypeInfo& msgType) {
  if (channelIdx < 0 || channelIdx >= config.pushCount) return false;
  const PushChannel& ch = config.pushChannels[channelIdx];
  if (!ConfigStore::isPushChannelValid(ch)) return false;

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (ch.type >= PUSH_TYPE_POST_JSON && ch.type <= PUSH_TYPE_WECHAT_WORK && !wifiOk) return false;
  if (ch.type == PUSH_TYPE_SMS && msgType.type == MSG_TYPE_SIM) return false;

  MessageContext ctx = buildMsgContext(sender, message, timestamp, msgType.toString());
  ctx.channelName = ch.name;
  ctx.channelType    = pushTypeLabel(ch.type);
  String renderedBody = ch.customBody.length() > 0 ? sanitizeText(MsgContext::render(ch.customBody, ctx)) : "";
  return _sendOneChannel(ch, ctx, sender, sanitizeText(message), timestamp, renderedBody);
}

void Push::send(const String& sender, const String& message, const String& timestamp, const MsgTypeInfo& msgType) {
  PushQueue::enqueue(sender, message, timestamp, msgType);
}

void Push::executeChain(const String& sender, const String& message, const String& timestamp, const MsgTypeInfo& msgType) {
  // T015: 推送前检查本机号码是否就绪
  // 入队整条推送链，待号码就绪后重新完整执行，确保故障转移策略正确生效
  if (!Sim::isNumberReady()) {
    LOG("PUSH", "本机号码未知，完整推送链入队等待号码就绪");
    PushRetry::enqueue(PUSH_RETRY_FULL_CHAIN, sender, message, timestamp, msgType, RetryReason::WAITING_NUMBER);
    return;
  }

  LOG("PUSH", "=== 开始多通道推送 ===");

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool anyAction = false;

  // 故障转移模式：收集失败通道，仅在整链全部失败时才入队重试，
  // 防止某后续通道成功后仍触发已入队的前序通道二次推送。
  int  failoverRetry[MAX_PUSH_CHANNELS];
  int  failoverRetryCount = 0;
  bool failoverChainDone  = false;  // true = 已有通道成功并 break

  MessageContext ctx = buildMsgContext(sender, message, timestamp, msgType.toString());

  for (int i = 0; i < config.pushCount; i++) {
    const PushChannel& ch = config.pushChannels[i];
    if (!ConfigStore::isPushChannelValid(ch)) continue;

    // HTTP 类通道（type 1–11）在 WiFi 未连接时跳过
    if (ch.type >= PUSH_TYPE_POST_JSON && ch.type <= PUSH_TYPE_WECHAT_WORK && !wifiOk) {
      LOG("PUSH", "WiFi未连接，跳过HTTP通道: %s", ch.name.c_str());
      continue;
    }

    if (ch.type == PUSH_TYPE_SMS && msgType.type == MSG_TYPE_SIM) {
      LOG("PUSH", "SIM事件跳过SMS通道: %s", ch.name.c_str());
      continue;
    }

    anyAction = true;
    String name = ch.name.length() > 0 ? ch.name : ("通道" + String(ch.type));
    LOG("PUSH", "发送到推送通道: %s", name.c_str());

    // 渲染自定义消息格式（非空时替换内置默认格式）
    ctx.channelName = ch.name;
    ctx.channelType    = pushTypeLabel(ch.type);
    String renderedBody = ch.customBody.length() > 0 ? sanitizeText(MsgContext::render(ch.customBody, ctx)) : "";

    bool ok = _sendOneChannel(ch, ctx, sender, sanitizeText(message), timestamp, renderedBody);

    if (config.pushStrategy == PUSH_STRATEGY_FAILOVER) {
      if (ok) {
        LOG("PUSH", "故障转移模式：通道 %s 成功，停止", name.c_str());
        failoverChainDone = true;
        break;
      }
      LOG("PUSH", "故障转移模式：通道 %s 失败，继续下一个", name.c_str());
      // 暂存待重试索引，等整链确认全部失败后再统一入队
      if (ch.retryOnFail && failoverRetryCount < MAX_PUSH_CHANNELS) {
        failoverRetry[failoverRetryCount++] = i;
      }
    } else {
      // 广播模式：继续所有通道
      delay(100);
      if (!ok && ch.retryOnFail) {
        PushRetry::enqueue(i, sender, message, timestamp, msgType);
        LOG("PUSH", "[Retry] 通道 %s 失败，已加入重试队列", name.c_str());
      }
    }
  }

  // 故障转移模式：只有整链全部失败时才入队重试
  if (config.pushStrategy == PUSH_STRATEGY_FAILOVER && !failoverChainDone) {
    for (int j = 0; j < failoverRetryCount; j++) {
      PushRetry::enqueue(failoverRetry[j], sender, message, timestamp, msgType);
      const String& rname = config.pushChannels[failoverRetry[j]].name;
      LOG("PUSH", "[Retry] 故障转移链全部失败，通道 %s 加入重试队列", rname.c_str());
    }
  }

  if (!anyAction) {
    LOG("PUSH", "未配置任何有效推送通道或未满足前置条件，跳过推送");
  }

  LOG("PUSH", "=== 多通道推送完成 ===");
}

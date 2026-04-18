#include "push_retry.h"
#include "push.h"
#include "push_channels.h"
#include "msg_context.h"
#include "time/time_module.h"
#include "sim/sim.h"
#include "logger.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <queue>

static std::queue<PushRetryTask> s_retryQueue;
static SemaphoreHandle_t         s_mutex        = nullptr;
static unsigned long             s_lastRetryMs  = 0;

void pushRetryInit() {
  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) {
    LOG("Push", "pushRetryInit: Mutex 创建失败");
  }
}

void pushRetryEnqueue(int channelIndex, const String& sender, const String& message,
                      const String& timestamp, MsgType msgType) {
  if (s_mutex == nullptr) return;

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if ((int)s_retryQueue.size() >= PUSH_RETRY_QUEUE_MAX) {
    LOG("Push", "[Retry] 队列已满（%d），丢弃最旧条目", PUSH_RETRY_QUEUE_MAX);
    s_retryQueue.pop();
  }
  PushRetryTask task;
  task.channelIndex = channelIndex;
  task.sender       = sender;
  task.message      = message;
  task.timestamp    = timestamp;
  task.msgType      = msgType;
  s_retryQueue.push(task);
  LOG("Push", "[Retry] 任务入队，通道索引 %d，队列大小 %d", channelIndex, (int)s_retryQueue.size());
  xSemaphoreGive(s_mutex);
}

void pushRetryTick() {
  if (s_mutex == nullptr) return;
  if (s_retryQueue.empty()) return;

  unsigned long now = millis();
  if (now - s_lastRetryMs < PUSH_RETRY_INTERVAL_MS) return;
  s_lastRetryMs = now;

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_retryQueue.empty()) {
    xSemaphoreGive(s_mutex);
    return;
  }
  PushRetryTask task = s_retryQueue.front();
  s_retryQueue.pop();
  xSemaphoreGive(s_mutex);

  int i = task.channelIndex;
  if (i < 0 || i >= MAX_PUSH_CHANNELS) {
    LOG("Push", "[Retry] 无效通道索引 %d，丢弃", i);
    return;
  }

  const PushChannel& ch = config.pushChannels[i];
  String name = ch.name.length() > 0 ? ch.name : ("通道" + String(i));

  // HTTP 类通道在 WiFi 未连接时跳过（保留条目以便后续重试）
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (ch.type >= PUSH_TYPE_POST_JSON && ch.type <= PUSH_TYPE_WECHAT_WORK && !wifiOk) {
    LOG("Push", "[Retry] WiFi未连接，跳过通道 %s（重新入队）", name.c_str());
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_retryQueue.push(task);
    xSemaphoreGive(s_mutex);
    return;
  }

  if (!isPushChannelValid(ch)) {
    LOG("Push", "[Retry] 通道 %s 配置无效，丢弃", name.c_str());
    return;
  }

  LOG("Push", "[Retry] 重试通道 %s，发送方: %s", name.c_str(), task.sender.c_str());

  // 构建消息上下文
  MessageContext ctx;
  ctx.sender    = task.sender;
  ctx.message   = task.message;
  ctx.timestamp = task.timestamp;
  ctx.date      = timeModuleGetDateStr();
  ctx.deviceId  = msgContextGetDeviceId();
  ctx.carrier   = simGetCarrier();
  ctx.simNumber = "";
  ctx.simSlot   = "SIM1";
  ctx.signal    = simGetSignal();

  String renderedBody = ch.customBody.length() > 0 ? renderTemplate(ch.customBody, ctx) : "";

  bool ok = false;
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:   ok = sendPostJson(ch, task.sender, task.message, task.timestamp, renderedBody);   break;
    case PUSH_TYPE_BARK:        ok = sendBark(ch, task.sender, task.message, task.timestamp, renderedBody);        break;
    case PUSH_TYPE_GET:         ok = sendGet(ch, task.sender, task.message, task.timestamp, renderedBody);         break;
    case PUSH_TYPE_DINGTALK:    ok = sendDingtalk(ch, task.sender, task.message, task.timestamp, renderedBody);    break;
    case PUSH_TYPE_PUSHPLUS:    ok = sendPushPlus(ch, task.sender, task.message, task.timestamp, renderedBody);    break;
    case PUSH_TYPE_SERVERCHAN:  ok = sendServerChan(ch, task.sender, task.message, task.timestamp, renderedBody);  break;
    case PUSH_TYPE_CUSTOM:      ok = sendCustom(ch, task.sender, task.message, task.timestamp, renderedBody);      break;
    case PUSH_TYPE_FEISHU:      ok = sendFeishu(ch, task.sender, task.message, task.timestamp, renderedBody);      break;
    case PUSH_TYPE_GOTIFY:      ok = sendGotify(ch, task.sender, task.message, task.timestamp, renderedBody);      break;
    case PUSH_TYPE_TELEGRAM:    ok = sendTelegram(ch, task.sender, task.message, task.timestamp, renderedBody);    break;
    case PUSH_TYPE_WECHAT_WORK: ok = sendWechatWork(ch, task.sender, task.message, task.timestamp, renderedBody);  break;
    case PUSH_TYPE_SMS:         ok = sendSmsPush(ch, task.sender, task.message, task.timestamp, renderedBody);     break;
    default:
      LOG("Push", "[Retry] 未知推送类型 %d，丢弃", (int)ch.type);
      return;
  }

  if (ok) {
    LOG("Push", "[Retry] 通道 %s 重试成功", name.c_str());
  } else {
    LOG("Push", "[Retry] 通道 %s 重试失败，重新入队", name.c_str());
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if ((int)s_retryQueue.size() < PUSH_RETRY_QUEUE_MAX) {
      s_retryQueue.push(task);
    } else {
      LOG("Push", "[Retry] 队列已满，丢弃失败任务（通道 %s）", name.c_str());
    }
    xSemaphoreGive(s_mutex);
  }
}

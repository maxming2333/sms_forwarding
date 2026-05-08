#include "push_queue.h"
#include "push.h"
#include "../logger/logger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// 堆上分配的消息条目；通过指针传递，避免 FreeRTOS 队列对 String 做 memcpy
struct PushQueueItem {
  String      sender;
  String      message;
  String      timestamp;
  MsgTypeInfo msgType;
};

static QueueHandle_t  s_queue      = nullptr;
static unsigned long  s_lastSendMs = 0;   // 上次 executeChain() 完成的时刻
static bool           s_isBusy     = false;  // executeChain() 执行期间为 true（防御性标志）

void PushQueue::init() {
  s_queue = xQueueCreate(PUSH_QUEUE_MAX, sizeof(PushQueueItem*));
}

void PushQueue::enqueue(const String& sender, const String& message,
                        const String& timestamp, const MsgTypeInfo& msgType) {
  if (!s_queue) return;

  // 队满时丢弃最旧条目，为新消息腾出空间
  if (uxQueueSpacesAvailable(s_queue) == 0) {
    PushQueueItem* old = nullptr;
    if (xQueueReceive(s_queue, &old, 0) == pdTRUE && old) {
      LOG("PushQ", "队列已满，丢弃最旧消息 from=%s", old->sender.c_str());
      delete old;
    }
  }

  PushQueueItem* item = new PushQueueItem{sender, message, timestamp, msgType};
  if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
    LOG("PushQ", "入队失败，丢弃消息 from=%s", sender.c_str());
    delete item;
  } else {
    LOG("PushQ", "入队成功，当前队列深度: %u", uxQueueMessagesWaiting(s_queue));
  }
}

void PushQueue::tick() {
  if (!s_queue) return;

  // 距上次推送链完成不足 PUSH_QUEUE_INTERVAL_MS，等待下一次 loop 再处理
  if (s_lastSendMs > 0 && millis() - s_lastSendMs < PUSH_QUEUE_INTERVAL_MS) return;

  PushQueueItem* item = nullptr;
  if (xQueueReceive(s_queue, &item, 0) != pdTRUE || !item) return;

  UBaseType_t remaining = uxQueueMessagesWaiting(s_queue);
  LOG("PushQ", "出队执行推送，队列剩余: %u 条", remaining);
  Push::executeChain(item->sender, item->message, item->timestamp, item->msgType);
  s_lastSendMs = millis();
  delete item;
}

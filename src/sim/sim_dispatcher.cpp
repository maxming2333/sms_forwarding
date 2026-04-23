#include "sim_dispatcher.h"
#include <Arduino.h>
#include <new>
#include "logger.h"

// ---------- 模块内部静态全局变量 ----------

static QueueHandle_t   s_queue       = nullptr;
static TaskHandle_t    s_task        = nullptr;
static SimUrcCallback  s_urcCb       = nullptr;
static SimCmdSlot*     s_activeCmd   = nullptr;
static unsigned long   s_cmdStartMs  = 0;

// 保护 SimCmdSlot::refCount 的自旋锁（跨核安全）
static portMUX_TYPE    s_slotMux     = portMUX_INITIALIZER_UNLOCKED;

// ---------- 内部：引用计数释放 ----------

// 每个 SimCmdSlot 由调用方和 reader task 各持一个引用（refCount=2）。
// 任意一方完成时调用此函数；当引用计数归零时负责释放信号量和内存。
static void releaseSlot(SimCmdSlot* slot) {
    if (slot == nullptr) return;
    int remaining;
    taskENTER_CRITICAL(&s_slotMux);
    remaining = --slot->refCount;
    taskEXIT_CRITICAL(&s_slotMux);
    if (remaining == 0) {
        vSemaphoreDelete(slot->doneSem);
        delete slot;
    }
}

// CMT PDU 行检测状态（是否等待 PDU 数据行）
static bool            s_waitingPdu  = false;

// ---------- 内部 URC 识别 ----------

static bool isUrcLine(const String& line) {
    if (line.equals("RING"))                   return true;
    if (line.startsWith("+CLIP:"))             return true;
    if (line.startsWith("+CMT:"))              return true;
    if (s_waitingPdu)                          return true;
    if (line.indexOf("+CPIN:") >= 0)           return true;
    if (line.startsWith("+SIMCARD:"))          return true;
    return false;
}

// ---------- 内部 URC 路由 ----------

static void routeURC(const String& line) {
    if (s_urcCb == nullptr) return;

    if (line.equals("RING")) {
        s_urcCb(SimUrcType::RING, line);
        return;
    }
    if (line.startsWith("+CLIP:")) {
        s_urcCb(SimUrcType::CLIP, line);
        return;
    }
    if (line.startsWith("+CMT:")) {
        s_waitingPdu = true;
        s_urcCb(SimUrcType::CMT, line);
        return;
    }
    if (s_waitingPdu) {
        s_waitingPdu = false;
        s_urcCb(SimUrcType::CMT_PDU, line);
        return;
    }
    if (line.indexOf("+CPIN: READY") >= 0) {
        s_urcCb(SimUrcType::CPIN_READY, line);
        return;
    }
    if (line.indexOf("+CPIN: NOT INSERTED") >= 0 || line.startsWith("+SIMCARD:0")) {
        s_urcCb(SimUrcType::SIM_REMOVE, line);
        return;
    }
}

// ---------- SIM reader task ----------

static void simReaderTask(void*) {
    String lineBuf;
    lineBuf.reserve(400);  // 预分配，SMS PDU hex 串典型长度约 340 字符

    for (;;) {
        // 读取 Serial1 字符，按行处理
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            if (c == '\n') {
                String line = lineBuf;
                lineBuf.remove(0, lineBuf.length());  // 清空内容但保留已分配容量，避免重复 malloc/free

                if (line.length() == 0) continue;

                if (s_activeCmd != nullptr) {
                    // T015: 有活跃指令时先检查是否为 URC 行
                    if (isUrcLine(line)) {
                        LOG("SIM", "[URC-during-cmd] %s", line.c_str());
                        routeURC(line);
                    } else {
                    // 追加到响应缓冲（防溢出）
                    size_t existing = strlen(s_activeCmd->respBuf);
                    size_t avail    = 255 - existing;
                    if (avail > 0) {
                        strncat(s_activeCmd->respBuf, line.c_str(), avail);
                        if (avail > 1) {
                            s_activeCmd->respBuf[existing + avail - 1] = '\0';
                        }
                        // 追加换行分隔
                        size_t cur = strlen(s_activeCmd->respBuf);
                        if (cur < 254) {
                            s_activeCmd->respBuf[cur]     = '\n';
                            s_activeCmd->respBuf[cur + 1] = '\0';
                        }
                    }

                    if (line.indexOf("OK") >= 0) {
                        s_activeCmd->isOk = true;
                        // s_activeCmd 须在 xSemaphoreGive 前清空，确保调用方唤醒时
                        // 不会看到旧指针，防止潜在的重复处理
                        SimCmdSlot* tmp = s_activeCmd;
                        s_activeCmd = nullptr;
                        xSemaphoreGive(tmp->doneSem);
                        releaseSlot(tmp);
                    } else if (line.indexOf("ERROR") >= 0) {
                        s_activeCmd->isOk = false;
                        SimCmdSlot* tmp = s_activeCmd;
                        s_activeCmd = nullptr;
                        xSemaphoreGive(tmp->doneSem);
                        releaseSlot(tmp);
                    }
                    }
                } else {
                    routeURC(line);
                }
            } else if (c != '\r') {
                lineBuf += c;
            }
        }

        // 取下一条命令（若当前无活跃命令）
        if (s_activeCmd == nullptr) {
            SimCmdSlot* ptr = nullptr;
            if (xQueueReceive(s_queue, &ptr, 0) == pdTRUE && ptr != nullptr) {
                s_activeCmd   = ptr;
                s_cmdStartMs  = millis();
                Serial1.println(s_activeCmd->cmd);
            }
        }

        // 超时检测
        if (s_activeCmd != nullptr &&
            millis() - s_cmdStartMs > s_activeCmd->timeoutMs) {
            LOG("SIM", "AT 指令超时: %s", s_activeCmd->cmd);
            s_activeCmd->isOk = false;
            SimCmdSlot* tmp = s_activeCmd;
            s_activeCmd = nullptr;
            xSemaphoreGive(tmp->doneSem);
            releaseSlot(tmp);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------- 公共 API 实现 ----------

void simRegisterUrcCallback(SimUrcCallback cb) {
    s_urcCb = cb;
}

void simDispatcherStart() {
    s_queue = xQueueCreate(SIM_CMD_QUEUE_SIZE, sizeof(SimCmdSlot*));
    if (s_queue == nullptr) {
        LOG("SIM", "simDispatcherStart: 队列创建失败");
        return;
    }
    xTaskCreate(simReaderTask, "sim_reader", SIM_READER_TASK_STACK,
                nullptr, SIM_READER_TASK_PRIORITY, &s_task);
}

bool simSendCommand(const char* cmd, unsigned long timeoutMs,
                    String* outResp, bool prio) {
    if (s_queue == nullptr) return false;

    // 堆上分配，避免调用方超时返回后 reader task 持有悬空栈指针（use-after-free）
    SimCmdSlot* slot = new (std::nothrow) SimCmdSlot();
    if (slot == nullptr) return false;

    strncpy(slot->cmd, cmd, 63);
    slot->cmd[63]      = '\0';
    slot->timeoutMs    = timeoutMs;
    slot->respBuf[0]   = '\0';
    slot->isOk         = false;
    slot->priority     = prio;
    // refCount 须在入队前初始化，确保 reader task 取出时看到正确的值
    slot->refCount     = 2;  // 调用方 + reader task 各持一个引用

    slot->doneSem = xSemaphoreCreateBinary();
    if (slot->doneSem == nullptr) {
        delete slot;
        return false;
    }

    BaseType_t sent;
    if (prio) {
        sent = xQueueSendToFront(s_queue, &slot, pdMS_TO_TICKS(100));
    } else {
        sent = xQueueSendToBack(s_queue, &slot, pdMS_TO_TICKS(100));
    }

    if (sent != pdTRUE) {
        vSemaphoreDelete(slot->doneSem);
        delete slot;
        return false;
    }

    BaseType_t taken = xSemaphoreTake(slot->doneSem, pdMS_TO_TICKS(timeoutMs + 500));

    bool result = false;
    if (taken == pdTRUE) {
        result = slot->isOk;
        if (outResp != nullptr) {
            *outResp = String(slot->respBuf);
        }
    }

    // 释放调用方持有的引用；若 reader task 已完成则此处触发回收，否则由 reader task 完成时回收
    releaseSlot(slot);
    return result;
}

void simPauseReader() {
    if (s_task != nullptr) vTaskSuspend(s_task);
}

void simResumeReader() {
    if (s_task != nullptr) vTaskResume(s_task);
}

bool simDispatcherRunning() {
    return s_queue != nullptr;
}

String simQueryPhoneNumber(unsigned long timeoutMs) {
    String resp;
    bool ok = simSendCommand("AT+CNUM", timeoutMs, &resp, false);
    if (!ok) return "";

    int start = resp.indexOf("+CNUM:");
    if (start < 0) return "";

    // +CNUM: "","13900001234",129
    // 第一对引号为 alpha 名称（可为空），第二对为实际号码
    int q1 = resp.indexOf('"', start);
    int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
    int q3 = (q2 >= 0) ? resp.indexOf('"', q2 + 1) : -1;
    int q4 = (q3 >= 0) ? resp.indexOf('"', q3 + 1) : -1;
    if (q3 < 0 || q4 <= q3) return "";

    return resp.substring(q3 + 1, q4);
}

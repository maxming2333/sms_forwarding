#include "sim_dispatcher.h"
#include <Arduino.h>
#include "logger.h"

// ---------- 模块内部静态全局变量 ----------

static QueueHandle_t   s_queue       = nullptr;
static TaskHandle_t    s_task        = nullptr;
static SemaphoreHandle_t s_directTxnMutex = nullptr;
static SimUrcCallback  s_urcCb       = nullptr;
static SimCmdSlot*     s_activeCmd   = nullptr;
static unsigned long   s_cmdStartMs  = 0;
static volatile bool   s_pauseRequested = false;
static volatile bool   s_readerPaused   = false;
static bool            s_drainAfterTimeout = false;
static unsigned long   s_lastRxMs          = 0;

// CMT PDU 行检测状态（是否等待 PDU 数据行）
static bool            s_waitingPdu  = false;

static constexpr size_t SIM_RESP_BUF_SIZE = 256;
static constexpr size_t SIM_LINE_BUF_MAX  = 512;
static constexpr unsigned long SIM_TIMEOUT_DRAIN_QUIET_MS = 300;

static bool isFinalOkLine(const String& line) {
    String s = line;
    s.trim();
    return s.equals("OK");
}

static bool isFinalErrorLine(const String& line) {
    String s = line;
    s.trim();
    return s.equals("ERROR") || s.startsWith("+CME ERROR") || s.startsWith("+CMS ERROR");
}

// ---------- 内部 URC 识别 ----------

static bool isUrcLine(const String& line) {
    if (line.equals("RING"))                   return true;
    if (line.startsWith("+CLIP:"))             return true;
    if (line.startsWith("+CMT:"))              return true;
    if (s_waitingPdu)                          return true;
    if (line.indexOf("+CPIN:") >= 0)           return true;
    if (line.startsWith("+SIMCARD:"))          return true;
    if (line.startsWith("+CUSD:"))             return true;
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
    if (line.startsWith("+CUSD:")) {
        s_urcCb(SimUrcType::CUSD, line);
        return;
    }
}

static void appendResponseLine(SimCmdSlot* slot, const String& line) {
    size_t existing = strnlen(slot->respBuf, SIM_RESP_BUF_SIZE);
    if (existing >= SIM_RESP_BUF_SIZE - 1) return;

    size_t remaining = (SIM_RESP_BUF_SIZE - 1) - existing;
    size_t copyLen = line.length();
    if (copyLen > remaining) copyLen = remaining;
    if (copyLen > 0) {
        memcpy(slot->respBuf + existing, line.c_str(), copyLen);
        existing += copyLen;
        slot->respBuf[existing] = '\0';
    }

    if (existing < SIM_RESP_BUF_SIZE - 1) {
        slot->respBuf[existing++] = '\n';
        slot->respBuf[existing] = '\0';
    }
}

// ---------- SIM reader task ----------

static void simReaderTask(void*) {
    String lineBuf;
    lineBuf.reserve(400);  // 预分配，SMS PDU hex 串典型长度约 340 字符

    for (;;) {
        if (s_pauseRequested && s_activeCmd == nullptr) {
            s_readerPaused = true;
            while (s_pauseRequested) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            s_readerPaused = false;
        }

        // 读取 Serial1 字符，按行处理
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            s_lastRxMs = millis();
            if (c == '\n') {
                String line = lineBuf;
                lineBuf = "";

                if (line.length() == 0) continue;

                if (s_activeCmd != nullptr) {
                    // T015: 有活跃指令时先检查是否为 URC 行
                    if (isUrcLine(line)) {
                        LOG("SIM", "[URC-during-cmd] %s", line.c_str());
                        routeURC(line);
                    } else {
                    appendResponseLine(s_activeCmd, line);

                    if (isFinalOkLine(line)) {
                        s_activeCmd->isOk = true;
                        xSemaphoreGive(s_activeCmd->doneSem);
                        s_activeCmd = nullptr;
                    } else if (isFinalErrorLine(line)) {
                        s_activeCmd->isOk = false;
                        xSemaphoreGive(s_activeCmd->doneSem);
                        s_activeCmd = nullptr;
                    }
                    }
                } else {
                    routeURC(line);
                }
            } else if (c != '\r') {
                lineBuf += c;
                if (lineBuf.length() > SIM_LINE_BUF_MAX) {
                    LOG("SIM", "串口行超过 %u 字节，已丢弃", (unsigned)SIM_LINE_BUF_MAX);
                    lineBuf = "";
                    s_waitingPdu = false;
                }
            }
        }

        // 取下一条命令（若当前无活跃命令）
        if (s_activeCmd == nullptr && !s_pauseRequested) {
            if (s_drainAfterTimeout) {
                if (millis() - s_lastRxMs < SIM_TIMEOUT_DRAIN_QUIET_MS) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                s_drainAfterTimeout = false;
            }
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
            xSemaphoreGive(s_activeCmd->doneSem);
            s_activeCmd = nullptr;
            s_drainAfterTimeout = true;
            s_lastRxMs = millis();
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
    s_directTxnMutex = xSemaphoreCreateMutex();
    if (s_directTxnMutex == nullptr) {
        LOG("SIM", "simDispatcherStart: 直接事务互斥锁创建失败");
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return;
    }
    xTaskCreate(simReaderTask, "sim_reader", SIM_READER_TASK_STACK,
                nullptr, SIM_READER_TASK_PRIORITY, &s_task);
}

bool simSendCommand(const char* cmd, unsigned long timeoutMs,
                    String* outResp, bool prio) {
    if (s_queue == nullptr) return false;

    SimCmdSlot slot;
    strncpy(slot.cmd, cmd, 63);
    slot.cmd[63]      = '\0';
    slot.timeoutMs    = timeoutMs;
    slot.respBuf[0]   = '\0';
    slot.isOk         = false;
    slot.priority     = prio;

    slot.doneSem = xSemaphoreCreateBinary();
    if (slot.doneSem == nullptr) return false;

    SimCmdSlot* ptr = &slot;
    BaseType_t sent;
    if (prio) {
        sent = xQueueSendToFront(s_queue, &ptr, pdMS_TO_TICKS(100));
    } else {
        sent = xQueueSendToBack(s_queue, &ptr, pdMS_TO_TICKS(100));
    }

    if (sent != pdTRUE) {
        vSemaphoreDelete(slot.doneSem);
        return false;
    }

    // 必须使用 portMAX_DELAY 等待 reader task 给信号量，
    // 不可自行超时：若 simSendCommand 提前返回，栈上的 slot 会被销毁，
    // reader task 之后再 xSemaphoreGive(s_activeCmd->doneSem) 将访问
    // 悬空指针，导致崩溃。reader task 内部已有 timeoutMs 超时机制，
    // 最终一定会 Give 信号量（OK / ERROR / 超时三路均有 Give）。
    xSemaphoreTake(slot.doneSem, portMAX_DELAY);
    vSemaphoreDelete(slot.doneSem);

    if (outResp != nullptr) {
        *outResp = String(slot.respBuf);
    }
    return slot.isOk;
}

bool simPauseReader(unsigned long timeoutMs) {
    if (s_task == nullptr) return true;
    if (s_directTxnMutex == nullptr) return false;
    if (xSemaphoreTake(s_directTxnMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        LOG("SIM", "等待直接串口事务锁超时");
        return false;
    }
    s_pauseRequested = true;
    unsigned long start = millis();
    while (!s_readerPaused) {
        if (millis() - start >= timeoutMs) {
            s_pauseRequested = false;
            xSemaphoreGive(s_directTxnMutex);
            LOG("SIM", "等待 reader 暂停超时");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void simResumeReader() {
    s_pauseRequested = false;
    if (s_directTxnMutex != nullptr) {
        xSemaphoreGive(s_directTxnMutex);
    }
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
    if (q3 >= 0 && q4 > q3) return resp.substring(q3 + 1, q4);

    // 兜底：部分模组 alpha 字段无引号，格式为 +CNUM: ,"13900001234",type
    // 与 /query?type=siminfo 端点使用相同的 ,"  解析逻辑
    int idx = resp.indexOf(",\"", start);
    if (idx >= 0) {
        int ei = resp.indexOf('"', idx + 2);
        if (ei > idx + 2) return resp.substring(idx + 2, ei);
    }
    return "";
}

#include "call.h"
#include "push/push.h"
#include "sms/phone_utils.h"
#include "sim/sim_dispatcher.h"
#include "time/time_module.h"
#include "logger.h"

// ---------- 模块内部静态变量 ----------

static volatile bool          s_pending          = false;
static String                 s_callerNumber     = "未知号码";
static volatile unsigned long s_clipWaitUntilMs  = 0;
static unsigned long          s_lastNotifyMs     = 0;
static bool                   s_clccAttempted    = false;  // 是否已主动发送 AT+CLCC
static unsigned long          s_clccAttemptMs    = 0;       // 计划发送 AT+CLCC 的时间点

// ---------- 内部：发送来电通知 ----------

static void dispatchCallNotification(const String& callerNum) {
    if (phoneMatchesBlacklist(callerNum)) {
        LOG("Call", "黑名单拦截来电，号码: %s", callerNum.c_str());
        return;
    }

    String ts = timeModuleGetDateStr();
    sendPushNotification(callerNum, "来电号码: " + callerNum + "\n时间: " + ts, ts, MsgTypeInfo(MSG_TYPE_CALL));

    s_lastNotifyMs = millis();
    LOG("Call", "来电通知已发送，号码: %s", callerNum.c_str());
}

// ---------- 公共 API ----------

void callInit() {
    s_pending         = false;
    s_callerNumber    = "未知号码";
    s_clipWaitUntilMs = 0;
    s_lastNotifyMs    = 0;
    s_clccAttempted   = false;
    s_clccAttemptMs   = 0;
}

void callHandleRING() {
    if (millis() - s_lastNotifyMs < CALL_DEDUP_MS) {
        LOG("Call", "防抖：忽略 RING（%lu ms 内已通知）", CALL_DEDUP_MS);
        return;
    }
    s_pending         = true;
    s_callerNumber    = "未知号码";
    s_clipWaitUntilMs = millis() + CALL_CLIP_WAIT_MS;
    s_clccAttempted   = false;
    s_clccAttemptMs   = millis() + CALL_CLCC_DELAY_MS;
    LOG("Call", "RING 检测，等待 +CLIP（%lu ms），%lu ms 后主动查询 AT+CLCC",
        CALL_CLIP_WAIT_MS, CALL_CLCC_DELAY_MS);
}

void callHandleCLIP(const String& line) {
    if (!s_pending) return;

    // 解析 +CLIP: "号码",129,...
    int q1 = line.indexOf('"');
    int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) {
        String num = line.substring(q1 + 1, q2);
        if (num.length() > 0) {
            s_callerNumber = num;
        } else {
            s_callerNumber = "号码保密";
        }
    }

    s_pending = false;
    dispatchCallNotification(s_callerNumber);
}

// 解析 AT+CLCC 响应，提取 MT（来电方向=1）通话的号码
// 响应格式: +CLCC: idx,dir,stat,mode,mpty,"number",type
static String parseCLCC(const String& resp) {
    int pos = 0;
    while (true) {
        int clccIdx = resp.indexOf("+CLCC:", pos);
        if (clccIdx < 0) break;
        // 跳过字段 idx, dir
        int commaAfterIdx = resp.indexOf(',', clccIdx + 6);
        if (commaAfterIdx < 0) break;
        // dir: 0=MO, 1=MT(来电)
        int commaAfterDir = resp.indexOf(',', commaAfterIdx + 1);
        if (commaAfterDir < 0) break;
        String dirStr = resp.substring(commaAfterIdx + 1, commaAfterDir);
        dirStr.trim();
        // 找引号内的号码
        int q1 = resp.indexOf('"', commaAfterDir);
        int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
        if (q1 >= 0 && q2 > q1) {
            String num = resp.substring(q1 + 1, q2);
            if (num.length() > 0) return num;
        }
        pos = clccIdx + 6;
    }
    return "";
}

void callTick() {
    if (!s_pending) return;
    unsigned long now = millis();

    // 主动查询：RING 到来后 CALL_CLCC_DELAY_MS 毫秒，被动 +CLIP 尚未到，发送 AT+CLCC
    if (!s_clccAttempted && now >= s_clccAttemptMs) {
        s_clccAttempted = true;
        LOG("Call", "主动发送 AT+CLCC 查询来电号码");
        String resp;
        // 优先级插队，避免被其他 AT 命令阻塞
        simSendCommand("AT+CLCC", 3000, &resp, true);
        LOG("Call", "AT+CLCC 响应: %s", resp.c_str());
        String num = parseCLCC(resp);
        if (num.length() > 0) {
            LOG("Call", "AT+CLCC 成功获取来电号码: %s", num.c_str());
            s_callerNumber = num;
            s_pending = false;
            dispatchCallNotification(s_callerNumber);
            return;
        }
        LOG("Call", "AT+CLCC 未获取到号码（可能已挂断或格式不符），继续等待 +CLIP");
    }

    // 超时兜底：15 秒内既无 +CLIP 也无 CLCC 号码
    if (now >= s_clipWaitUntilMs) {
        s_pending = false;
        dispatchCallNotification(s_callerNumber);  // 此时 s_callerNumber 仍为 "未知号码"
    }
}

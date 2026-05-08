#include "call.h"
#include "push/push.h"
#include "sms/phone_utils.h"
#include "sim/sim_dispatcher.h"
#include "time/time_sync.h"
#include "../logger/logger.h"

volatile bool          Call::s_pending          = false;
volatile bool          Call::s_dispatchPending  = false;
String                 Call::s_callerNumber     = "未知号码";
volatile unsigned long Call::s_clipWaitUntilMs  = 0;
unsigned long          Call::s_lastNotifyMs     = 0;
bool                   Call::s_clccAttempted    = false;
unsigned long          Call::s_clccAttemptMs    = 0;

void Call::dispatch(const String& callerNum) {
  if (phoneMatchesBlacklist(callerNum)) {
    LOG("Call", "黑名单拦截来电，号码: %s", callerNum.c_str());
    return;
  }
  String ts = TimeSync::dateStr();
  Push::send(callerNum, "来电号码: " + callerNum + "\n时间: " + ts, ts, MsgTypeInfo(MSG_TYPE_CALL));
  s_lastNotifyMs = millis();
  LOG("Call", "来电通知已发送，号码: %s", callerNum.c_str());
}

void Call::init() {
  s_pending         = false;
  s_dispatchPending = false;
  s_callerNumber    = "未知号码";
  s_clipWaitUntilMs = 0;
  s_lastNotifyMs    = 0;
  s_clccAttempted   = false;
  s_clccAttemptMs   = 0;
}

void Call::handleRING() {
  if (millis() - s_lastNotifyMs < DEDUP_MS) {
    LOG("Call", "防抖：忽略 RING（%lu ms 内已通知）", DEDUP_MS);
    return;
  }
  s_pending         = true;
  s_dispatchPending = false;
  s_callerNumber    = "未知号码";
  s_clipWaitUntilMs = millis() + CLIP_WAIT_MS;
  s_clccAttempted   = false;
  s_clccAttemptMs   = millis() + CLCC_DELAY_MS;
  LOG("Call", "RING 检测，等待 +CLIP（%lu ms），%lu ms 后主动查询 AT+CLCC", CLIP_WAIT_MS, CLCC_DELAY_MS);
}

void Call::handleCLIP(const String& line) {
  if (!s_pending) {
    return;
  }
  int q1 = line.indexOf('"');
  int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
  if (q1 >= 0 && q2 > q1) {
    String num = line.substring(q1 + 1, q2);
    s_callerNumber = num.length() > 0 ? num : String("号码保密");
  }
  s_pending         = false;
  s_dispatchPending = true;
}

String Call::parseCLCC(const String& resp) {
  int pos = 0;
  while (true) {
    int clccIdx = resp.indexOf("+CLCC:", pos);
    if (clccIdx < 0) {
      break;
    }
    int commaAfterIdx = resp.indexOf(',', clccIdx + 6);
    if (commaAfterIdx < 0) {
      break;
    }
    int commaAfterDir = resp.indexOf(',', commaAfterIdx + 1);
    if (commaAfterDir < 0) {
      break;
    }
    int q1 = resp.indexOf('"', commaAfterDir);
    int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) {
      String num = resp.substring(q1 + 1, q2);
      if (num.length() > 0) {
        return num;
      }
    }
    pos = clccIdx + 6;
  }
  return "";
}

void Call::tick() {
  if (s_dispatchPending) {
    s_dispatchPending = false;
    dispatch(s_callerNumber);
    return;
  }
  if (!s_pending) {
    return;
  }
  unsigned long now = millis();

  if (!s_clccAttempted && now >= s_clccAttemptMs) {
    s_clccAttempted = true;
    LOG("Call", "主动发送 AT+CLCC 查询来电号码");
    String resp;
    SimDispatcher::sendCommand("AT+CLCC", 3000, &resp, true);
    LOG("Call", "AT+CLCC 响应: %s", resp.c_str());
    String num = parseCLCC(resp);
    if (num.length() > 0) {
      LOG("Call", "AT+CLCC 成功获取来电号码: %s", num.c_str());
      s_callerNumber = num;
      s_pending = false;
      dispatch(s_callerNumber);
      return;
    }
    LOG("Call", "AT+CLCC 未获取到号码（可能已挂断或格式不符），继续等待 +CLIP");
  }

  if (now >= s_clipWaitUntilMs) {
    s_pending = false;
    dispatch(s_callerNumber);
  }
}

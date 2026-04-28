#include "sim.h"
#include "sim_dispatcher.h"
#include "call/call.h"
#include "sms/sms.h"
#include "logger.h"
#include "config/config.h"
#include "push/push.h"
#include "time/time_module.h"
#include <esp_task_wdt.h>

// ---------- internal state ----------

static SimState s_state      = SIM_UNKNOWN;
static bool     s_needReinit = false;

// SIM info cache (populated after SIM_READY)
static String   s_carrier    = "未知";
static String   s_signal     = "未知";
static String   s_phoneNum   = "未知";

// ---------- US021: 本机号码重试状态 ----------
static bool          s_numberReady   = false;
static unsigned long s_numRetryNext  = 0;
static constexpr unsigned long SIM_NUMBER_RETRY_INTERVAL_MS = 5000;

// ---------- US2: 数据流量状态机 ----------

enum TrafficState {
  TS_IDLE,
  TS_PENDING,
  TS_WAIT_RETRY,
  TS_DONE,
  TS_TIMED_OUT
};

struct TrafficSM {
  TrafficState  state       = TS_IDLE;
  unsigned long triggerMs   = 0;
  unsigned long lastActionMs = 0;
};

static TrafficSM s_tsm;

// ---------- T005: AT helpers ----------

static bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  if (simDispatcherRunning()) {
    return simSendCommand(cmd, timeout, nullptr, false);
  }
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp;
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read(); resp += c;
      if (resp.indexOf("OK")    >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

static bool runInitStep(const char* cmd, unsigned long timeout, int maxRetry, const char* stepName) {
  for (int i = 0; i < maxRetry; i++) {
    if (sendATandWaitOK(cmd, timeout)) {
      LOG("SIM", "%s 成功", stepName);
      return true;
    }
    LOG("SIM", "%s 失败，重试 %d/%d", stepName, i + 1, maxRetry);
    delay(300);
    esp_task_wdt_reset();
  }
  LOG("SIM", "%s 最终失败", stepName);
  return false;
}

// ---------- US2: simTrafficTick — 数据流量控制（通过调度器发送 AT 指令）----------

static void simTrafficTick() {
  switch (s_tsm.state) {
    case TS_PENDING: {
      // 检查总超时
      if (millis() - s_tsm.triggerMs > 300000) {
        LOG("SIM", "数据流量: 总超时 5 分钟，放弃重试");
        s_tsm.state = TS_TIMED_OUT;
        break;
      }
      String cmd = "AT+CGACT=";
      cmd += config.dataTraffic ? "1" : "0";
      cmd += ",1";
      LOG("SIM", "数据流量: 发送 %s", cmd.c_str());
      bool ok = simSendCommand(cmd.c_str(), 6000, nullptr, false);
      if (ok) {
        LOG("SIM", "数据流量: AT+CGACT 成功");
        s_tsm.state = TS_DONE;
      } else {
        // 指令失败时查询当前状态，若上下文已处于目标状态则视为成功
        // （软重启后上下文可能已被关闭，再次关闭会返回 ERROR）
        String cgactResp;
        bool alreadyInDesiredState = false;
        if (simSendCommand("AT+CGACT?", 3000, &cgactResp, false)) {
          String desiredPattern = String("+CGACT: 1,") + (config.dataTraffic ? "1" : "0");
          if (cgactResp.indexOf(desiredPattern) >= 0) {
            alreadyInDesiredState = true;
          }
        }
        if (alreadyInDesiredState) {
          LOG("SIM", "数据流量: 上下文已处于目标状态，无需重试");
          s_tsm.state = TS_DONE;
        } else {
          LOG("SIM", "数据流量: AT+CGACT 失败或超时，3s 后重试");
          s_tsm.lastActionMs = millis();
          s_tsm.state        = TS_WAIT_RETRY;
        }
      }
      break;
    }
    case TS_WAIT_RETRY: {
      if (millis() - s_tsm.triggerMs > 300000) {
        LOG("SIM", "数据流量: 总超时 5 分钟，放弃重试");
        s_tsm.state = TS_TIMED_OUT;
      } else if (millis() - s_tsm.lastActionMs >= 3000) {
        s_tsm.state = TS_PENDING;
      }
      break;
    }
    case TS_IDLE:
    case TS_DONE:
    case TS_TIMED_OUT:
    default:
      break;
  }
}

// ---------- T006 helper: CEREG polling ----------

static bool waitCEREG() {
  String resp;
  if (simDispatcherRunning()) {
    simSendCommand("AT+CEREG?", 2000, &resp, false);
  } else {
    Serial1.println("AT+CEREG?");
    unsigned long start = millis();
    while (millis() - start < 2000) {
      while (Serial1.available()) { char c = Serial1.read(); resp += c; }
      if (resp.indexOf("+CEREG:") >= 0) break;
    }
  }
  if (resp.indexOf("+CEREG:") >= 0) {
    if (resp.indexOf(",1") >= 0 || resp.indexOf(",5") >= 0) return true;
    if (resp.indexOf(",0") >= 0 || resp.indexOf(",2") >= 0 ||
        resp.indexOf(",3") >= 0 || resp.indexOf(",4") >= 0) return false;
  }
  return false;
}

// ---------- T006 helper: 模组配置（CNMI/CMGF/CLIP，必须在 CFUN=1 射频恢复后调用，
//            CFUN=4→1 切换会重置这些非持久化设置）----------

static bool runModemConfig() {
  if (!runInitStep("AT+CNMI=2,2,0,0,0", 1000, 3, "CNMI")) return false;
  if (!runInitStep("AT+CMGF=0", 1000, 3, "CMGF")) return false;
  runInitStep("AT+CLIP=1", 1000, 3, "CLIP");  // 启用主叫号码上报，失败不阻断初始化
  return true;
}

// ---------- T006 helper: 等待网络注册（需射频在线，在 CFUN=1 之后调用）----------

static bool runNetworkWait() {
  // 轮询 CEREG，最多 30 次 × 2s = 60s
  for (int i = 0; i < 30; i++) {
    if (waitCEREG()) {
      LOG("SIM", "网络已注册");
      return true;
    }
    LOG("SIM", "等待网络注册... %d/30", i + 1);
    // 每次失败后等待 2 秒再重试，分段喂狗避免 TWDT 触发
    unsigned long waitStart = millis();
    while (millis() - waitStart < 2000) {
      delay(300);
      esp_task_wdt_reset();
    }
  }
  LOG("SIM", "网络注册超时");
  return false;
}

// ---------- T006 helper: 完整初始化序列（热插入路径使用，不含 CFUN 门控）----------

static bool runInitSequence() {
  return runModemConfig() && runNetworkWait();
}

// ---------- T006: simInit ----------

void simInit() {
  // 检测 SIM 卡是否存在（AT+CPIN? 3000ms）
  while (Serial1.available()) Serial1.read();
  Serial1.println("AT+CPIN?");
  unsigned long start = millis();
  String resp;
  bool gotResponse = false;
  while (millis() - start < 3000) {
    while (Serial1.available()) {
      char c = Serial1.read(); resp += c;
    }
    if (resp.indexOf("+CPIN: READY") >= 0) {
      gotResponse = true; break;
    }
    if (resp.indexOf("ERROR") >= 0 || resp.indexOf("NOT INSERTED") >= 0) {
      break;
    }
  }

  if (!gotResponse || resp.indexOf("+CPIN: READY") < 0) {
    s_state = SIM_NOT_INSERTED;
    LOG("SIM", "未检测到 SIM 卡，跳过初始化");
    return;
  }

  LOG("SIM", "SIM 卡就绪，开始初始化");
  s_state = SIM_INITIALIZING;

  // 步骤1: 所有准备完成，恢复射频上线
  // 注意：CFUN=4→1 切换会重置 CNMI/CMGF/CLIP 等非持久化设置，
  //       因此 runModemConfig 必须放在 CFUN=1 之后立即执行
  sendATandWaitOK("AT+CFUN=1", 3000);
  LOG("SIM", "射频已恢复 (CFUN=1)");

  delay(300);

  // 步骤2: 查询本机号码（AT+CNUM 读 SIM 卡 EF 文件，不需要射频）
  {
    String num = simQueryPhoneNumber(3000);
    if (num.length() > 0) {
      s_phoneNum    = num;
      s_numberReady = true;
      LOG("SIM", "首次号码查询成功: %s", num.c_str());
    } else {
      s_numberReady  = false;
      s_numRetryNext = millis() + SIM_NUMBER_RETRY_INTERVAL_MS;
      LOG("SIM", "首次号码查询失败，%lu ms 后再试", SIM_NUMBER_RETRY_INTERVAL_MS);
    }
  }

  // 步骤3: 重新配置模组参数（CFUN=1 会重置这些设置，必须在此补全）
  if (!runModemConfig()) {
    s_state = SIM_INIT_FAILED;
    LOG("SIM", "SIM 初始化失败（模组配置阶段）");
    return;
  }

  // 步骤4: 等待网络注册
  LOG("SIM", "等待网络注册");
  if (runNetworkWait()) {
    // 步骤5: 网络就绪后尝试 SIM 时间同步
    timeModuleSyncFromSIM();
    s_state            = SIM_READY;
    s_tsm.state        = TS_PENDING;
    s_tsm.triggerMs    = millis();
    s_tsm.lastActionMs = millis();
    LOG("SIM", "SIM 初始化成功");
  } else {
    s_state = SIM_INIT_FAILED;
    LOG("SIM", "SIM 初始化失败");
  }
}

// ---------- simGetState ----------

SimState simGetState() {
  return s_state;
}

// ---------- simHandleURC ----------

void simHandleURC(const String& line) {
  if (line.indexOf("+CPIN: READY") >= 0) {
    if (s_state != SIM_READY && s_state != SIM_INITIALIZING) {
      s_needReinit = true;
      LOG("SIM", "检测到 SIM 就绪 URC，等待重新初始化");
      if (config.simNotifyEnabled) {
        sendPushNotification("设备", "SIM 卡已就绪，设备将重新初始化 SIM 模块", timeModuleGetDateStr(), MsgTypeInfo(MSG_TYPE_SIM));
      }
    }
    return;
  }

  if (line.indexOf("+CPIN: NOT INSERTED") >= 0 || line.indexOf("+SIMCARD:0") >= 0) {
    SimState prev = s_state;
    s_state      = SIM_NOT_INSERTED;
    s_needReinit = false;
    s_tsm        = TrafficSM{};
    LOG("SIM", "SIM 卡已拔出，状态已清除");
    if (config.simNotifyEnabled && prev == SIM_READY) {
      sendPushNotification("设备", "SIM 卡已拔出，当前状态：未插入", timeModuleGetDateStr(), MsgTypeInfo(MSG_TYPE_SIM));
    }
    return;
  }
}

// ---------- simTick ----------

void simTick() {
  if (s_needReinit) {
    s_needReinit = false;
    s_state      = SIM_INITIALIZING;
    LOG("SIM", "开始热插入重新初始化");
    if (runInitSequence()) {
      s_state            = SIM_READY;
      s_tsm.state        = TS_PENDING;
      s_tsm.triggerMs    = millis();
      s_tsm.lastActionMs = millis();
      LOG("SIM", "热插入初始化成功");
    } else {
      s_state = SIM_INIT_FAILED;
      LOG("SIM", "热插入初始化失败");
    }
  }

  simTrafficTick();

  // T008: 本机号码重试查询（独立于 SIM 状态，只要调度器在线且号码未就绪就持续重试）
  if (!s_numberReady && simDispatcherRunning() && millis() >= s_numRetryNext) {
    LOG("SIM", "本机号码重试查询...");
    String num = simQueryPhoneNumber(3000);
    if (num.length() > 0) {
      s_phoneNum     = num;
      s_numberReady  = true;
      s_numRetryNext = ULONG_MAX;
      LOG("SIM", "本机号码更新: %s", num.c_str());
    } else {
      s_numberReady  = false;
      s_numRetryNext = millis() + SIM_NUMBER_RETRY_INTERVAL_MS;
      LOG("SIM", "本机号码重试失败，%lu ms 后再试", SIM_NUMBER_RETRY_INTERVAL_MS);
    }
  }
}

// ---------- simFetchInfo ----------

static String normalizeCarrier(const String& raw) {
  String lower = raw;
  lower.trim();
  lower.toLowerCase();
  if (lower == "cmcc" || lower == "china mobile" || lower == "46000" ||
      lower == "46002" || lower == "46007" || lower == "46008" || lower == "46020") {
    return "中国移动";
  }
  if (lower == "cucc" || lower == "china unicom" || lower == "chn-unicom" ||
      lower == "46001" || lower == "46006" || lower == "46009") {
    return "中国联通";
  }
  if (lower == "ctcc" || lower == "china telecom" || lower == "chn-ct" ||
      lower == "46003" || lower == "46005" || lower == "46011") {
    return "中国电信";
  }
  return raw;
}

void simFetchInfo() {
  {
    String resp;
    simSendCommand("AT+COPS?", 3000, &resp, false);
    int start = resp.indexOf("+COPS:");
    if (start >= 0) {
      int q1 = resp.indexOf('"', start);
      int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
      if (q1 >= 0 && q2 > q1) {
        s_carrier = normalizeCarrier(resp.substring(q1 + 1, q2));
        LOG("SIM", "运营商: %s", s_carrier.c_str());
      }
    }
  }
  {
    String resp;
    simSendCommand("AT+CSQ", 2000, &resp, false);
    int start = resp.indexOf("+CSQ:");
    if (start >= 0) {
      int csq = -1;
      sscanf(resp.c_str() + start + 5, " %d", &csq);
      if (csq >= 0 && csq != 99) {
        int dbm = -113 + 2 * csq;
        char buf[24];
        snprintf(buf, sizeof(buf), "%ddBm", dbm);
        s_signal = String(buf);
        LOG("SIM", "信号强度: %s", s_signal.c_str());
      } else {
        s_signal = "未知";
      }
    }
  }
  {
    String num = simQueryPhoneNumber(3000);
    if (num.length() > 0) {
      s_phoneNum     = num;
      s_numberReady  = true;
      s_numRetryNext = ULONG_MAX;
      LOG("SIM", "本机号码: %s", s_phoneNum.c_str());
    } else {
      s_numberReady  = false;
      s_numRetryNext = millis() + SIM_NUMBER_RETRY_INTERVAL_MS;
    }
  }
}

// ---------- Getter functions ----------

String simGetCarrier()  { return s_carrier; }
String simGetSignal()   { return s_signal; }
String simGetPhoneNum() { return s_phoneNum; }
bool simIsNumberReady() { return s_numberReady; }

// ---------- URC 路由（由 SIM reader task 回调调用） ----------

static void onUrc(SimUrcType type, const String& line) {
  switch (type) {
    case SimUrcType::RING:       callHandleRING();          break;
    case SimUrcType::CLIP:       callHandleCLIP(line);      break;
    case SimUrcType::CMT:        smsHandleCMTHeader();      break;
    case SimUrcType::CMT_PDU:    smsHandlePDU(line);        break;
    case SimUrcType::CPIN_READY: simHandleURC(line);        break;
    case SimUrcType::SIM_REMOVE: simHandleURC(line);        break;
    default:                                                 break;
  }
}

// ---------- simStartReaderTask ----------

void simStartReaderTask() {
  smsStartProcTask();          // 先启动 sms_proc 任务和队列
  simRegisterUrcCallback(onUrc);
  simDispatcherStart();
  LOG("SIM", "SIM reader task 已启动");
}

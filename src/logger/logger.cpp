#include "logger.h"
#include <LittleFS.h>
#include <memory>
#include <time.h>

// ---------- 私有状态 ----------

static bool         s_serialEnabled  = true;
static bool         s_storageEnabled = false;

// 不写存储的模块黑名单（指向字符串字面量，生命周期由调用方保证）
static const char*  s_fileSkip[Logger::FILE_SKIP_MAX] = {};
static int          s_fileSkipCount = 0;

// 去重状态：相邻两条 module+msg 完全一致时直接丢弃，只保留串口输出
static char   s_lastModule[16]  = {};
static char   s_lastMsg[256]    = {};

// 文件批量写入暂存区（减少 Flash 刷写次数）
static char    s_fileBuf[Logger::FLUSH_LINES][Logger::LINE_LEN];
static int     s_filePending   = 0;
static time_t  s_lastFlushTime = 0;

// ---------- 文件轮转 ----------

// 若写入 pendingBytes 后文件超限，则从文件顶部裁掉等量字节，保持文件大小恒定。
// 调用方在此之后再以追加模式写入 pendingBytes，净增量为零。
static void rotateIfNeeded(size_t pendingBytes) {
  if (!LittleFS.exists(Logger::FILE_PATH) || pendingBytes == 0) return;

  File probe = LittleFS.open(Logger::FILE_PATH, "r");
  size_t sz = probe ? probe.size() : 0;
  probe.close();
  if (sz + pendingBytes <= Logger::FILE_MAX_BYTES) return;

  // 从顶部跳过 pendingBytes，再对齐到下一个完整行边界
  File old = LittleFS.open(Logger::FILE_PATH, "r");
  if (!old) return;
  // seek 到 pendingBytes 处，再向后跳到下一个行边界（确保保留内容第一行完整）
  old.seek(pendingBytes);
  old.readStringUntil('\n');  // 丢弃被 seek 截断的残行尾部（到 '\n' 为止）
  String tail;
  tail.reserve(sz > pendingBytes ? sz - pendingBytes : 0);
  while (old.available()) tail += (char)old.read();
  old.close();

  File nf = LittleFS.open(Logger::FILE_PATH, "w");
  if (nf) { nf.print(tail); nf.close(); }
}

// ---------- 私有辅助函数 ----------

// 串口输出
static void writeToSerial(const char* line) {
  Serial.print(line);
  Serial.print("\r\n");
}

// 去重检查并更新状态，返回 true 表示是重复条目
static bool isDuplicate(const char* module, const char* msg) {
  bool dup = (s_lastModule[0] != '\0')
          && (strcmp(module, s_lastModule) == 0)
          && (strcmp(msg,    s_lastMsg)    == 0);
  strncpy(s_lastModule, module, sizeof(s_lastModule) - 1);
  s_lastModule[sizeof(s_lastModule) - 1] = '\0';
  strncpy(s_lastMsg, msg, sizeof(s_lastMsg) - 1);
  s_lastMsg[sizeof(s_lastMsg) - 1] = '\0';
  return dup;
}

// 检查模块是否在存储黑名单中（命中则跳过缓冲和文件）
static bool shouldSkipStorage(const char* module) {
  for (int i = 0; i < s_fileSkipCount; i++) {
    if (strcmp(module, s_fileSkip[i]) == 0) return true;
  }
  return false;
}

// 将暂存区所有行批量写入 LittleFS
static void flushToFile() {
  if (s_filePending == 0) return;
  // 计算本次实际写入字节数（每行内容 + println 的 '\n'）
  size_t pendingBytes = 0;
  for (int i = 0; i < s_filePending; i++) {
    pendingBytes += strlen(s_fileBuf[i]) + 1;
  }
  rotateIfNeeded(pendingBytes);
  File f = LittleFS.open(Logger::FILE_PATH, "a");
  if (f) {
    for (int i = 0; i < s_filePending; i++) f.println(s_fileBuf[i]);
    f.close();
  }
  s_filePending   = 0;
  s_lastFlushTime = time(nullptr);
}

// 写入存储：去重、黑名单过滤，再加入暂存区并按需批量刷写
static void writeToStorage(const char* line, const char* module, const char* msg, time_t now) {
  if (shouldSkipStorage(module)) return;
  if (isDuplicate(module, msg)) return;
  if (s_filePending < Logger::FLUSH_LINES) {
    strncpy(s_fileBuf[s_filePending], line, Logger::LINE_LEN - 1);
    s_fileBuf[s_filePending][Logger::LINE_LEN - 1] = '\0';
    s_filePending++;
    // 写入第一条时记录起始时间，确保超时 flush 能正确触发
    if (s_filePending == 1) s_lastFlushTime = now;
  }
  bool timeFlush = (now - s_lastFlushTime >= Logger::FLUSH_INTERVAL_SEC);
  if (s_filePending >= Logger::FLUSH_LINES || timeFlush) {
    flushToFile();
  }
}

// ---------- 公有方法 ----------

void Logger::init(const char* const* skipModules) {
  s_fileSkipCount = 0;
  if (skipModules) {
    for (int i = 0; skipModules[i] != nullptr && i < FILE_SKIP_MAX; i++) {
      s_fileSkip[s_fileSkipCount++] = skipModules[i];
    }
  }
  // 每次 init（含软重启）都清空文件和暂存缓冲，确保从干净状态开始
  clearFile();
}

void Logger::setSerialEnabled(bool enabled) {
  s_serialEnabled = enabled;
}

void Logger::setStorageEnabled(bool enabled) {
  s_storageEnabled = enabled;
}

void Logger::flush() {
  flushToFile();
}

void Logger::clearFile() {
  // 丢弃暂存区，重置刷写计时
  s_filePending   = 0;
  s_lastFlushTime = 0;
  LittleFS.remove(FILE_PATH);
  // 重置去重状态
  s_lastModule[0] = '\0';
  s_lastMsg[0]    = '\0';
}

void Logger::log(const char* module, const char* fmt, ...) {
  // 格式化时间戳和消息
  char ts[20];
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);

  va_list args;
  va_start(args, fmt);
  char msg[256];
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  // 统一格式化日志行，供串口 / 缓冲 / 文件共用
  char line[LINE_LEN];
  snprintf(line, sizeof(line), "[%s] [%-*s] %s", ts, MODULE_PAD, module, msg);

  // --- 串口 ---
  if (s_serialEnabled) {
    writeToSerial(line);
  }

  // --- 文件（经暂存缓冲批量刷写，减少 Flash 磨损）---
  if (!s_storageEnabled) return;
  writeToStorage(line, module, msg, now);
}

std::shared_ptr<File> Logger::dumpStream(int n) {
  // 先落盘，确保文件包含全部日志（暂存区至多 FLUSH_LINES 条，写放大极小）
  flushToFile();

  if (!LittleFS.exists(FILE_PATH)) return nullptr;
  auto fp = std::make_shared<File>(LittleFS.open(FILE_PATH, "r"));
  if (!*fp) return nullptr;

  size_t sz      = fp->size();
  size_t readMax = (size_t)n * LINE_LEN;
  if (sz > readMax) {
    fp->seek(sz - readMax);
    fp->readStringUntil('\n');  // 丢弃被 seek 截断的残行，确保从完整行开始
  }
  return fp;  // 调用方析构 shared_ptr 时 File 析构函数自动 close()
}

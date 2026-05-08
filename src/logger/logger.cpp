#include "logger.h"
#include <LittleFS.h>
#include <time.h>

// ---------- 私有状态 ----------

static char         s_lines[Logger::BUF_LINES][Logger::LINE_LEN];
static int          s_head        = 0;
static int          s_count       = 0;
static bool         s_fileEnabled = false;

// 不写文件的模块黑名单（指向字符串字面量，生命周期由调用方保证）
static const char*  s_fileSkip[Logger::FILE_SKIP_MAX] = {};
static int          s_fileSkipCount = 0;

// 去重状态：相邻两条 module+msg 完全一致时，用新时间戳原地替换而不追加
static char   s_lastModule[16]  = {};
static char   s_lastMsg[256]    = {};
static size_t s_lastFileOffset  = 0;  // 上一条写入文件的起始字节位置

// ---------- 文件轮转 ----------

static void rotateIfNeeded() {
  if (!LittleFS.exists(Logger::FILE_PATH)) return;

  File probe = LittleFS.open(Logger::FILE_PATH, "r");
  size_t sz = probe ? probe.size() : 0;
  probe.close();
  if (sz <= Logger::FILE_MAX_BYTES) return;

  // 读出后 FILE_KEEP_BYTES 字节，按整行写回
  File old = LittleFS.open(Logger::FILE_PATH, "r");
  if (!old) return;
  old.seek(sz - Logger::FILE_KEEP_BYTES);
  old.readStringUntil('\n');  // 跳过可能截断的首行
  String tail;
  tail.reserve(Logger::FILE_KEEP_BYTES);
  while (old.available()) tail += (char)old.read();
  old.close();

  File nf = LittleFS.open(Logger::FILE_PATH, "w");
  if (nf) { nf.print(tail); nf.close(); }
}

// ---------- 公有方法 ----------

void Logger::enableFile(const char* const* fileSkipModules) {
  s_fileEnabled    = true;
  s_fileSkipCount  = 0;
  if (fileSkipModules) {
    for (int i = 0; fileSkipModules[i] != nullptr && i < Logger::FILE_SKIP_MAX; i++) {
      s_fileSkip[s_fileSkipCount++] = fileSkipModules[i];
    }
  }
}

void Logger::clearFile() {
  LittleFS.remove(FILE_PATH);
  s_lastFileOffset = 0;
  s_lastModule[0]  = '\0';
  s_lastMsg[0]     = '\0';
}

void Logger::print(const char* module, const char* fmt, ...) {
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

  // 写串口
  Serial.printf("[%s] [%-*s] %s\r\n", ts, Logger::MODULE_PAD, module, msg);

  // 判断是否与上一条相同（module + msg 一致，时间戳不计）
  bool isDup = (s_count > 0)
            && (strcmp(module, s_lastModule) == 0)
            && (strcmp(msg,    s_lastMsg)    == 0);

  // 更新去重记录
  strncpy(s_lastModule, module, sizeof(s_lastModule) - 1);
  s_lastModule[sizeof(s_lastModule) - 1] = '\0';
  strncpy(s_lastMsg, msg, sizeof(s_lastMsg) - 1);
  s_lastMsg[sizeof(s_lastMsg) - 1] = '\0';

  // 写内存环形缓冲
  if (isDup) {
    // 原地替换上一槽（仅更新时间戳）
    int prevIdx = (s_head - 1 + BUF_LINES) % BUF_LINES;
    snprintf(s_lines[prevIdx], LINE_LEN, "[%s] [%-*s] %s", ts, Logger::MODULE_PAD, module, msg);
  } else {
    snprintf(s_lines[s_head], LINE_LEN, "[%s] [%-*s] %s", ts, Logger::MODULE_PAD, module, msg);
    s_head = (s_head + 1) % BUF_LINES;
    if (s_count < BUF_LINES) s_count++;
  }

  // 写 LittleFS 持久化文件
  if (s_fileEnabled) {
    // 模块过滤：在黑名单中的模块不写文件
    bool skip = false;
    for (int i = 0; i < s_fileSkipCount; i++) {
      if (strcmp(module, s_fileSkip[i]) == 0) { skip = true; break; }
    }

    if (!skip) {
      if (isDup && LittleFS.exists(FILE_PATH)) {
        // 原地替换文件最后一行（seek 到上一行起始位置，覆写后截断）
        File f = LittleFS.open(FILE_PATH, "r+");
        if (f) {
          f.seek(s_lastFileOffset);
          f.printf("[%s] [%-*s] %s\n", ts, Logger::MODULE_PAD, module, msg);
          f.truncate(f.position());
          f.close();
        }
      } else {
        rotateIfNeeded();
        File f = LittleFS.open(FILE_PATH, "a");
        if (f) {
          s_lastFileOffset = f.size();  // 追加前的文件末尾 = 新行起始位置
          f.printf("[%s] [%-*s] %s\n", ts, Logger::MODULE_PAD, module, msg);
          f.close();
        }
      }
    }
  }
}

String Logger::dump(int n) {
  // 优先从 LittleFS 文件读取
  if (s_fileEnabled) {
    File f = LittleFS.open(FILE_PATH, "r");
    if (f) {
      size_t sz = f.size();
      size_t readMax = (size_t)n * LINE_LEN;
      if (sz > readMax) f.seek(sz - readMax);
      String out;
      out.reserve(readMax);
      bool skipFirst = (sz > readMax);
      while (f.available()) {
        String line = f.readStringUntil('\n');
        if (skipFirst) { skipFirst = false; continue; }
        out += line;
        out += '\n';
      }
      f.close();
      return out;
    }
  }

  // 回退：内存缓冲
  if (n > s_count) n = s_count;
  String out;
  out.reserve(n * 80);
  int start = (s_head - s_count + BUF_LINES) % BUF_LINES;
  int skip  = s_count - n;
  for (int i = 0; i < s_count; i++) {
    if (i < skip) continue;
    out += s_lines[(start + i) % BUF_LINES];
    out += '\n';
  }
  return out;
}

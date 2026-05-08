#pragma once
#include <Arduino.h>

class Logger {
public:
  // 内存缓冲参数
  static constexpr int    BUF_LINES       = 200;
  static constexpr int    LINE_LEN        = 160;

  // LittleFS 持久化参数
  // LittleFS 分区 256KB，HTML 文件共 ~104KB，实际可用约 130KB。
  static constexpr size_t FILE_MAX_BYTES  = 96 * 1024;   // 文件最大 96KB（< LittleFS 可用量）
  static constexpr size_t FILE_KEEP_BYTES = 48 * 1024;   // 溢出时保留末尾 48KB（必须 < FILE_MAX_BYTES）
  static constexpr const char* FILE_PATH  = "/log.txt";

  // 在 LittleFS.begin() 成功后调用，启用持久化写入。
  // fileSkipModules: 不写入文件的模块名列表（仍写串口和内存缓冲），以 nullptr 结尾。
  // 例：Logger::enableFile({"SIM", "HTTP", nullptr})
  static void enableFile(const char* const* fileSkipModules = nullptr);

  // 清空 LittleFS 日志文件。
  static void clearFile();

  // 格式化并写入一条日志（串口 + 内存缓冲 + 可选 LittleFS）。
  static void print(const char* module, const char* fmt, ...);

  // 返回最近 n 条日志纯文本（优先读文件，回退到内存缓冲）。
  static String dump(int n = BUF_LINES);
};

// Usage: LOG("WiFi", "Connected to %s", ssid)
#define LOG(module, fmt, ...) Logger::print((module), (fmt), ##__VA_ARGS__)

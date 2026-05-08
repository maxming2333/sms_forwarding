#pragma once
#include <Arduino.h>

class Logger {
public:
  // 内存缓冲参数
  static constexpr int    BUF_LINES       = 200;
  static constexpr int    LINE_LEN        = 160;

  // 模块名对齐宽度（printf %-*s 的宽度值）
  static constexpr int    MODULE_PAD      = 8;

  // LittleFS 持久化参数
  // LittleFS 分区 256KB，HTML 文件共 ~104KB，实际可用约 130KB。
  static constexpr size_t FILE_MAX_BYTES  = 96 * 1024;   // 文件最大 96KB（< LittleFS 可用量）
  static constexpr size_t FILE_KEEP_BYTES = FILE_MAX_BYTES - (5 * 300);   // 溢出时保留末尾（必须 < FILE_MAX_BYTES），删除前 5 行
  static constexpr const char* FILE_PATH  = "/log.txt";
  static constexpr int    FILE_SKIP_MAX   = 16;          // 模块黑名单最大条目数

  // 在 LittleFS.begin() 成功后调用，初始化文件日志（注册跳过模块列表）。
  // 仅做初始化，不控制写入开关；写入开关由 setFileEnabled() 控制。
  // fileSkipModules: 不写入文件的模块名列表（仍写串口和内存缓冲），以 nullptr 结尾。
  // 例：Logger::init({"SIM", "HTTP", nullptr})
  static void init(const char* const* fileSkipModules = nullptr);

  // 运行时开关：控制是否实际写入文件（不影响串口和内存缓冲）。
  static void setFileEnabled(bool enabled);

  // 清空 LittleFS 日志文件。
  static void clearFile();

  // 格式化并写入一条日志（串口 + 内存缓冲 + 可选 LittleFS）。
  static void log(const char* module, const char* fmt, ...);

  // 返回最近 n 条日志纯文本（优先读文件，回退到内存缓冲）。
  static String dump(int n = BUF_LINES);
};

// Usage: LOG("WiFi", "Connected to %s", ssid)
#define LOG(module, fmt, ...) Logger::log((module), (fmt), ##__VA_ARGS__)

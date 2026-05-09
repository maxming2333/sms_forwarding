#pragma once
#include <Arduino.h>
#include <FS.h>
#include <memory>

class Logger {
public:
  // 内存缓冲参数
  static constexpr int    DUMP_DEFAULT_LINES = 200;  // dump() 默认返回行数
  static constexpr int    LINE_LEN           = 160;

  // 模块名对齐宽度（printf %-*s 的宽度值）
  static constexpr int    MODULE_PAD         = 8;

  // LittleFS 持久化参数
  // LittleFS 分区 256KB，HTML 文件共 ~104KB，实际可用约 130KB。
  static constexpr size_t FILE_MAX_BYTES     = 96 * 1024;   // 文件最大 96KB（< LittleFS 可用量，超出则裁顶等量后追加）
  static constexpr const char* FILE_PATH     = "/log.txt";
  static constexpr int    FILE_SKIP_MAX      = 16;   // 模块黑名单最大条目数
  static constexpr int    FLUSH_LINES        = 10;   // 积累多少条后批量刷写文件
  static constexpr int    FLUSH_INTERVAL_SEC = 10;   // 超时强制刷写（秒）

  // 在 LittleFS.begin() 成功后调用，初始化存储（注册跳过模块列表）。
  // skipModules: 不写入存储的模块名列表（仍走串口），以 nullptr 结尾。
  // 例：Logger::init({"SIM", "HTTP", nullptr})
  static void init(const char* const* skipModules = nullptr);

  // 运行时开关：控制是否输出到串口（默认 true）。
  static void setSerialEnabled(bool enabled);

  // 运行时开关：控制是否写入暂存缓冲和 LittleFS 文件（默认 false，不影响串口）。
  static void setStorageEnabled(bool enabled);

  // 将暂存缓冲中未刷写的行立即写入文件。
  static void flush();

  // 清空 LittleFS 日志文件和暂存缓冲。
  static void clearFile();

  // 格式化并写入一条日志（串口 + 可选文件）。
  static void log(const char* module, const char* fmt, ...);

  // flush 后返回已 seek 到位的文件句柄（shared_ptr 保证跨 async 生命周期安全）。
  // 文件不存在时返回 nullptr；调用方析构 shared_ptr 时 File 自动 close()。
  static std::shared_ptr<File> dumpStream(int n = DUMP_DEFAULT_LINES);
};

// Usage: LOG("WiFi", "Connected to %s", ssid)
#define LOG(module, fmt, ...) Logger::log((module), (fmt), ##__VA_ARGS__)

#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Arduino.h>

// RAII 封装：在堆上管理 HTTPClient + WiFiClientSecure 的完整生命周期。
// 用法：
//   auto session = HttpSession::request(url);
//   int code = session->http()->GET();
//   // 离开作用域时自动调用 end() 并释放 TLS 上下文
// TLS 策略：全局使用 setInsecure()（跳过证书验证）。
// 若需启用验证，修改 request() 内部的 _tls->setCACert(...) 调用。
class HttpSession {
public:
    // 工厂函数：创建并初始化会话（自动 begin），失败时返回 nullptr
    static HttpSession* request(const String& url);

    ~HttpSession();  // 调用 http.end()，释放 _http 和 _tls

    // 底层 HTTPClient 访问器：session->http()->GET() / addHeader(...) 等
    HTTPClient* http() { return _http; }

    // 禁止拷贝，防止双重释放
    HttpSession(const HttpSession&)            = delete;
    HttpSession& operator=(const HttpSession&) = delete;

private:
    HttpSession() = default;
    HTTPClient*       _http = nullptr;
    WiFiClientSecure* _tls  = nullptr;
};

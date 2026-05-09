#include "http.h"
#include <lwip/dns.h>  // dns_clear_cache()

// ── HttpSession ──────────────────────────────────────────────────────
HttpSession* HttpSession::request(const String& url) {
    // 清除 lwIP DNS 缓存，确保网络恢复后不使用过期解析结果
    dns_clear_cache();

    // OOM 防护：ESP32-C3 堆有限，Arduino 无异常模式下 new 失败返回 nullptr
    HttpSession* session = new HttpSession();
    if (!session) return nullptr;

    session->_http = new HTTPClient();
    if (!session->_http) {
        delete session;
        return nullptr;
    }

    if (url.startsWith("https://")) {
        session->_tls = new WiFiClientSecure();
        if (!session->_tls) {
            delete session;
            return nullptr;
        }
        // TLS 策略：跳过服务器证书验证（适用于内网/自签名证书场景）
        // 若需启用验证，改为：session->_tls->setCACert(root_ca_cert);
        session->_tls->setInsecure();
        if (!session->_http->begin(*session->_tls, url)) {
            delete session;
            return nullptr;
        }
    } else {
        if (!session->_http->begin(url)) {
            delete session;
            return nullptr;
        }
    }
    session->_http->setConnectTimeout(5000);
    session->_http->setTimeout(10000);
    return session;
}

HttpSession::~HttpSession() {
    if (_http) {
        _http->end();
        delete _http;
        _http = nullptr;
    }
    // _tls 必须在 _http 之后释放，确保 HTTPClient 不再持有引用时再析构 TLS 上下文
    if (_tls) {
        delete _tls;
        _tls = nullptr;
    }
}

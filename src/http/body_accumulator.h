#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <cstdlib>
#include <cstring>

static constexpr size_t HTTP_JSON_BODY_MAX_BYTES = 51200;

inline bool httpAccumulateBody(AsyncWebServerRequest* request,
                               uint8_t* data,
                               size_t len,
                               size_t index,
                               size_t total,
                               size_t maxBytes,
                               const char** outBody) {
  *outBody = nullptr;
  size_t expected = total > 0 ? total : index + len;
  if (expected > maxBytes || index + len > maxBytes) {
    if (index == 0 || request->_tempObject == nullptr) {
      request->send(413, "application/json", "{\"ok\":false,\"error\":\"请求体超过大小限制\"}");
    }
    return false;
  }

  if (index == 0) {
    if (request->_tempObject != nullptr) {
      free(request->_tempObject);
      request->_tempObject = nullptr;
    }
    request->_tempObject = calloc(expected + 1, 1);
    if (request->_tempObject == nullptr) {
      request->send(500, "application/json", "{\"ok\":false,\"error\":\"内存不足\"}");
      return false;
    }
  }

  if (request->_tempObject == nullptr) {
    request->send(500, "application/json", "{\"ok\":false,\"error\":\"请求体状态异常\"}");
    return false;
  }

  memcpy(static_cast<uint8_t*>(request->_tempObject) + index, data, len);
  if (index + len < expected) return true;

  static_cast<char*>(request->_tempObject)[index + len] = '\0';
  *outBody = static_cast<const char*>(request->_tempObject);
  return true;
}

inline void httpReleaseAccumulatedBody(AsyncWebServerRequest* request) {
  if (request->_tempObject != nullptr) {
    free(request->_tempObject);
    request->_tempObject = nullptr;
  }
}

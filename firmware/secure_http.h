#pragma once

#include <HTTPClient.h>
#include <StreamString.h>
#include <WiFiClientSecure.h>
#include <atomic>

// 根证书库由 Arduino-ESP32 的 ESP-IDF SDK 提供。
extern const uint8_t cloudCaStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t cloudCaEnd[] asm("_binary_x509_crt_bundle_end");

static void cloudUseCaBundle(WiFiClientSecure &client) {
  client.setCACertBundle(cloudCaStart, cloudCaEnd - cloudCaStart);
}

// HTTPClient 负责去除 chunked 帧。未知 Content-Length 也按实际写入字节限制。
class BoundedHttpBody : public StreamString {
 public:
  explicit BoundedHttpBody(size_t limit, const std::atomic<bool> *cancel = nullptr)
      : limit(limit), cancel(cancel) {}

  bool receive(HTTPClient &http) {
    const int length = http.getSize();
    if (length >= 0 && (size_t)length > limit) return false;
    if (length == 0) return true;
    return http.writeToStream(this) >= 0;
  }

  size_t write(const uint8_t *data, size_t count) override {
    if ((cancel && cancel->load()) || count > limit - length()) return 0;
    return StreamString::write(data, count);
  }

  size_t write(uint8_t byte) override { return write(&byte, 1); }

 private:
  size_t limit;
  const std::atomic<bool> *cancel;
};

#pragma once

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_hmac.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>

#include "secure_http.h"

enum class XzBinding : uint8_t { Idle, Fetching, Waiting, Bound, Offline, Failed, Expired };
static std::atomic<XzBinding> gXzState{XzBinding::Idle};
static std::atomic<bool> gXzRequested{false};
static std::atomic<int> gXzCode{-1};
static std::atomic<int> gXzStatus{0};
static uint32_t gXzNextPoll = 0;
static uint32_t gXzExpires = 0;
static char gXzMac[18];
static String gXzUuid;
static String gXzSerial;
static String gXzChallenge;
static constexpr int XZ_PROOF_FAILED = -1001;

static void xzRequestBinding() {
  if (gXzState.load() == XzBinding::Fetching) return;
  gXzState.store(XzBinding::Fetching);
  gXzRequested.store(true);
}

static void xzPrintBinding() {
  auto state = gXzState.load();
  const char *names[] = {"IDLE", "FETCHING", "WAITING", "BOUND", "OFFLINE", "FAILED", "EXPIRED"};
  Serial.printf("[XZ] %s status=%d\n", names[(int)state], gXzStatus.load());
  if (state == XzBinding::Waiting) Serial.printf("[XZ] activation code: %06d\n", gXzCode.load());
}

static bool xzLoadIdentity() {
  if (gXzUuid.length() == 36) return true;
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
  snprintf(gXzMac, sizeof(gXzMac), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // 复用官方固件的 NVS 键。换网、重启、保留 NVS 的刷写都沿用同一个 Client-Id。
  Preferences prefs;
  if (!prefs.begin("board", false)) return false;
  String uuid = prefs.getString("uuid");
  bool ok = true;
  if (uuid.length() != 36) {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    char formatted[37];
    snprintf(formatted, sizeof(formatted),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    ok = prefs.putString("uuid", formatted) > 0;
    uuid = formatted;
  }
  prefs.end();
  if (!ok) return false;
  gXzUuid = std::move(uuid);

  // 仅使用已经配置过 HMAC Key0 的板卡的 eFuse 序列号。
  char serial[33] = {};
  if (esp_efuse_get_key_purpose(EFUSE_BLK_KEY0) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP &&
      esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial, 32 * 8) == ESP_OK) {
    const size_t length = strlen(serial);
    bool printable = false, valid = true;
    for (size_t i = 0; i < sizeof(serial); ++i) {
      const uint8_t c = serial[i];
      if (i < length) {
        valid &= c >= 0x20 && c <= 0x7e;
        printable |= c != ' ';
      } else {
        valid &= c == 0;
      }
    }
    if (valid && printable) gXzSerial = serial;
  }
  return true;
}

static String xzDeviceInfo() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  const auto *app = esp_app_get_description();
  JsonDocument doc;
  doc["version"] = 2;
  doc["language"] = "zh-CN";
  doc["flash_size"] = ESP.getFlashChipSize();
  doc["psram_size"] = ESP.getPsramSize();
  doc["minimum_free_heap_size"] = String(ESP.getMinFreeHeap());
  doc["mac_address"] = gXzMac;
  doc["uuid"] = gXzUuid;
  doc["chip_model_name"] = "esp32s3";
  doc["chip_info"]["model"] = static_cast<int>(chip.model);
  doc["chip_info"]["cores"] = chip.cores;
  doc["chip_info"]["revision"] = chip.revision;
  doc["chip_info"]["features"] = chip.features;
  doc["application"]["name"] = "roundclock";
  doc["application"]["version"] = "2.6.2";
  doc["application"]["idf_version"] = app->idf_ver;
  doc["application"]["compile_time"] = String(app->date) + "T" + app->time + "Z";
  char elfHash[65];
  for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i)
    snprintf(elfHash + i * 2, 3, "%02x", app->app_elf_sha256[i]);
  doc["application"]["elf_sha256"] = elfHash;
  JsonArray partitions = doc["partition_table"].to<JsonArray>();
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it) {
    const esp_partition_t *part = esp_partition_get(it);
    JsonObject entry = partitions.add<JsonObject>();
    entry["label"] = part->label;
    entry["type"] = static_cast<int>(part->type);
    entry["subtype"] = static_cast<int>(part->subtype);
    entry["address"] = part->address;
    entry["size"] = part->size;
    it = esp_partition_next(it);
  }
  doc["display"]["width"] = 360;
  doc["display"]["height"] = 360;
  doc["display"]["monochrome"] = false;
  doc["board"]["type"] = "esp32-s3-touch-lcd-1.85b";
  doc["board"]["name"] = "esp32-s3-touch-lcd-1.85b";
  doc["board"]["manufacturer"] = "waveshare";
  doc["board"]["rssi"] = WiFi.RSSI();
  if (const auto *running = esp_ota_get_running_partition()) doc["ota"]["label"] = running->label;
  String request;
  serializeJson(doc, request);
  return request;
}

static String xzActivationBody() {
  if (!gXzSerial.length()) return "{}";
  uint8_t digest[32];
  esp_err_t err = esp_hmac_calculate(HMAC_KEY0, gXzChallenge.c_str(), gXzChallenge.length(), digest);
  if (err != ESP_OK) {
    Serial.printf("[XZ] HMAC error 0x%x\n", (unsigned)err);
    return "";
  }
  char hmac[65];
  for (size_t i = 0; i < sizeof(digest); ++i)
    snprintf(hmac + i * 2, 3, "%02x", digest[i]);
  JsonDocument doc;
  doc["algorithm"] = "hmac-sha256";
  doc["serial_number"] = gXzSerial;
  doc["challenge"] = gXzChallenge;
  doc["hmac"] = hmac;
  String body;
  serializeJson(doc, body);
  return body;
}

static int xzPost(bool activate, String &response) {
  String body = activate ? xzActivationBody() : xzDeviceInfo();
  if (!body.length()) return XZ_PROOF_FAILED;
  WiFiClientSecure client;
  cloudUseCaBundle(client);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6500);
  const char *url = activate ? "https://api.tenclass.net/xiaozhi/ota/activate"
                             : "https://api.tenclass.net/xiaozhi/ota/";
  if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.addHeader("Activation-Version", gXzSerial.length() ? "2" : "1");
  if (gXzSerial.length()) http.addHeader("Serial-Number", gXzSerial);
  http.addHeader("Device-Id", gXzMac);
  http.addHeader("Client-Id", gXzUuid);
  http.addHeader("Accept-Language", "zh-CN");
  http.addHeader("Content-Type", "application/json");
  http.setUserAgent("esp32-s3-touch-lcd-1.85b/2.6.2");
  int status = http.POST(std::move(body));
  if (!activate && status == HTTP_CODE_OK) {
    BoundedHttpBody reply(8192);
    if (reply.receive(http)) response = std::move(static_cast<String &>(reply));
    else { Serial.println("[XZ] response incomplete or too large"); status = HTTPC_ERROR_STREAM_WRITE; }
  }
  if (status < 0) {
    char detail[96] = {};
    client.lastError(detail, sizeof(detail));
    Serial.printf("[XZ] %s transport=%d %s\n", activate ? "activate" : "ota", status, detail);
  } else {
    Serial.printf("[XZ] %s HTTP=%d\n", activate ? "activate" : "ota", status);
  }
  http.end();
  return status;
}

static void xzQueryBinding() {
  gXzCode.store(-1);
  gXzStatus.store(0);
  if (WiFi.status() != WL_CONNECTED) { gXzState.store(XzBinding::Offline); return; }
  if (!xzLoadIdentity()) {
    Serial.println("[XZ] could not load device identity");
    gXzState.store(XzBinding::Failed);
    return;
  }
  String response;
  int status = xzPost(false, response);
  gXzStatus.store(status);
  JsonDocument doc;
  if (status != HTTP_CODE_OK || deserializeJson(doc, response) || !doc.is<JsonObject>()) {
    gXzState.store(XzBinding::Failed);
    return;
  }
  if (!doc["activation"].is<JsonObject>()) {
    bool hasTransport = doc["mqtt"].is<JsonObject>() || doc["websocket"].is<JsonObject>();
    // v2.7：把 websocket 配置落 NVS —— 真正的小智协议要用它连 wss://.../xiaozhi/v1/
    if (doc["websocket"].is<JsonObject>()) {
      String url   = String(doc["websocket"]["url"]   | "");
      String token = String(doc["websocket"]["token"] | "");
      if (url.length()) {
        if (cfgGetStr("xz.url") != url) cfgSet("xz.url", url);
        if (cfgGetStr("xz.token") != token) cfgSet("xz.token", token);
        Serial.printf("[XZ] ws=%s token=%d chars\n", url.c_str(), token.length());
      }
    }
    gXzState.store(hasTransport ? XzBinding::Bound : XzBinding::Failed);
    xzPrintBinding();
    return;
  }
  const char *code = doc["activation"]["code"] | "";
  const char *challenge = doc["activation"]["challenge"] | "";
  if (strlen(code) != 6 || strspn(code, "0123456789") != 6 || !*challenge) {
    Serial.println("[XZ] missing code or challenge");
    gXzState.store(XzBinding::Failed);
    return;
  }
  gXzChallenge = challenge;
  gXzCode.store(atoi(code));
  const uint32_t now = millis();
  gXzNextPoll = now + 3000;
  const uint32_t timeout = doc["activation"]["timeout_ms"] | 120000UL;
  gXzExpires = now + constrain(timeout, 5000UL, 300000UL);
  gXzState.store(XzBinding::Waiting);
  xzPrintBinding();
}

static void xzPollBinding() {
  if (gXzRequested.exchange(false)) { xzQueryBinding(); return; }
  if (gXzState.load() != XzBinding::Waiting) return;
  uint32_t now = millis();
  if ((int32_t)(now - gXzExpires) >= 0) {
    gXzState.store(XzBinding::Expired);
    return;
  }
  if ((int32_t)(now - gXzNextPoll) < 0 || WiFi.status() != WL_CONNECTED) return;

  String ignored;
  int status = xzPost(true, ignored);
  gXzStatus.store(status);
  gXzNextPoll = millis() + 5000;
  // 主循环可以在上一条确认请求还没结束时要求重新获取。
  if (gXzRequested.load()) return;
  if (status == HTTP_CODE_OK) {
    gXzState.store(XzBinding::Bound);
    xzPrintBinding();
    // 绑定完成后必须再查一次配置：websocket url/token 只在配置响应里
    gXzRequested.store(true);
  } else if (status == XZ_PROOF_FAILED || (status > 0 && status != HTTP_CODE_ACCEPTED)) {
    gXzState.store(XzBinding::Failed);
  }
}

// ---------------- v2.7 语音通道要用的连接参数 ----------------
static String xzWsUrl()   { return cfgGetStr("xz.url", "wss://api.tenclass.net/xiaozhi/v1/"); }
static String xzWsToken() { return cfgGetStr("xz.token", ""); }
static const char *xzDeviceMac() { xzLoadIdentity(); return gXzMac; }
static const char *xzClientId()  { xzLoadIdentity(); return gXzUuid.c_str(); }

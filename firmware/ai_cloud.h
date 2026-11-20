#pragma once
// ============================================================
//  云端 AI v1.0
//  ① 阿里云百炼 Paraformer-realtime-v2：PCM 流式语音转文字
//  ② Qwen：把转写结果整理成待办 / 会议纪要
//  ③ 小智页面的语音问答由百炼转写和 Qwen 提供。
//  凭据存 NVS，串口 BAILIAN 配置。
// ============================================================
#include <WiFiClientSecure.h>
#include <base64.h>
#include <ArduinoJson.h>
#include <atomic>
#include <new>
#include "xiaozhi_binding.h"
#include <esp_random.h>
#include <mbedtls/sha1.h>
static void render();
static void showToast(const char *s);

// ⚠ 凭据必须在 setup() 里加载（cloudLoadCreds），不能写成全局静态初始化：
//   全局构造发生在 nvs_flash_init 之前，cfgGetStr 必然返回空串，而且之后再也不会重读 ——
//   现象就是"串口写 key 提示 saved、重启后读回 empty"。
static String gBailianKey;
// 模型名：qwen-turbo 在新账号上经常是 Throttling.RateQuota（免费额度落在 qwen-plus 那档），
// 所以默认用 qwen-plus，串口 MODEL <name> 可改（NVS rcfg/qwen.model）。
static String gQwenModel = "qwen-plus";
static void cloudLoadCreds() {
  gBailianKey    = cfgGetStr("bailian.key");
  gQwenModel     = cfgGetStr("qwen.model", "qwen-plus");
  Serial.printf("[AI] creds: bailian=%s (%d chars) model=%s\n",
                gBailianKey.length() ? "SET" : "(empty)", gBailianKey.length(),
                gQwenModel.c_str());
}

// ---------------- WebSocket 客户端（百炼控制消息和二进制 PCM） ----------------
class CloudWS {
 public:
  WiFiClientSecure client;
  String error;

  bool begin(const char *host, const char *uri, const String &key,
             const String &extraHeaders = String()) {
    cloudUseCaBundle(client);
    client.setHandshakeTimeout(12);
    if (!client.connect(host, 443, 6000)) {
      char detail[96] = {};
      client.lastError(detail, sizeof(detail));
      Serial.printf("[WS] connect failed: %s\n", detail);
      return fail("WS_CONNECT");
    }

    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    String wsKey = base64::encode(nonce, sizeof(nonce));
    String req = String("GET ") + uri + " HTTP/1.1\r\nHost: " + host +
                 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                 "Sec-WebSocket-Key: " + wsKey + "\r\nAuthorization: Bearer " + key + "\r\n" +
                 extraHeaders + "\r\n";
    if (client.print(req) != req.length()) return fail("WS_WRITE");

    const uint32_t deadline = millis() + 8000;
    String line;
    if (!readHeader(line, deadline)) return fail("WS_HANDSHAKE");
    int status = line.substring(line.indexOf(' ') + 1).toInt();
    if (status != 101) return fail(String("WS_HTTP_") + status);

    String acceptKey = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    if (mbedtls_sha1((const uint8_t *)acceptKey.c_str(), acceptKey.length(), digest) != 0)
      return fail("WS_HANDSHAKE");
    const String expected = base64::encode(digest, sizeof(digest));
    bool accepted = false;
    for (int i = 0; i < 40; ++i) {
      if (!readHeader(line, deadline)) break;
      if (!line.length()) return accepted || fail("WS_HANDSHAKE");
      int colon = line.indexOf(':');
      if (colon < 0 || !line.substring(0, colon).equalsIgnoreCase("Sec-WebSocket-Accept")) continue;
      String value = line.substring(colon + 1);
      value.trim();
      accepted = value == expected;
    }
    return fail("WS_HANDSHAKE");
  }

  bool sendText(const String &text) {
    return sendFrame(0x1, (const uint8_t *)text.c_str(), text.length());
  }

  bool sendPcm(const int16_t *data, size_t frames) {
    return sendFrame(0x2, (const uint8_t *)data, frames * sizeof(*data));
  }

  // 保留跨轮询的分片消息。半个帧超时后关闭连接，避免把后续字节当作新帧头。
  bool recvText(String &out, uint32_t waitMs) {
    out = "";
    const uint32_t waitUntil = millis() + waitMs;
    do {
      if (client.available() <= 0) {
        if (!client.connected()) return fail("WS_CLOSED");
        if ((int32_t)(waitUntil - millis()) <= 0) return false;
        delay(2);
        continue;
      }
      const uint32_t deadline = millis() + 4000;
      uint8_t header[2];
      if (!readExact(header, sizeof(header), deadline)) return fail("WS_READ");
      const bool fin = header[0] & 0x80;
      const uint8_t opcode = header[0] & 0x0f;
      size_t len = header[1] & 0x7f;
      if ((header[0] & 0x70) || (header[1] & 0x80) || len == 127)
        return fail("WS_FRAME");
      if (len == 126) {
        uint8_t extended[2];
        if (!readExact(extended, sizeof(extended), deadline)) return fail("WS_READ");
        len = (extended[0] << 8) | extended[1];
      }
      if (opcode >= 0x8) {
        uint8_t control[125];
        if (!fin || len > sizeof(control)) return fail("WS_FRAME");
        if (!readExact(control, len, deadline)) return fail("WS_READ");
        if (opcode == 0x8) return fail("WS_CLOSED");
        if (opcode == 0x9 && !sendFrame(0xA, control, len)) return false;
        continue;
      }
      if ((fragmented ? opcode != 0 : opcode != 1) || message.length() + len > 16384)
        return fail("WS_FRAME");
      uint8_t block[512];
      while (len) {
        size_t count = min(len, sizeof(block));
        if (!readExact(block, count, deadline)) return fail("WS_READ");
        if (!message.concat((const char *)block, count)) return fail("NO_MEMORY");
        len -= count;
      }
      fragmented = !fin;
      if (fin) {
        out = std::move(message);
        message = "";
        return true;
      }
    } while ((int32_t)(waitUntil - millis()) > 0);
    return false;
  }

  void close() { client.stop(); }

  // v2.7 小智：一帧通吃。返回 1=文本 2=二进制 0=超时 -1=错误。
  // 小智的消息都是单帧（不做分片重组），音频帧最大几百字节，超长的直接丢弃。
  int recvMessage(String &out, uint8_t *bin, size_t binCap, size_t &binLen, uint32_t waitMs) {
    out = "";
    binLen = 0;
    const uint32_t waitUntil = millis() + waitMs;
    do {
      if (client.available() <= 0) {
        if (!client.connected()) { fail("WS_CLOSED"); return -1; }
        if ((int32_t)(waitUntil - millis()) <= 0) return 0;
        delay(2);
        continue;
      }
      const uint32_t deadline = millis() + 4000;
      uint8_t header[2];
      if (!readExact(header, sizeof(header), deadline)) { fail("WS_READ"); return -1; }
      const bool fin = header[0] & 0x80;
      const uint8_t opcode = header[0] & 0x0f;
      size_t len = header[1] & 0x7f;
      if ((header[0] & 0x70) || (header[1] & 0x80) || len == 127) { fail("WS_FRAME"); return -1; }
      if (len == 126) {
        uint8_t extended[2];
        if (!readExact(extended, sizeof(extended), deadline)) { fail("WS_READ"); return -1; }
        len = (extended[0] << 8) | extended[1];
      }
      if (opcode >= 0x8) {
        uint8_t control[125];
        if (!fin || len > sizeof(control)) { fail("WS_FRAME"); return -1; }
        if (!readExact(control, len, deadline)) { fail("WS_READ"); return -1; }
        if (opcode == 0x8) { fail("WS_CLOSED"); return -1; }
        if (opcode == 0x9 && !sendFrame(0xA, control, len)) return -1;
        continue;
      }
      if (!fin) { fail("WS_FRAGMENT"); return -1; }
      if (opcode == 0x1) {
        if (len > 8192) { fail("WS_FRAME"); return -1; }
        uint8_t block[256];
        while (len) {
          size_t count = min(len, sizeof(block));
          if (!readExact(block, count, deadline)) { fail("WS_READ"); return -1; }
          if (!out.concat((const char *)block, count)) { fail("NO_MEMORY"); return -1; }
          len -= count;
        }
        return 1;
      }
      if (opcode == 0x2 && len <= binCap) {
        if (!readExact(bin, len, deadline)) { fail("WS_READ"); return -1; }
        binLen = len;
        return 2;
      }
      uint8_t block[256];                       // 其它情况：读掉丢弃
      while (len) {
        size_t count = min(len, sizeof(block));
        if (!readExact(block, count, deadline)) { fail("WS_READ"); return -1; }
        len -= count;
      }
    } while ((int32_t)(waitUntil - millis()) > 0);
    return 0;
  }

  bool sendBinary(const uint8_t *data, size_t len) { return sendFrame(0x2, data, len); }

 private:
  String message;
  bool fragmented = false;

  bool fail(const String &reason) {
    error = reason;
    close();
    return false;
  }

  bool readExact(uint8_t *data, size_t len, uint32_t deadline) {
    while (len) {
      int available = client.available();
      if (available > 0) {
        int count = client.read(data, min(len, (size_t)available));
        if (count <= 0) return false;
        data += count;
        len -= count;
      } else {
        if (!client.connected() || (int32_t)(deadline - millis()) <= 0) return false;
        delay(2);
      }
    }
    return true;
  }

  bool readHeader(String &line, uint32_t deadline) {
    line = "";
    uint8_t byte;
    while (line.length() < 768 && (int32_t)(deadline - millis()) > 0 &&
           readExact(&byte, 1, deadline)) {
      if (byte == '\n') { line.trim(); return true; }
      if (!line.concat((char)byte)) return false;
    }
    return false;
  }

  bool sendFrame(uint8_t opcode, const uint8_t *data, size_t len) {
    if (len > 65535) return fail("WS_FRAME");
    uint8_t header[8] = {uint8_t(0x80 | opcode)};
    size_t size = 2;
    if (len < 126) {
      header[1] = 0x80 | len;
    } else {
      header[1] = 0x80 | 126;
      header[size++] = len >> 8;
      header[size++] = len;
    }
    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    memcpy(header + size, mask, sizeof(mask));
    size += sizeof(mask);
    if (client.write(header, size) != size) return fail("WS_WRITE");
    uint8_t block[1024];
    for (size_t off = 0; off < len;) {
      size_t count = min(sizeof(block), len - off);
      for (size_t i = 0; i < count; ++i) block[i] = data[off + i] ^ mask[(off + i) % 4];
      if (client.write(block, count) != count) return fail("WS_WRITE");
      off += count;
    }
    return true;
  }
};

// ---------------- Todo 清单（NVS，最多 30 条） ----------------
#define TODO_MAX 30
struct TodoItem {
  char     text[96];
  uint8_t  done;
  uint8_t  _pad[3];
  uint32_t ts;        // 创建时 millis()/1000，仅调试用
  uint32_t deadline;  // Unix epoch 秒（本地时区解释），0 = 无期限
};
static TodoItem gTodos[TODO_MAX];
static int gTodoN = 0;

static uint32_t todoNow() { return (uint32_t)time(nullptr); }

// 本地时间 → epoch。拒绝会被 mktime 自动进位的非法日期。
static uint32_t todoMakeEpoch(int y, int mo, int d, int h, int mi) {
  if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  time_t result = mktime(&t);
  if (result < 0 || result > UINT32_MAX || t.tm_year != y - 1900 || t.tm_mon != mo - 1 ||
      t.tm_mday != d || t.tm_hour != h || t.tm_min != mi) return 0;
  return (uint32_t)result;
}

static uint32_t todoParseDeadline(const String &s) {
  String v = s; v.trim();
  const char *text = v.c_str();
  if (*text == '+') {
    const char *digits = text + 1;
    char *end;
    if (*digits < '0' || *digits > '9') return 0;
    long days = strtol(digits, &end, 10);
    if (*end || days > 3650) return 0;
    uint64_t at = todoNow() + (uint64_t)days * 86400;
    return at <= UINT32_MAX ? (uint32_t)at : 0;
  }
  int y, mo, d, h, mi, len = 0;
  if (sscanf(text, "%4d-%2d-%2d %2d:%2d%n", &y, &mo, &d, &h, &mi, &len) == 5 &&
      text[len] == 0) return todoMakeEpoch(y, mo, d, h, mi);
  if (sscanf(text, "%4d-%2d-%2d%n", &y, &mo, &d, &len) == 3 && text[len] == 0)
    return todoMakeEpoch(y, mo, d, 18, 0);
  return 0;
}

static int todoDayNumber(const struct tm &local) {
  int y = local.tm_year + 1899;
  return 365 * y + y / 4 - y / 100 + y / 400 + local.tm_yday;
}

static void todoDeadlineLabel(uint32_t dl, char *buf, int n, uint16_t &col) {
  col = COL(110, 128, 146);
  if (!dl) { snprintf(buf, n, "-"); return; }
  const time_t now = todoNow(), due = dl;
  struct tm today, deadline;
  if (!localtime_r(&now, &today) || !localtime_r(&due, &deadline)) {
    snprintf(buf, n, "-");
    return;
  }
  const int days = todoDayNumber(deadline) - todoDayNumber(today);
  if (due < now) {
    if (days < 0) snprintf(buf, n, "逾期%d天", -days);
    else snprintf(buf, n, "已逾期");
    col = COL(240, 110, 100);
  } else if (days == 0) {
    snprintf(buf, n, "今天"); col = C_ORANGE;
  } else if (days == 1) {
    snprintf(buf, n, "明天"); col = C_YELLOW;
  } else if (days <= 30) {
    snprintf(buf, n, "%d天后", days); col = COL(120, 190, 150);
  } else {
    snprintf(buf, n, "%d/%d", deadline.tm_mon + 1, deadline.tm_mday);
  }
}

struct TodoSnapshot {
  uint8_t version;
  uint8_t count;
  uint8_t reserved[2];
  TodoItem items[TODO_MAX];
};

static bool todoSave() {
  TodoSnapshot snapshot = {};
  snapshot.version = 1;
  snapshot.count = gTodoN;
  const size_t len = offsetof(TodoSnapshot, items) + gTodoN * sizeof(TodoItem);
  memcpy(snapshot.items, gTodos, gTodoN * sizeof(TodoItem));
  Preferences prefs;
  if (prefs.begin("rcfg", false) && prefs.putBytes("todo.v1", &snapshot, len) == len) return true;
  Serial.println("[TODO] snapshot save failed");
  return false;
}

static void todoLoad() {
  {
    Preferences prefs;
    if (prefs.begin("rcfg", true) && prefs.isKey("todo.v1")) {
      TodoSnapshot snapshot = {};
      const size_t len = prefs.getBytesLength("todo.v1");
      if (len >= offsetof(TodoSnapshot, items) && len <= sizeof(snapshot) &&
          prefs.getBytes("todo.v1", &snapshot, len) == len &&
          snapshot.version == 1 && snapshot.count <= TODO_MAX &&
          len == offsetof(TodoSnapshot, items) + snapshot.count * sizeof(TodoItem)) {
        gTodoN = snapshot.count;
        memcpy(gTodos, snapshot.items, gTodoN * sizeof(TodoItem));
        for (int i = 0; i < gTodoN; ++i) gTodos[i].text[sizeof(gTodos[i].text) - 1] = 0;
        return;
      }
    }
  }
  // 兼容旧索引和旧条目键。下一次修改时用一个快照提交。
  gTodoN = constrain(cfgGetInt("todo.n", 0), 0, TODO_MAX);
  for (int i = 0; i < gTodoN; i++) {
    char key[16];
    snprintf(key, sizeof(key), "todo.%d", i);
    String v = cfgGetStr(key);
    // 新格式：done|deadline|文字；旧格式 done|文字 仍可读（deadline=0）
    int p = v.indexOf('|');
    if (p > 0) {
      gTodos[i].done = (uint8_t)v.substring(0, p).toInt();
      int q = v.indexOf('|', p + 1);
      if (q > 0) {
        gTodos[i].deadline = (uint32_t)v.substring(p + 1, q).toInt();
        strlcpy(gTodos[i].text, v.substring(q + 1).c_str(), sizeof(gTodos[i].text));
      } else {
        gTodos[i].deadline = 0;
        strlcpy(gTodos[i].text, v.substring(p + 1).c_str(), sizeof(gTodos[i].text));
      }
    } else {
      gTodos[i].done = 0; gTodos[i].text[0] = 0; gTodos[i].deadline = 0;
    }
    gTodos[i].ts = 0;
  }
}
static bool todoCommit() {
  if (todoSave()) return true;
  todoLoad();
  showToast("待办保存失败");
  return false;
}

static void todoAppend(const char *text, uint32_t deadline) {
  if (!text || !*text) return;
  if (gTodoN >= TODO_MAX) {
    // 满了就挤掉最早一条已完成的，否则挤掉最早一条
    int victim = 0;
    for (int i = 0; i < gTodoN; i++) if (gTodos[i].done) { victim = i; break; }
    for (int i = victim; i < gTodoN - 1; i++) gTodos[i] = gTodos[i + 1];
    gTodoN--;
  }
  memset(&gTodos[gTodoN], 0, sizeof(TodoItem));
  size_t len = min(strlen(text), sizeof(gTodos[gTodoN].text) - 1);
  while (len && ((uint8_t)text[len] & 0xc0) == 0x80) --len;
  memcpy(gTodos[gTodoN].text, text, len);
  gTodos[gTodoN].done = 0;
  gTodos[gTodoN].ts = (uint32_t)(millis() / 1000);
  gTodos[gTodoN].deadline = deadline;
  gTodoN++;
}
static void todoToggle(int i) {
  if (i < 0 || i >= gTodoN) return;
  gTodos[i].done = !gTodos[i].done;
  todoCommit();
}
static void todoDel(int i) {
  if (i < 0 || i >= gTodoN) return;
  for (int k = i; k < gTodoN - 1; k++) gTodos[k] = gTodos[k + 1];
  gTodoN--;
  todoCommit();
}
// 去重：同一条待办已存在（未勾选）就不再重复加；
// 若已存在但此前无期限、这次带上了期限，则补写期限并返回 true（算一次更新）。
static bool todoAddUnique(const char *text, uint32_t deadline = 0) {
  if (!text || !*text) return false;
  String t(text); t.trim();
  if (!t.length()) return false;
  for (int i = 0; i < gTodoN; i++) {
    if (!gTodos[i].done && strcmp(gTodos[i].text, t.c_str()) == 0) {
      if (deadline && !gTodos[i].deadline) {
        gTodos[i].deadline = deadline; return true;
      }
      return false;
    }
  }
  todoAppend(t.c_str(), deadline);
  return true;
}
// 从行尾剥出 "@2026-11-18 15:00" / "@+3" 这种期限标记，返回 epoch（0=没有）
static uint32_t todoStripInlineDeadline(String &line) {
  int at = line.lastIndexOf('@');
  if (at < 0) return 0;
  String tail = line.substring(at + 1); tail.trim();
  uint32_t dl = todoParseDeadline(tail);
  if (dl) line = line.substring(0, at);
  line.trim();
  return dl;
}
// 把多行文本（每行一条，可带 " @日期"）批量写入待办，返回实际新增条数。
// 备忘 ASR 提待办、串口 ASRTODO 都走这里，逻辑只有一份。
static int todoAddLines(const String &lines, int maxItems) {
  int added = 0, p = 0;
  while (p < (int)lines.length() && added < maxItems) {
    int nl = lines.indexOf('\n', p); if (nl < 0) nl = lines.length();
    String line = lines.substring(p, nl); line.trim(); p = nl + 1;
    uint32_t dl = todoStripInlineDeadline(line);
    // Qwen 偶尔带 "- " / 序号 / 空行，统一剥掉
    if (line.startsWith("- ")) line = line.substring(2);
    if (line.startsWith("· ")) line = line.substring(2);
    unsigned int digits = 0;
    while (digits < line.length() && line[digits] >= '0' && line[digits] <= '9') ++digits;
    if (digits > 0 && digits < line.length()) {
      String suffix = line.substring(digits);
      if (suffix.startsWith("、")) line = suffix.substring(strlen("、"));
      else if (suffix.startsWith(". ") || suffix.startsWith(") ")) line = suffix.substring(2);
    }
    line.trim();
    if (!line.length()) continue;
    if (line == "无" || line == "没有待办" || line == "无待办" || line == "暂无") continue;
    if (todoAddUnique(line.c_str(), dl)) added++;
  }
  if (added && !todoCommit()) return -1;
  return added;
}

// ---------------- 百炼 ASR：Paraformer-realtime-v2 ----------------
static String gAsrErr;
static std::atomic<bool> gAiCancel{false};

class AsrSession {
 public:
  explicit AsrSession(const char *taskId) : id(taskId) {}

  bool read(CloudWS &ws, uint32_t waitMs) {
    String packet;
    if (!ws.recvText(packet, waitMs)) {
      if (ws.error.length()) gAsrErr = ws.error;
      return !gAsrErr.length();
    }

    JsonDocument event;
    if (deserializeJson(event, packet)) {
      gAsrErr = "ASR_JSON";
      return false;
    }
    JsonObjectConst header = event["header"];
    const char *name = header["event"] | "";
    const char *eventId = header["task_id"] | "";
    if (*eventId ? strcmp(eventId, id) != 0 : strcmp(name, "task-failed") != 0) return true;
    if (!strcmp(name, "task-started")) {
      started = true;
      Serial.println("[ASR] task-started");
    } else if (!strcmp(name, "result-generated")) {
      JsonObjectConst sentence = event["payload"]["output"]["sentence"];
      if ((sentence["sentence_end"] | false) && !(sentence["heartbeat"] | false)) {
        if (!text.concat(sentence["text"] | "")) {
          gAsrErr = "NO_MEMORY";
          return false;
        }
      }
    } else if (!strcmp(name, "task-failed")) {
      gAsrErr = header["error_code"] | "TASK_FAILED";
      Serial.printf("[ASR] task-failed %s: %.160s\n",
                    gAsrErr.c_str(), header["error_message"] | "");
      return false;
    } else if (!strcmp(name, "task-finished")) {
      finished = true;
    }
    return true;
  }

  bool started = false;
  bool finished = false;
  String text;

 private:
  const char *id;
};

static String asrTranscribe(const int16_t *pcm, int frames, const String &key, File *wav) {
  gAsrErr = "";
  if (gAiCancel.load()) { gAsrErr = "CANCELLED"; return ""; }
  if (!key.length()) { gAsrErr = "NO_KEY"; return ""; }
  if (WiFi.status() != WL_CONNECTED) { gAsrErr = "NO_WIFI"; return ""; }

  CloudWS ws;
  if (!ws.begin("dashscope.aliyuncs.com", "/api-ws/v1/inference", key)) {
    gAsrErr = ws.error;
    return "";
  }
  char taskId[33];
  snprintf(taskId, sizeof(taskId), "%08lx%08lx%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
  AsrSession session(taskId);
  {
    JsonDocument doc;
    doc["header"]["action"] = "run-task";
    doc["header"]["task_id"] = taskId;
    doc["header"]["streaming"] = "duplex";
    auto payload = doc["payload"].to<JsonObject>();
    payload["task_group"] = "audio";
    payload["task"] = "asr";
    payload["function"] = "recognition";
    payload["model"] = "paraformer-realtime-v2";
    payload["parameters"]["format"] = "pcm";
    payload["parameters"]["sample_rate"] = 16000;
    payload["input"].to<JsonObject>();
    String request;
    serializeJson(doc, request);
    if (!ws.sendText(request)) { gAsrErr = ws.error; return ""; }
  }

  uint32_t until = millis() + 8000;
  while (!session.started && (int32_t)(until - millis()) > 0) {
    if (gAiCancel.load()) { gAsrErr = "CANCELLED"; return ""; }
    if (!session.read(ws, 200)) return "";
  }
  if (!session.started) { gAsrErr = "START_TIMEOUT"; return ""; }

  // 每帧发送 100 ms 的 PCM16 单声道音频，分片间接收服务端事件。
  constexpr int chunkFrames = 1600;
  int16_t chunk[chunkFrames];
  for (int offset = 0; offset < frames; offset += chunkFrames) {
    int count = min(chunkFrames, frames - offset);
    if (gAiCancel.load()) { gAsrErr = "CANCELLED"; return ""; }
    const int16_t *samples = chunk;
    if (wav) {
      size_t bytes = count * sizeof(*samples);
      if (wav->read((uint8_t *)chunk, bytes) != bytes) { gAsrErr = "WAV_READ"; return ""; }
    } else {
      samples = pcm + offset;
    }
    if (!ws.sendPcm(samples, count)) { gAsrErr = ws.error; return ""; }
    until = millis() + count / 16;
    while ((int32_t)(until - millis()) > 0) {
      if (!session.read(ws, 10)) return "";
      if (session.finished) { gAsrErr = "EARLY_FINISH"; return ""; }
    }
  }

  JsonDocument finish;
  finish["header"]["action"] = "finish-task";
  finish["header"]["task_id"] = taskId;
  finish["header"]["streaming"] = "duplex";
  finish["payload"]["input"].to<JsonObject>();
  String request;
  serializeJson(finish, request);
  if (!ws.sendText(request)) { gAsrErr = ws.error; return ""; }

  until = millis() + 15000;
  while (!session.finished && (int32_t)(until - millis()) > 0) {
    if (gAiCancel.load()) { gAsrErr = "CANCELLED"; return ""; }
    if (!session.read(ws, 250)) return "";
  }
  ws.close();
  if (!session.finished) { gAsrErr = "FINISH_TIMEOUT"; return ""; }
  session.text.trim();
  if (!session.text.length()) gAsrErr = "NO_VALID_AUDIO";
  Serial.printf("[ASR] task-finished, %d frames, %u bytes\n", frames,
                (unsigned)session.text.length());
  return session.text;
}

// ---------------- Qwen 通用 HTTP 调用 ----------------
static String qwenCall(const char *systemPrompt, const char *userPrompt, const String &key,
                       const String &model, String &error, int maxTok = 512) {
  String out;
  if (gAiCancel.load()) { error = "CANCELLED"; return out; }
  if (!key.length()) { error = "NO_KEY"; return out; }
  if (WiFi.status() != WL_CONNECTED) { error = "NO_WIFI"; return out; }
  WiFiClientSecure c;
  cloudUseCaBundle(c);
  c.setHandshakeTimeout(12);
  StaticJsonDocument<2048> doc;
  doc["model"] = model.c_str();
  doc["temperature"] = 0.2;
  doc["max_tokens"] = maxTok;
  doc["messages"][0]["role"] = "system";
  doc["messages"][0]["content"] = systemPrompt;
  doc["messages"][1]["role"] = "user";
  doc["messages"][1]["content"] = userPrompt;
  String body; serializeJson(doc, body);
  HTTPClient http;
  if (!http.begin(c, "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions")) {
    error = "CONNECT";
    return out;
  }
  http.setConnectTimeout(6000);
  http.setTimeout(15000);
  http.addHeader("Authorization", "Bearer " + key);
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(body);
  BoundedHttpBody reply(12288, &gAiCancel);
  const bool received = status > 0 && reply.receive(http);
  http.end();
  if (gAiCancel.load()) { error = "CANCELLED"; return out; }
  if (status <= 0) {
    char detail[96] = {};
    c.lastError(detail, sizeof(detail));
    Serial.printf("[Qwen] HTTP transport=%d %s\n", status, detail);
    error = "RESPONSE";
    return out;
  }
  if (!received) { error = "RESPONSE_SIZE_OR_READ"; return out; }
  JsonDocument jd;
  if (deserializeJson(jd, static_cast<const String &>(reply))) {
    error = status == 200 ? String("JSON") : String("HTTP_") + status;
    return out;
  }
  const char *apiErr = jd["error"]["message"] | (const char *)nullptr;
  if (apiErr) {
    error = jd["error"]["code"] | "API_ERROR";
    Serial.printf("[Qwen] api error %s: %.160s\n", error.c_str(), apiErr);
    return out;
  }
  if (status != 200) { error = String("HTTP_") + status; return out; }
  out = jd["choices"][0]["message"]["content"].as<String>();
  out.trim();
  out.replace("\r", "");
  if (!out.length()) error = "EMPTY_RESPONSE";
  else Serial.printf("[Qwen] response %u bytes\n", (unsigned)out.length());
  return out;
}

// 把转写整理成待办（每行一条，带 @期限）/ 会议纪要
static String qwenExtract(const char *text, bool meetingMode, const String &key,
                          const String &model, String &error) {
  String prompt;
  if (meetingMode) {
    prompt = "把下面的会议录音转写整理成简洁中文纪要，每行一条，最多8行，第一行写「纪要：」。只输出正文：\n";
  } else {
    time_t now = time(nullptr);
    struct tm ti; localtime_r(&now, &ti);
    char nowBuf[32];
    snprintf(nowBuf, sizeof(nowBuf), "%04d-%02d-%02d %02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
    // 让云端把"明天/周五"换算成绝对日期，设备端只做一次 mktime，避免本地解析中文相对时间
    prompt = String("从下面的语音转写里提取要办的事项，每行一条，最多6条，每行不超过20个汉字，不要编号不要解释。")
           + "如果提到时间，就在该行末尾加一个空格和@，再写换算后的具体日期时间，格式 @2026-11-18 15:00；"
           + "没有具体钟点就写 @2026-11-18 18:00。当前时间是 " + nowBuf
           + "，请把明天/后天/周几等相对说法换算成具体日期。没有时间就不要加@。"
           + "没有待办就输出「无」。只输出正文：\n";
  }
  String full = prompt + text;
  return qwenCall("你是简洁的中文秘书，只输出要求的行格式。", full.c_str(), key, model, error);
}

// 通用问答（文字版，走 Qwen；串口 QWEN <问题> 或小智页文字提问时也可用）
static String qwenAsk(const char *question, const String &key, const String &model, String &error) {
  // 设备字库只有 GB2312 点阵：emoji / 波浪号会被画成空白，回答里别出现
  return qwenCall("你是简洁友好的中文助手，用简体中文回答，控制在80字以内，"
                  "不要用 Markdown 符号、emoji 表情或特殊符号（设备屏幕字体不支持）。",
                  question, key, model, error, 320);
}
static String gLastAnswer;

// ---------------- 语音 AI 交互状态机（问答 / 备忘 / 会议纪要共用） ----------------
// 三个页面共用同一条链路：点录音 -> micBegin 录音 -> 再点停止 -> 后台任务
//   ASR(Paraformer) -> 按模式交给 Qwen（问答 / 提待办 / 出纪要）-> 落盘 & 重绘。
// 全程异步，UI 只读状态，不阻塞触摸。凭据没配时也能走完流程，只是最后提示不可用。
enum AiVoiceMode  { AVM_CHAT = 0, AVM_MEMO, AVM_MEETING };
enum AiVoiceState { AVS_IDLE = 0, AVS_RECORDING, AVS_PROCESSING, AVS_DONE, AVS_ERROR };
static AiVoiceState gAiState = AVS_IDLE;
static AiVoiceMode  gAiMode  = AVM_CHAT;
static String gAiTranscript;                 // ASR 原文（页内小字展示）
static String gAiResult;                     // 最终答复 / 纪要 / 添加结果
static uint32_t gAiRecStartMs = 0;           // 本段录音起始时刻
static uint32_t gAiRecLimitMs = 20000;       // 一段录音的时长上限（录音机 60s，其余 20s）
static bool     gAiRecToFile  = false;       // 本段是"录音机"（停=存卡，不转写）
static void recToggleRecord(uint32_t limitMs = MIC_MAX_SAMPLES / 16);               // recorder.h 里的录音机按键（时长到顶时用它收尾）
static bool recOpenPcm(const char *path, File &wav, int &frames);
static void recStopPlay();                   // 录音前先停回放（同一组 I2S0，边放边录就是啸叫）
static bool recMinutesSave(const char *wavPath, const String &result);  // v2.9：纪要落 TF 卡
static int gAiPeak = 0, gAiRms = 0;          // 录音电平（主循环刷新，UI 画条）
// 结果归属：问答 / 备忘 / 会议各看各的，切页不串台
static AiVoiceMode gAiResultOwner = AVM_CHAT;
// 闭环：从待办页点"语音加"进来时置位，备忘识别完成写入后自动回清单
static bool gMemoBackToTodo = false;
static int  gMemoLastAdded = 0;              // 最近一次备忘实际新增条数（回清单时提示）
// 备忘转写完成后，待办页用来高亮"刚加的几条"的起始索引（<=新增前的条数）
static int  gTodoNewFrom = -1;

struct AiVoiceJob {
  AiVoiceMode mode;
  int frames;
  int16_t *pcm;
  String key;
  String model;
  String transcript;
  String recording;
  String result;
  String error;
};

// 交接 PCM 和结果的所有权。页面、待办和 NVS 都在主循环更新。
static std::atomic<AiVoiceJob *> gVoicePending{nullptr};
static std::atomic<AiVoiceJob *> gVoiceComplete{nullptr};

static bool gVoiceWorkerReady = false;

static bool aiQueueVoice(AiVoiceMode mode, int frames, int16_t *pcm, const String &text = "",
                         const char *recording = "") {
  if (!gVoiceWorkerReady) { free(pcm); return false; }
  AiVoiceJob *job = new (std::nothrow) AiVoiceJob{mode, frames, pcm, gBailianKey, gQwenModel,
                                                 text, recording, "", ""};
  if (!job) {
    free(pcm);
    return false;
  }
  gAiMode = mode;
  gAiState = AVS_PROCESSING;
  gAiCancel.store(false);
  gVoicePending.store(job);
  return true;
}

static void aiVoiceRun(AiVoiceJob &job) {
  if (gAiCancel.load()) {
    free(job.pcm);
    job.pcm = nullptr;
    return;
  }
  if (job.pcm || job.recording.length()) {
    if (job.pcm) {
      job.transcript = asrTranscribe(job.pcm, job.frames, job.key, nullptr);
      free(job.pcm);
      job.pcm = nullptr;
    } else {
      TfTransfer transfer;
      File wav;
      if (!transfer || !recOpenPcm(job.recording.c_str(), wav, job.frames)) {
        job.error = "录音读取失败";
        return;
      }
      job.transcript = asrTranscribe(nullptr, job.frames, job.key, &wav);
    }
    if (!job.transcript.length()) {
      Serial.printf("[ASR] failed: %s\n", gAsrErr.c_str());
      if (gAsrErr == "WAV_READ")
        job.error = "录音读取失败";
      else if (gAsrErr.indexOf("NO_VALID_AUDIO") >= 0)
        job.error = "没听清，靠近点再说一次";
      else if (gAsrErr == "NO_KEY" || gAsrErr == "WS_HTTP_401" || gAsrErr == "WS_HTTP_403")
        job.error = "请检查百炼 Key";
      else if (gAsrErr == "NO_WIFI")
        job.error = "请连接 Wi-Fi";
      else
        job.error = "识别失败 " + gAsrErr;
      return;
    }
  }

  String error;
  if (job.mode == AVM_CHAT)
    job.result = qwenAsk(job.transcript.c_str(), job.key, job.model, error);
  else
    job.result = qwenExtract(job.transcript.c_str(), job.mode == AVM_MEETING, job.key, job.model, error);
  if (error.length()) job.error = "百炼 " + error;
}

// netTask 复用开机分配的栈和 TLS 内存。
static bool cloudPollJobs() {
  xzPollBinding();
  auto binding = gXzState.load();
  if ((binding == XzBinding::Fetching || binding == XzBinding::Waiting) && !gAiCancel.load())
    return true;
  if (AiVoiceJob *job = gVoicePending.exchange(nullptr)) {
    aiVoiceRun(*job);
    gVoiceComplete.store(job);
    return true;
  }
  return false;
}

static void cloudApplyJobs() {
  AiVoiceJob *job = gVoiceComplete.exchange(nullptr);
  if (!job) return;
  if (gAiCancel.exchange(false)) job->error = "已取消";
  const bool textChat = !job->frames && job->mode == AVM_CHAT;
  gAiResultOwner = job->mode;
  if (textChat) {
    gLastAnswer = job->error.length() ? job->error : job->result;
    Serial.printf("[Qwen] %s\n", gLastAnswer.c_str());
  } else {
    gAiTranscript = std::move(job->transcript);
    if (job->mode == AVM_MEMO) {
      gTodoNewFrom = -1;
      gMemoLastAdded = 0;
      if (!job->error.length()) {
        gTodoNewFrom = gTodoN;
        gMemoLastAdded = todoAddLines(job->result, 8);
        if (gMemoLastAdded < 0) {
          gMemoLastAdded = 0;
          job->error = "待办保存失败";
        } else {
          job->result = gMemoLastAdded ? ("已加入 " + String(gMemoLastAdded) + " 条待办") : "没有新的待办";
        }
      }
      if (!job->frames) Serial.printf("[AI] ASRTODO added %d todos (total %d)\n", gMemoLastAdded, gTodoN);
    }
    gAiResult = job->error.length() ? job->error : job->result;
    // 会议纪要成功后立刻落盘：与录音同编号的 /REC/RECxxxx.TXT，掉电可再读再管。
    // 这里在主循环执行（cloudApplyJobs），写卡安全；失败只影响纪要文件，不影响 UI 显示。
    if (job->mode == AVM_MEETING && !job->error.length() && job->recording.length())
      recMinutesSave(job->recording.c_str(), gAiResult);
  }
  gAiState = job->error.length() ? AVS_ERROR : AVS_DONE;
  delete job;
}

// 点录音键：开始录音（第一次）/ 停止并启动后台处理（第二次）
static void aiVoiceToggle(AiVoiceMode mode) {
  if (gAiState == AVS_PROCESSING) {
    if (gAiMode == mode) gAiCancel.store(true);
    return;
  }
  if (gAiState == AVS_RECORDING) {
    if (gAiRecToFile || gAiMode != mode) return;
    micStop();
    int frames = gMicN.load();
    gAiResultOwner = mode;
    if (frames < 1600) {
      micReleaseBuffer();
      gAiResult = frames ? "录音太短" : "麦克风无采样";
      gAiState = AVS_ERROR;
      return;
    }
    int16_t *pcm = micTakeBuffer();
    if (!pcm || !aiQueueVoice(mode, frames, pcm)) {
      gAiResult = "内存不足";
      gAiState = AVS_ERROR;
    }
    return;
  }

  gAiMode = mode;
  gAiResultOwner = mode;
  gAiTranscript = ""; gAiResult = ""; gAiPeak = gAiRms = 0;
  if (mode == AVM_MEMO) { gMemoLastAdded = 0; gTodoNewFrom = -1; }
  gAiRecToFile = false;
  gAiRecLimitMs = 20000;
  recStopPlay();
  if (!micBegin(30, 16000 * 20)) {
    micReleaseBuffer();
    gAiResult = "麦克风初始化失败";
    gAiState = AVS_ERROR;
    return;
  }
  gAiRecStartMs = millis();
  gAiState = AVS_RECORDING;
}

// 主循环里周期性调用：录音中刷新电平并重绘（不阻塞）
static void aiVoicePoll() {
  if (gAiState != AVS_RECORDING) return;
  micLevel(gAiPeak, gAiRms);
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 250) { lastDraw = millis(); render(); }
  // 到时长上限自动收尾：录音机 = 存盘（不清空、不转写），其余三页 = 转写
  if (millis() - gAiRecStartMs > gAiRecLimitMs) {
    if (gAiRecToFile) recToggleRecord();
    else { aiVoiceToggle(gAiMode); render(); }
  }
}

// ---------------- 串口配置 ----------------
static void cloudCmd(const String &cmd) {
  String up = cmd; up.toUpperCase();
  if (up.startsWith("BAILIAN ")) {
    gBailianKey = cmd.substring(8); gBailianKey.trim();
    cfgSet("bailian.key", gBailianKey);
    Serial.printf("[AI] bailian key saved (%d chars)\n", gBailianKey.length());
  } else if (up == "BAILIAN") {
    // 回读走 NVS 而不是 RAM 缓存：缓存可能是开机时读空的旧值，会误导排查
    String k = cfgGetStr("bailian.key");
    Serial.printf("[AI] bailian key: %s (%d chars)\n", k.length() ? "SET" : "(empty)", k.length());
  } else if (up.startsWith("MODEL ")) {
    gQwenModel = cmd.substring(6); gQwenModel.trim();
    cfgSet("qwen.model", gQwenModel);
    Serial.printf("[AI] model -> %s\n", gQwenModel.c_str());
  } else if (up == "MODEL") {
    Serial.printf("[AI] model = %s\n", gQwenModel.c_str());
  } else if (up == "XZBIND") {
    xzRequestBinding();
    xzPrintBinding();
  } else if (up == "XZSTATUS") {
    xzPrintBinding();
  } else if (up == "XZTOKEN" || up.startsWith("XZTOKEN ")) {
    Serial.println("[XZ] XZBIND starts platform activation, XZSTATUS shows its status");
  } else if (up == "TODOS") {
    Serial.printf("[TODO] %d items\n", gTodoN);
    for (int i = 0; i < gTodoN; i++) {
      char lab[20]; uint16_t col = 0;
      todoDeadlineLabel(gTodos[i].deadline, lab, sizeof(lab), col);
      Serial.printf("  %d) [%c] %s  @%lu (%s)\n", i + 1, gTodos[i].done ? 'x' : ' ',
                    gTodos[i].text, (unsigned long)gTodos[i].deadline, lab);
    }
  } else if (up.startsWith("TODOADD ")) {
    // TODOADD <文字> [@+3]  —— 尾部 @+N 表示 N 天后，@YYYY-MM-DD 表示具体日期
    String body = cmd.substring(8);
    uint32_t dl = todoStripInlineDeadline(body);
    todoAppend(body.c_str(), dl); todoCommit(); render();
    Serial.printf("[TODO] add '%s' deadline=%lu\n", body.c_str(), (unsigned long)dl);
  } else if (up.startsWith("TODOSET ")) {
    // TODOSET <序号从1起> <+3|2026-11-20 15:00|0清除>
    String args = cmd.substring(8);
    int sp = args.indexOf(' ');
    if (sp > 0) {
      int idx = args.substring(0, sp).toInt() - 1;
      String ds = args.substring(sp + 1);
      if (idx >= 0 && idx < gTodoN) {
        gTodos[idx].deadline = ds == "0" ? 0 : todoParseDeadline(ds);
        todoCommit(); render();
        Serial.printf("[TODO] #%d deadline=%lu\n", idx + 1, (unsigned long)gTodos[idx].deadline);
      } else Serial.println("[TODO] bad index");
    } else Serial.println("[TODO] usage: TODOSET <n> <+3|YYYY-MM-DD HH:MM|0>");
  } else if (up == "TODOCLEAR") {
    gTodoN = 0; todoCommit(); render();
  } else if (up.startsWith("NETTEST")) {
    // 网络分层诊断：DNS -> TCP -> TLS，哪一层断了一眼看出来
    String host = cmd.length() > 8 ? cmd.substring(8) : "dashscope.aliyuncs.com";
    host.trim();
    IPAddress ip;
    uint32_t t0 = millis();
    bool ok = WiFi.hostByName(host.c_str(), ip);
    Serial.printf("[NET] dns %s -> %s (%lums)\n", host.c_str(),
                  ok ? ip.toString().c_str() : "FAIL", (unsigned long)(millis() - t0));
    if (ok) {
      WiFiClient plain;
      t0 = millis();
      bool tc = plain.connect(ip, 443, 5000);
      Serial.printf("[NET] tcp :443 %s (%lums)\n", tc ? "OK" : "FAIL",
                    (unsigned long)(millis() - t0));
      plain.stop();
      WiFiClientSecure sec;
      sec.setInsecure();
      sec.setHandshakeTimeout(10);
      t0 = millis();
      bool tl = sec.connect(host.c_str(), 443);
      char ebuf[128] = {0};
      if (!tl) sec.lastError(ebuf, sizeof(ebuf));   // mbedTLS 原文，别只报 FAIL
      Serial.printf("[NET] tls %s (%lums, free=%u, minfree=%u) %s\n", tl ? "OK" : "FAIL",
                    (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMinFreeHeap(), ebuf);
      sec.stop();
    }
  } else if (up.startsWith("NETTLS ")) {
    // NETTLS <ip> <port> <sni> —— 连指定 IP、用指定 SNI 握手（配合 PC 上的中继脚本抓包）
    String s = cmd.substring(7);
    int a = s.indexOf(' '), b = (a > 0) ? s.indexOf(' ', a + 1) : -1;
    if (a < 0 || b < 0) { Serial.println("[NET] usage: NETTLS <ip> <port> <sni>"); }
    else {
      IPAddress ip; ip.fromString(s.substring(0, a));
      int port = s.substring(a + 1, b).toInt();
      String sni = s.substring(b + 1); sni.trim();
      WiFiClientSecure sec;
      sec.setInsecure();
      sec.setHandshakeTimeout(15);
      uint32_t t0 = millis();
      bool ok = sec.connect(ip, (uint16_t)port, sni.c_str(), NULL, NULL, NULL);
      char ebuf[128] = {0};
      if (!ok) sec.lastError(ebuf, sizeof(ebuf));
      Serial.printf("[NET] tls %s:%d sni=%s -> %s (%lums) %s\n", ip.toString().c_str(), port,
                    sni.c_str(), ok ? "OK" : "FAIL", (unsigned long)(millis() - t0), ebuf);
      sec.stop();
    }
  } else if (up == "MEM") {
    Serial.printf("[MEM] heap=%u min=%u  int_free=%u int_largest=%u  psram_free=%u psram_largest=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  } else if (up == "AICANCEL") {
    if (gAiState == AVS_PROCESSING) gAiCancel.store(true);
  } else if (up == "AI") {
    // 语音状态机快照：验收用，无需真麦克风也能看链路状态
    static const char *ST[] = {"IDLE", "RECORDING", "PROCESSING", "DONE", "ERROR"};
    static const char *MD[] = {"CHAT", "MEMO", "MEETING"};
    Serial.printf("[AI] state=%s mode=%s peak=%d rms=%d asr=%d result=%d\n",
                  ST[(int)gAiState], MD[(int)gAiMode], gAiPeak, gAiRms,
                  gAiTranscript.length(), gAiResult.length());
    if (gAiTranscript.length()) Serial.printf("[AI] asr: %s\n", gAiTranscript.c_str());
    if (gAiResult.length())     Serial.printf("[AI] out: %s\n", gAiResult.c_str());
  } else if (up.startsWith("QWEN ") || up == "ASRTODO") {
    if (gAiState == AVS_PROCESSING || gAiState == AVS_RECORDING) {
      Serial.println("[AI] busy");
      return;
    }
    bool chat = up.startsWith("QWEN ");
    String input = chat ? cmd.substring(5) : "下午三点开会 记得买牛奶 周五前交报告";
    if (!aiQueueVoice(chat ? AVM_CHAT : AVM_MEMO, 0, nullptr, input)) Serial.println("[AI] out of memory");
    else if (chat) gLastAnswer = "思考中...";
  } else if (up.startsWith("TODODEL ")) {
    // 串口删第 n 条（1 起），验收单条删除用
    int n = cmd.substring(8).toInt();
    todoDel(n - 1); render();
    Serial.printf("[TODO] del #%d, now %d\n", n, gTodoN);
  }
}

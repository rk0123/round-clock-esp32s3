#pragma once
// ============================================================
//  小智真协议客户端 v2.7
//  ------------------------------------------------------------
//  PC 侧实测（tools/ota_probe.py）：
//    · 握手 wss://api.tenclass.net/xiaozhi/v1/ ，头 Authorization/Protocol-Version/
//      Device-Id/Client-Id；服务器 hello 永远回 format=opus sample_rate=24000，
//      即使请求 pcm 也一样 —— 所以两端都必须自己 Opus 编解码。
//    · 上行 16k 单声道 60ms 一帧（960 样本）裸 Opus 二进制帧；
//      下行 24k 单声道 Opus 二进制帧，JSON 走文本帧。
//    · 控制：listen start/stop（manual 模式由设备决定一句话什么时候结束），
//      打断用 abort。tts 的 start/sentence_start/stop 三态决定说话何时结束。
//
//  本机没有 opus 编码器（ESP32-audioI2S / ESP8266Audio 都只有解码器），
//  所以 firmware/src/opus 里 vendor 了 libopus 1.3.1 定点版（tools/vendor_opus.py 生成）。
//
//  对话模型：半双工 + 连续对话
//    点一下 → 连接 → 听（本地能量 VAD 断句）→ 松手 listen stop → 等 stt/tts
//    → 边收边解码边写 I2S（就是"小智说话"）→ tts stop → 停 400ms → 自动再听
//    再点一下 = 插话（abort），离开页面 = 关会话。
// ============================================================
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <freertos/idf_additions.h>
#include "src/opus/include/opus.h"

enum XzChat : uint8_t {
  XC_OFF = 0, XC_CONNECTING, XC_READY, XC_LISTENING, XC_THINKING, XC_SPEAKING, XC_ERROR
};
enum : uint8_t { XZC_NONE = 0, XZC_OPEN, XZC_TALK, XZC_CLOSE };

static std::atomic<XzChat>  gXzChat{XC_OFF};
static std::atomic<uint8_t> gXzCmd{XZC_NONE};
static std::atomic<bool>    gXzAuto{true};      // 连续对话（tts 结束后自动再听）
static std::atomic<uint32_t> gXzTurnNo{0};      // 第几轮，UI 用它判断"新内容"
static std::atomic<int> gXzLevel{0};            // 收音电平 0..100，给页面画小弧

// 任务写、主循环读：String 不是线程安全的，统一走这把锁
static SemaphoreHandle_t gXzTextMutex = nullptr;
static String gXzHeard;      // 服务器回的中文识别结果
static String gXzReply;      // TTS 逐句文本拼起来的答复
static String gXzErrText;    // 出错原因（页面上显示）

static void xzSetText(String &dst, const String &src) {
  if (!gXzTextMutex) { dst = src; return; }
  xSemaphoreTake(gXzTextMutex, portMAX_DELAY);
  dst = src;
  xSemaphoreGive(gXzTextMutex);
}
static String xzGetText(String &src) {
  if (!gXzTextMutex) return src;
  xSemaphoreTake(gXzTextMutex, portMAX_DELAY);
  String copy = src;
  xSemaphoreGive(gXzTextMutex);
  return copy;
}

// ---------------- 会话资源（PSRAM，全部由任务独占） ----------------
struct XzSession {
  CloudWS ws;
  String sid;
  int rate = 24000;          // 服务器下行采样率
  OpusEncoder *enc = nullptr;
  OpusDecoder *dec = nullptr;
  int16_t *pcm = nullptr;    // 960  上行一帧
  int16_t *decBuf = nullptr; // 2880 解码输出
  int16_t *up = nullptr;     // 8640 上采样到 48k
  uint8_t *pkt = nullptr;    // 640  编码输出
  uint8_t *in  = nullptr;    // 1200 下行音频帧
  bool audioOpen = false;
};
static XzSession gXzSess;

static const int XZ_FRAME = 960;        // 16k × 60ms
static const int XZ_OUT_RATE = 48000;   // 板子 I2S 固定在 48k（与 ES7210 共享时钟）

// 连接参数在**主循环**（内部 RAM 栈）里读好，语音任务只读这几个副本。
// 见 xzHandshake 里的说明：PSRAM 栈上碰 NVS 会直接 panic。
static String gXzUrlCache, gXzTokenCache, gXzMacCache, gXzClientCache;
static uint32_t gXzAudioPkts = 0, gXzAudioFrames = 0;   // 下行 TTS 音频统计（验收用）
// 验收开关：用合成音代替麦克风走一遍上行，专门用来检验"服务器下行 TTS 有没有出声"。
// 只有串口 XZTEST 会打开它，正常使用永远是 false。
static std::atomic<bool> gXzToneTest{false};

static int xzRms(const int16_t *pcm, int n) {
  int64_t sum = 0;
  for (int i = 0; i < n; i++) sum += (int64_t)pcm[i] * pcm[i];
  return (int)sqrtf((float)(sum / n));
}

static bool xzParseUrl(const String &url, String &host, String &path) {
  int p = url.indexOf("://");
  String rest = p >= 0 ? url.substring(p + 3) : url;
  int slash = rest.indexOf('/');
  if (slash < 0) { host = rest; path = "/"; } else { host = rest.substring(0, slash); path = rest.substring(slash); }
  host.trim();
  return host.length() > 0;
}

static bool xzAllocAudio() {
  if (gXzSess.pcm) return true;
  gXzSess.pcm    = (int16_t *)ps_malloc(XZ_FRAME * sizeof(int16_t));
  gXzSess.decBuf = (int16_t *)ps_malloc(2880 * sizeof(int16_t));
  gXzSess.up     = (int16_t *)ps_malloc(8640 * sizeof(int16_t));
  gXzSess.pkt    = (uint8_t *)ps_malloc(640);
  gXzSess.in     = (uint8_t *)ps_malloc(1200);
  return gXzSess.pcm && gXzSess.decBuf && gXzSess.up && gXzSess.pkt && gXzSess.in;
}

static void xzFail(const String &why) {
  Serial.printf("[XZ] FAIL %s\n", why.c_str());
  xzSetText(gXzErrText, why);
  gXzChat.store(XC_ERROR);
}

// ---------------- 下行播放：opus -> 48k PCM -> I2S0 TX ----------------
static void xzPlayPacket(const uint8_t *pkt, size_t len) {
  if (!gXzSess.dec || !gXzSess.audioOpen) return;
  int n = opus_decode(gXzSess.dec, pkt, (opus_int32)len, gXzSess.decBuf, 2880, 0);
  if (n <= 0) {
    Serial.printf("[XZ] opus_decode %d\n", n);
    return;
  }
  int ratio = XZ_OUT_RATE / gXzSess.rate;
  if (ratio < 1 || ratio > 6) ratio = 1;
  // ⚠ 音频库的 I2S TX 固定是 16bit **立体声**（Audio.cpp:273 STEREO）。
  // 单声道样本直接写进去会被当成 L/R 交替的帧 → 每个声道只拿到一半样本，
  // 声音变慢一倍、音调低八度。必须每个样本左右各写一份。
  int outN = 0;
  for (int i = 0; i < n; i++) {
    const int16_t cur = gXzSess.decBuf[i];
    const int16_t nxt = (i + 1 < n) ? gXzSess.decBuf[i + 1] : cur;
    for (int k = 0; k < ratio; k++) {
      if (outN + 2 > 8640) break;
      const int16_t v = (int16_t)(cur + ((int32_t)(nxt - cur) * k) / ratio);
      gXzSess.up[outN++] = v;
      gXzSess.up[outN++] = v;
    }
  }
  if (outN) {
    const int wrote = audioPcmWrite(gXzSess.up, outN, 400);
    gXzAudioPkts++;
    if (wrote > 0) gXzAudioFrames += wrote / 2;   // 立体声，帧数 = 样本数/2
    if (gXzAudioPkts <= 3 || wrote <= 0)
      Serial.printf("[XZ] audio #%u dec=%d out=%d wrote=%d\n",
                    (unsigned)gXzAudioPkts, n, outN, wrote);
  }
}

// ---------------- JSON 分发 ----------------
// 返回 true 表示"这一轮说完了"
static bool xzHandleJson(const String &msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg)) return false;
  const char *type = doc["type"] | "";
  // 排障用：把每一帧文本消息的类型和缩略内容打出来（音频帧是二进制，不走这里）
  String brief = msg.length() > 160 ? msg.substring(0, 160) + "..." : msg;
  Serial.printf("[XZ] rx [%s] %s\n", type, brief.c_str());
  if (!strcmp(type, "stt")) {
    String text = String(doc["text"] | "");
    if (text.length()) {
      xzSetText(gXzHeard, text);
      xzSetText(gXzReply, "");      // 新一轮开始，清掉上一句答复
      gXzChat.store(XC_THINKING);
      Serial.printf("[XZ] stt: %s\n", text.c_str());
    }
    return false;
  }
  if (!strcmp(type, "tts")) {
    const char *st = doc["state"] | "";
    if (!strcmp(st, "start")) {
      gXzChat.store(XC_SPEAKING);
    } else if (!strcmp(st, "sentence_start")) {
      String line = String(doc["text"] | "");
      // 服务端会把工具调用当成 tts 文本发过来（"% search_music..." / "% play_music..."），
      // 这些不是给人听的话，别显示也别念。
      if (line.startsWith("%")) line = "";
      if (line.length()) {
        String merged = xzGetText(gXzReply);
        if (merged.length()) merged += "\n";
        merged += line;
        xzSetText(gXzReply, merged);
        Serial.printf("[XZ] tts: %s\n", line.c_str());
      }
    } else if (!strcmp(st, "stop")) {
      return true;
    }
    return false;
  }
  if (!strcmp(type, "llm")) {
    const char *emo = doc["emotion"] | "";
    if (*emo) Serial.printf("[XZ] emotion: %s\n", emo);
    return false;
  }
  if (!strcmp(type, "alert")) {
    String m = String(doc["message"] | "");
    if (m.length()) xzSetText(gXzErrText, m);
    return false;
  }
  if (!strcmp(type, "system")) {
    Serial.printf("[XZ] system: %s\n", (const char *)(doc["command"] | ""));
    return false;
  }
  return false;
}

// ---------------- 建链 ----------------
static bool xzHandshake() {
  // ⚠ 这里**绝不能**直接读 NVS：本任务若跑在 PSRAM 栈上，Preferences::getString
  // → nvs_get_str → esp_partition_read 会在关 cache 时断言 panic
  // （esp_task_stack_is_sane_cache_disabled）。连接参数由 xzChatToggle 预先缓存。
  const String &url = gXzUrlCache;
  const String &token = gXzTokenCache;
  String host, path;
  if (!xzParseUrl(url, host, path)) { xzFail("URL 无效"); return false; }
  if (!token.length()) { xzFail("先去绑定设备"); return false; }

  String extra = String("Protocol-Version: 1\r\nDevice-Id: ") + gXzMacCache +
                 "\r\nClient-Id: " + gXzClientCache + "\r\n";
  Serial.printf("[XZ] connect %s%s token=%d mac=%s\n", host.c_str(), path.c_str(),
                (int)token.length(), gXzMacCache.c_str());
  Serial.printf("[XZ] heap int=%u blk=%u dma=%u ps=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  // mbedtls 握手要两块 16KB 连续内部内存（IN/OUT 记录缓冲）。开机时天气/ADSB
  // 的 TLS 还没退干净，这里会撞上 "SSL - Memory allocation failed"。
  // 等一等再试：netTask 一收工就有 70KB 以上，握手就过得去。
  bool opened = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (gXzSess.ws.begin(host.c_str(), path.c_str(), token, extra)) { opened = true; break; }
    Serial.printf("[XZ] ws attempt %d failed: %s (int=%u)\n", attempt + 1,
                  gXzSess.ws.error.c_str(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    delay(700 + attempt * 400);
  }
  if (!opened) { xzFail("连接失败 " + gXzSess.ws.error); return false; }
  // features.aec=false：本板没有硬件回声消除，靠半双工（说的时候不收音）避回声
  const char *hello =
      "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
      "\"features\":{\"mcp\":false,\"aec\":false},"
      "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
  if (!gXzSess.ws.sendText(hello)) { xzFail("发送 hello 失败"); return false; }

  const uint32_t until = millis() + 10000;
  String msg;
  size_t binLen = 0;
  while ((int32_t)(until - millis()) > 0) {
    int r = gXzSess.ws.recvMessage(msg, gXzSess.in, 1200, binLen, 1000);
    if (r < 0) { xzFail("握手失败 " + gXzSess.ws.error); return false; }
    if (r != 1) continue;
    JsonDocument doc;
    if (deserializeJson(doc, msg)) continue;
    const char *type = doc["type"] | "";
    if (strcmp(type, "hello")) continue;
    gXzSess.sid = String(doc["session_id"] | "");
    int sr = doc["audio_params"]["sample_rate"] | 24000;
    gXzSess.rate = (sr >= 8000 && sr <= 48000) ? sr : 24000;
    Serial.printf("[XZ] hello sid=%s rate=%d\n", gXzSess.sid.c_str(), gXzSess.rate);

    int err = 0;
    if (!gXzSess.enc) gXzSess.enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!gXzSess.enc) { xzFail(String("编码器 ") + err); return false; }
    opus_encoder_ctl(gXzSess.enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(gXzSess.enc, OPUS_SET_COMPLEXITY(1));   // 240MHz 上 complexity1 才够实时
    opus_encoder_ctl(gXzSess.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(gXzSess.enc, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(gXzSess.enc, OPUS_SET_DTX(0));
    if (!gXzSess.dec) gXzSess.dec = opus_decoder_create(gXzSess.rate, 1, &err);
    if (!gXzSess.dec) { xzFail(String("解码器 ") + err); return false; }
    return true;
  }
  xzFail("等不到 hello");
  return false;
}

// ---------------- 听：本地 VAD 断句 ----------------
static bool xzListen() {
  const size_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!micBegin(30, 0, true)) { xzFail("麦克风不可用"); return false; }
  const size_t h1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  Serial.printf("[XZ] listen start");

  // 底噪探针借 decBuf（PSRAM）当临时缓冲：960 样本放栈上会白吃 1.9KB，
  // 这个任务里每一分栈都很紧。
  int noise = 0;
  if (micStreamRead(gXzSess.decBuf, XZ_FRAME, 400) == XZ_FRAME)
    noise = xzRms(gXzSess.decBuf, XZ_FRAME);
  // 30dB 增益下本机底噪实测 RMS 2000 上下，旧口径 max(260, noise*3) 会被
  // 底噪直接顶穿（阈值 4000 vs 峰值 4081）→ 静音也判定成"说话了"。
  // 新口径：比底噪高 70% 且绝对值不低于 1200，上限 6000 保证远处说话还能触发。
  const int thresh = constrain(noise + noise / 2, 1200, 6000);
  Serial.printf(" noise=%d thresh=%d\n", noise, thresh);

  String start = String("{\"session_id\":\"") + gXzSess.sid +
                 "\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}";
  gXzSess.ws.sendText(start);
  gXzChat.store(XC_LISTENING);
  xzSetText(gXzErrText, "");

  const uint32_t t0 = millis();
  uint32_t lastVoice = t0;
  bool spoke = false;
  int peak = 0;
  int voiced = 0;          // 超阈值的帧数（60ms/帧）。1 帧噪声不许当成"开口"。

  while ((int32_t)(t0 + 15000 - millis()) > 0) {
    // 边听边收：服务器可能在说话前就推 stt
    String msg;
    size_t binLen = 0;
    while (gXzSess.ws.recvMessage(msg, gXzSess.in, 1200, binLen, 0) == 1) xzHandleJson(msg);

    const uint8_t cmd = gXzCmd.load();
    if (cmd == XZC_CLOSE) break;

    int level = 0;
    if (gXzToneTest.load()) {
      // 1.5kHz 方波 + 慢包络，足够让 VAD 认定"在说话"，也足够服务端回一句
      static uint32_t ph = 0;
      for (int i = 0; i < XZ_FRAME; i++) {
        ph += 1;
        const int env = 6000 + 3000 * ((ph / 1600) % 2);     // 每 100ms 起伏
        gXzSess.pcm[i] = (int16_t)(((ph / 16) & 1) ? env : -env);
      }
      level = 7000;
    } else {
      const int got = micStreamRead(gXzSess.pcm, XZ_FRAME, 200);
      if (got < XZ_FRAME) {
        if (!gMicOn.load()) break;
        continue;
      }
      level = xzRms(gXzSess.pcm, XZ_FRAME);
    }
    if (level > peak) peak = level;
    gXzLevel.store(constrain(level / 40, 0, 100));
    if (level > thresh) { spoke = true; voiced++; lastVoice = millis(); }

    const int n = opus_encode(gXzSess.enc, gXzSess.pcm, XZ_FRAME, gXzSess.pkt, 640);
    if (n > 0) gXzSess.ws.sendBinary(gXzSess.pkt, n);
    else if (n < 0) { Serial.printf("[XZ] encode %d\n", n); break; }

    if (cmd == XZC_TALK) break;                       // 用户又点了一下 = 说完
    // 断句：持续开口（>=4 帧 240ms）之后静音 900ms 才算说完。
    // 只按 1 帧就停会把环境噪声当成一句话传上去，服务端只会回"没听清"。
    if (gXzToneTest.load() && voiced >= 25) break;
    if (voiced >= 4 && millis() - lastVoice > 900) break;
  }

  String stop = String("{\"session_id\":\"") + gXzSess.sid + "\",\"type\":\"listen\",\"state\":\"stop\"}";
  gXzSess.ws.sendText(stop);
  micStop();
  gXzLevel.store(0);
  Serial.printf("[XZ] listen stop spoke=%d voiced=%d peak=%d heap %u->%u->%u\n", (int)spoke, voiced, peak,
                (unsigned)h0, (unsigned)h1, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  if (voiced < 4) return false;      // 只是噪声，别去等服务端（它会回"没听清"，白等 20s）
  gXzChat.store(XC_THINKING);
  return true;
}

// ---------------- 等答复 + 边收边放 ----------------
// 返回 true = 正常说完（可以进下一轮），false = 需要重新听 / 会话结束
static bool xzAwaitAnswer() {
  const uint32_t until = millis() + 20000;
  bool speaking = false;
  bool finished = false;
  while ((int32_t)(until - millis()) > 0) {
    const uint8_t cmd = gXzCmd.load();
    if (cmd == XZC_CLOSE) return false;
    if (cmd == XZC_TALK) {                              // 插话
      String abortMsg = String("{\"session_id\":\"") + gXzSess.sid +
                        "\",\"type\":\"abort\",\"reason\":\"wake_word_detected\"}";
      gXzSess.ws.sendText(abortMsg);
      gXzSess.ws.sendText(String("{\"session_id\":\"") + gXzSess.sid +
                          "\",\"type\":\"listen\",\"state\":\"stop\"}");
      return false;
    }

    String msg;
    size_t binLen = 0;
    int r = gXzSess.ws.recvMessage(msg, gXzSess.in, 1200, binLen, 200);
    if (r < 0) { xzFail("连接断开 " + gXzSess.ws.error); return false; }
    if (r == 1) {
      if (xzHandleJson(msg)) { finished = true; break; }
      if (gXzChat.load() == XC_SPEAKING) speaking = true;
      continue;
    }
    if (r == 2) {
      if (!gXzSess.audioOpen) {
        audioPcmBegin();
        gXzSess.audioOpen = true;
        speaking = true;
        gXzChat.store(XC_SPEAKING);
      }
      xzPlayPacket(gXzSess.in, binLen);
    }
  }
  if (gXzSess.audioOpen) {
    delay(350);                     // 让最后一块 DMA 播完，别把尾音掐掉
    audioPcmEnd();
    gXzSess.audioOpen = false;
  }
  Serial.printf("[XZ] answer finished=%d speaking=%d audio pkt=%u frames=%u\n",
                (int)finished, (int)speaking, (unsigned)gXzAudioPkts, (unsigned)gXzAudioFrames);
  if (!finished && !speaking) xzSetText(gXzErrText, "小智没有回应");
  return finished;
}

// ---------------- 会话主循环 ----------------
static void xzCloseSession() {
  if (gXzSess.audioOpen) { audioPcmEnd(); gXzSess.audioOpen = false; }
  gXzSess.ws.close();
  gXzSess.sid = "";
  if (gMicOn.load()) micStop();
  gXzChat.store(XC_OFF);
}

static void xzRunSession() {
  gXzChat.store(XC_CONNECTING);
  xzSetText(gXzErrText, "");
  if (!xzAllocAudio()) { xzFail("PSRAM 不足"); return; }
  if (WiFi.status() != WL_CONNECTED) { xzFail("请连接 Wi-Fi"); return; }
  if (!xzHandshake()) return;

  gXzChat.store(XC_READY);
  gXzCmd.store(XZC_TALK);          // 连上就立刻开始第一轮
  for (;;) {
    const uint8_t cmd = gXzCmd.load();
    if (cmd == XZC_CLOSE) break;
    gXzCmd.store(XZC_NONE);
    if (!xzListen()) {
      if (gXzCmd.load() == XZC_CLOSE) break;
      gXzChat.store(XC_READY);
      // 没听到声音：连续模式下再给一次机会，否则退回待机
      if (!gXzAuto.load()) break;
      continue;
    }
    gXzTurnNo.fetch_add(1);
    xzAwaitAnswer();
    if (gXzCmd.load() == XZC_CLOSE) break;
    if (!gXzAuto.load()) break;
    delay(350);                    // 说完稍等，避免自己的尾音被当作下一句
  }
  xzCloseSession();
}

static void xzChatTask(void *) {
  for (;;) {
    // 只认 OPEN 这个"请求"。不能顺带判 gXzChat==XC_OFF —— xzChatToggle 为了让
    // 页面立刻显示"连接小智"已经把状态置成 XC_CONNECTING，旧条件永远不成立。
    if (gXzCmd.load() == XZC_OPEN) {
      gXzCmd.store(XZC_NONE);
      xzRunSession();
    }
    delay(50);
  }
}

// ---------------- 对外 API ----------------
static bool gXzTaskReady = false;

static void xzEnsureTask() {
  if (gXzTaskReady) return;
  if (!gXzTextMutex) gXzTextMutex = xSemaphoreCreateMutex();
  // 栈放 PSRAM：mbedtls 握手 + opus 编解码 + JSON 解析太吃栈，16KB 内部栈当场溢出
  // （实测 "A stack overflow in task xzchat"）；而内部 RAM 还要留给 TLS 的
  // 两块 16KB 记录缓冲，抢不得。
  // ⚠ 代价：这个任务里**绝对不能**碰 NVS/flash 写（Preferences、nvs_set_*、OTA），
  // 否则会在 spi_flash_disable_interrupts_caches 里断言重启。所有配置读取都在
  // xzChatToggle（主循环栈）里做完，存成 gXzUrlCache 等副本。
  TaskHandle_t h = nullptr;
  if (xTaskCreatePinnedToCoreWithCaps(xzChatTask, "xzchat", 32768, nullptr, 2, &h, 1,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    Serial.println("[XZ] psram stack failed, fallback to internal");
    if (xTaskCreatePinnedToCore(xzChatTask, "xzchat", 20480, nullptr, 2, &h, 1) != pdPASS) {
      Serial.println("[XZ] task create failed");
      return;
    }
  }
  gXzTaskReady = true;
  Serial.println("[XZ] voice task ready");
}

// 点一下说话键：第一次=开会话，之后=说完了/插话
static void xzChatToggle() {
  xzEnsureTask();
  if (!gXzTaskReady) { xzSetText(gXzErrText, "任务创建失败"); gXzChat.store(XC_ERROR); return; }
  const XzChat state = gXzChat.load();
  if (state == XC_OFF || state == XC_ERROR) {
    if (WiFi.status() != WL_CONNECTED) { xzSetText(gXzErrText, "请连接 Wi-Fi"); gXzChat.store(XC_ERROR); return; }
    // 在主循环（内部 RAM 栈）里把 NVS 读出来缓存，任务侧只读副本
    gXzUrlCache    = xzWsUrl();
    gXzTokenCache  = xzWsToken();
    gXzMacCache    = String(xzDeviceMac());
    gXzClientCache = String(xzClientId());
    if (!gXzTokenCache.length()) {         // 还没拿到 websocket 配置
      xzRequestBinding();
      xzSetText(gXzErrText, "正在取配置，稍后再点");
      gXzChat.store(XC_OFF);
      return;
    }
    gXzSess.ws.close();
    xzSetText(gXzHeard, "");
    xzSetText(gXzReply, "");
    gXzCmd.store(XZC_OPEN);
    gXzChat.store(XC_CONNECTING);
    return;
  }
  if (state == XC_CONNECTING) return;
  gXzCmd.store(XZC_TALK);
}

static void xzChatStop() {
  gXzToneTest.store(false);
  if (gXzChat.load() == XC_OFF) return;
  gXzCmd.store(XZC_CLOSE);
}

static bool xzChatActive() { return gXzChat.load() != XC_OFF; }

static const char *xzStateName(XzChat s) {
  switch (s) {
    case XC_OFF: return "IDLE";
    case XC_CONNECTING: return "CONNECT";
    case XC_READY: return "READY";
    case XC_LISTENING: return "LISTEN";
    case XC_THINKING: return "THINK";
    case XC_SPEAKING: return "SPEAK";
    default: return "ERROR";
  }
}

static void xzStatusDump() {
  Serial.printf("[XZ] chat=%s auto=%d turn=%u rate=%d sid=%s\n",
                xzStateName(gXzChat.load()), gXzAuto.load() ? 1 : 0,
                (unsigned)gXzTurnNo.load(), gXzSess.rate, gXzSess.sid.c_str());
  Serial.printf("[XZ] heard=%s\n[XZ] reply=%s\n[XZ] err=%s\n",
                xzGetText(gXzHeard).c_str(), xzGetText(gXzReply).c_str(),
                xzGetText(gXzErrText).c_str());
  Serial.printf("[XZ] ws=%s token=%d chars\n", xzWsUrl().c_str(), xzWsToken().length());
}

// 串口验收：XZCHAT 开会话 / XZCLOSE / XZAUTO 0|1 / XZSAY（等同点一下）
static bool xzChatCmd(const String &cmd) {
  String up = cmd; up.toUpperCase();
  if (up == "XZCHAT") { xzChatToggle(); return true; }
  if (up == "XZCLOSE") { xzChatStop(); return true; }
  if (up == "XZINFO") { xzStatusDump(); return true; }
  if (up == "XZTEST") {                        // 合成音自测：验证下行 TTS 到底有没有出声
    gXzToneTest.store(true);
    xzChatToggle();
    return true;
  }
  if (up.startsWith("XZAUTO")) { gXzAuto.store(cmd.substring(6).toInt() != 0); xzStatusDump(); return true; }
  return false;
}

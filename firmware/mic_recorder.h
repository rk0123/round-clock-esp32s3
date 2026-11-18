#pragma once
// ============================================================
//  双麦录音 v2.0  ·  ES7210 + I2S0 全双工 RX
//  ⚠ v1.0 用 I2S1 的 slave RX 是**行不通的**（实测 samples 恒为 0）：
//  这块板 ES7210 的 BCLK(48)/LRCK(38)/DIN(39) 与 ES8311 播放侧**硬连在同一组线上**，
//  而 ESP32 的一个 GPIO 只有一个 out_sel。I2S0(master) 正在输出 BCLK/LRCK，
//  I2S1 却要把同一组脚当输入 → 收不到时钟，DMA 永远等不到数据。
//  正解（也是 ESP-IDF / esp_codec_dev 的标准做法）：**I2S0 全双工** ——
//  TX 喂 ES8311 出声、RX 收 ES7210 的麦克风，两者天然共享同一组 BCLK/LRCK。
//  为此给 ESP32-audioI2S 打了 patch 7/7：i2s_new_channel 同时拿 tx 和 rx 句柄。
//  采样：ES7210 48 kHz 立体声，左声道每 3 帧取均值，输出 16 kHz 单声道。
// ============================================================
#include "es7210.h"
#include <atomic>

static i2s_chan_handle_t gMicChan   = nullptr;   // I2S0 的 RX handle（属于音频库，不 del）
static bool              gMicReady  = false;     // RX 通道是否已 init_std_mode
static es7210_dev_handle_t gMicCodec= nullptr;
static int16_t *gMicBuf = nullptr;
static std::atomic<int> gMicN{0};
static std::atomic<bool> gMicOn{false};
static std::atomic<bool> gMicTaskActive{false};
#define MIC_MAX_SAMPLES   (16000 * 60)

static void micTask(void *) {
  static int16_t raw[768];
  int count = 0;
  int dec = 0, sum = 0;
  esp_err_t lastError = ESP_OK;
  while (gMicOn.load() && count < MIC_MAX_SAMPLES) {
    size_t got = 0;
    esp_err_t status = i2s_channel_read(gMicChan, raw, sizeof(raw), &got, 50);
    if (status != ESP_OK) lastError = status;
    if (status != ESP_OK && status != ESP_ERR_TIMEOUT) { delay(5); continue; }
    const int frames = got / (2 * sizeof(int16_t));
    for (int i = 0; i < frames && count < MIC_MAX_SAMPLES; ++i) {
      sum += raw[i * 2];
      if (++dec == 3) {
        gMicBuf[count++] = sum / 3;
        sum = 0;
        dec = 0;
      }
    }
    gMicN.store(count);
  }
  if (count == 0) Serial.printf("[MIC] no PCM, I2S status=0x%x\n", (unsigned)lastError);
  gMicTaskActive.store(false);
  vTaskDelete(nullptr);
}

static void micFreeCodec() {
  I2C_Lock();
  if (gMicCodec) { es7210_del_codec(gMicCodec); gMicCodec = nullptr; }
  I2C_Unlock();
}

// 只读诊断：统计 BCLK(48)/WS(38)/DIN(39) 在 ms 毫秒内的电平跳变次数。
// 不碰 out_sel、不改方向，只把 input buffer 打开（输出功能不受影响），
// 用来判定"时钟有没有到 / ES7210 有没有吐数据"。
static void micPinEdges(uint32_t ms, const char *tag) {
  static const int PIN[3] = {48, 38, 39};
  int last[3], cnt[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    // 48/38 被 I2S0 当输出用，input buffer 是关的；切成 INPUT_OUTPUT 才能一边保持
    // I2S0 的 out_sel 输出、一边读电平。39 是纯输入，切成 INPUT_OUTPUT 反而会去驱动它。
    gpio_set_direction((gpio_num_t)PIN[i],
                       PIN[i] == 39 ? GPIO_MODE_INPUT : GPIO_MODE_INPUT_OUTPUT);
    last[i] = gpio_get_level((gpio_num_t)PIN[i]);
  }
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    for (int i = 0; i < 3; i++) {
      int v = gpio_get_level((gpio_num_t)PIN[i]);
      if (v != last[i]) { last[i] = v; cnt[i]++; }
    }
  }
  Serial.printf("[MIC] %-11s BCLK48=%-6d WS38=%-6d DIN39=%-6d (edges/%lums)\n",
                tag, cnt[0], cnt[1], cnt[2], (unsigned long)ms);
}

static bool micBegin(int gainDb = 30) {
  if (gMicOn.load()) return false;
  if (!gAudioReady || !gAudio) { Serial.println("[MIC] audio subsystem not ready"); return false; }
  gMicChan = gAudio->getRxHandle();
  if (!gMicChan) {
    Serial.println("[MIC] RX handle missing, check ESP32-audioI2S full-duplex patch");
    return false;
  }
  if (gAudioState == A_PLAYING || gAudioState == A_CONNECTING) audioStop();
  if (!gMicBuf) gMicBuf = (int16_t *)ps_malloc(MIC_MAX_SAMPLES * sizeof(int16_t));
  if (!gMicBuf) { Serial.println("[MIC] PSRAM alloc FAILED"); return false; }

  // ES7210 全部 I2C 操作都在总线锁内（触摸任务 100Hz 也用 Wire）
  I2C_Lock();
  es7210_i2c_config_t cfg = {};
  cfg.i2c_addr = 0x40;
  esp_err_t e = es7210_new_codec(&cfg, &gMicCodec);
  if (e == ESP_OK) {
    es7210_codec_config_t cc = {};
    cc.i2s_format    = ES7210_I2S_FMT_I2S;
    cc.mclk_ratio    = 256;
    cc.sample_rate_hz= 48000;                   // 跟随播放侧的 48k 时钟
    cc.bit_width     = ES7210_I2S_BITS_16B;
    cc.mic_bias      = ES7210_MIC_BIAS_2V87;
    cc.mic_gain      = gainDb >= 36 ? ES7210_MIC_GAIN_36DB
                     : gainDb >= 30 ? ES7210_MIC_GAIN_30DB
                     : gainDb >= 24 ? ES7210_MIC_GAIN_24DB
                                    : ES7210_MIC_GAIN_18DB;
    cc.flags.tdm_enable = false;
    e = es7210_config_codec(gMicCodec, &cc);
    if (e != ESP_OK) {
      Serial.printf("[MIC] config_codec failed err=0x%x\n", (int)e);
    } else {
      // ⚠ 这个接口的范围是 -95..32 dB，**传 40 会直接返回 ESP_ERR_INVALID_ARG(0x102)**，
      // 以前三步笼统打一行 "config failed"，把"增益越界"误看成"I2C 没应答"。上限就是 32。
      e = es7210_config_volume(gMicCodec, 30);
      if (e != ESP_OK) Serial.printf("[MIC] config_volume failed err=0x%x\n", (int)e);
    }
  } else {
    Serial.printf("[MIC] es7210_new_codec failed err=0x%x (I2C 0x40 无应答?)\n", (int)e);
  }
  I2C_Unlock();
  if (e != ESP_OK) {
    Serial.printf("[MIC] ES7210 config failed err=0x%x\n", (int)e);
    micFreeCodec();
    return false;
  }

  // I2S0 全双工 RX：句柄来自音频库（同一个 controller，天生共享 BCLK/LRCK）。
  // 只声明 din；bclk/ws 也写上（同一 controller 的同一组脚，IDF 允许重复声明）。
  if (!gMicReady) {
    Serial.printf("[MIC] before rx init: dma-free=%u int-free=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    i2s_std_config_t rxstd = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,        // MCLK 只由 TX 输出一次
        .bclk = (gpio_num_t)48,         // 与播放共用
        .ws   = (gpio_num_t)38,
        .dout = I2S_GPIO_UNUSED,
        .din  = (gpio_num_t)39,         // ES7210 SDOUT
      },
    };
    e = i2s_channel_init_std_mode(gMicChan, &rxstd);
    if (e != ESP_OK) {
      Serial.printf("[MIC] rx init_std_mode failed 0x%x\n", (int)e);
      micFreeCodec();
      return false;
    }
    gMicReady = true;
  }
  e = i2s_channel_enable(gMicChan);
  if (e != ESP_OK) {
    Serial.printf("[MIC] rx enable failed 0x%x\n", (int)e);
    micFreeCodec();
    return false;
  }

  gMicN.store(0);
  gMicOn.store(true);
  gMicTaskActive.store(true);
  if (xTaskCreatePinnedToCore(micTask, "micTask", 4096, nullptr, 2, nullptr, 1) != pdPASS) {
    gMicTaskActive.store(false);
    gMicOn.store(false);
    i2s_channel_disable(gMicChan);
    micFreeCodec();
    Serial.println("[MIC] task creation failed");
    return false;
  }
  Serial.printf("[MIC] ES7210 @0x40 ready, I2S0 full-duplex RX 48k->16k, gain=%d dB\n", gainDb);
  return true;
}

static void micStop() {
  if (!gMicOn.exchange(false)) return;
  // 等采音任务写完最后一块 PCM，再关 RX 和释放 codec。
  while (gMicTaskActive.load()) delay(2);
  i2s_channel_disable(gMicChan);
  micFreeCodec();
  Serial.printf("[MIC] stopped, %d samples\n", gMicN.load());
}

// 返回最近 200ms 的峰值 / RMS，给录音页画电平条
static void micLevel(int &peak, int &rms) {
  peak = rms = 0;
  const int count = gMicN.load();
  if (!gMicBuf || count < 16) return;
  int from = max(0, count - 3200);
  // ⚠ long 在 ESP32 上只有 32 位：3200 个 32768² 累加 ≈ 3.4e12 会溢出成 INT_MAX。
  // 之前播放中测出 rms=2147483647 就是这个 bug，不是录音坏。
  int64_t sum = 0;
  for (int i = from; i < count; i++) {
    int v = abs(gMicBuf[i]);
    if (v > peak) peak = v;
    sum += (int64_t)v * (int64_t)v;
  }
  rms = (int)sqrtf((float)(sum / (count - from)));
}

// 串口验收入口：MICTEST [秒]，打印峰值/RMS 和前 16 个采样
static void micCmd(const String &cmd) {
  if (cmd == "MIC") {
    Serial.printf("[MIC] state=%s n=%d/%d buf=%s\n",
                  gMicOn.load() ? "ON" : "OFF", gMicN.load(), MIC_MAX_SAMPLES,
                  gMicBuf ? "PSRAM" : "none");
    return;
  }
  if (cmd == "MICPIN") {
    micPinEdges(150, "idle");
    if (micBegin()) {
      micPinEdges(150, "afterBegin");
      micStop();
      micPinEdges(150, "afterStop");
    }
    return;
  }
  if (cmd.startsWith("MICTEST")) {
    int secs = cmd.length() > 8 ? cmd.substring(8).toInt() : 3;
    if (secs < 1) secs = 3;
    if (secs > 60) secs = 60;
    if (!micBegin()) return;
    delay(secs * 1000);
    micStop();
    int peak, rms; micLevel(peak, rms);
    Serial.printf("[MIC] %ds test: samples=%d peak=%d rms=%d\n", secs, gMicN.load(), peak, rms);
    Serial.print("[MIC] first16:");
    for (int i = 0; i < min(16, gMicN.load()); i++) Serial.printf(" %d", gMicBuf[i]);
    Serial.println();
    return;
  }
}

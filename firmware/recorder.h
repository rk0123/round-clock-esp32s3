#pragma once
// ============================================================
//  录音机 v2.5  ·  录音 → TF 卡 WAV（16kHz / 16bit / mono）→ 可回放
//  用户口径（2026-11-18）：
//    ① "录音机首先是录音机功能，额外提供文字纪要功能可选"
//       —— 以前按停止就自动跑 ASR + Qwen，录完还要干等十几秒，还不能回放。
//          现在停止 = 存盘 + 回列表，纪要点「纪要」键才跑。
//    ② "录音机要可以回放的"  —— 回放键播当前选中那一段，列表里可以挑别的。
//    ③ "文件保存在tf卡上"    —— /REC/REC0001.WAV，编号自增，不占内部 flash。
//  PCM 来源就是 mic_recorder.h 的 gMicBuf（I2S0 全双工 RX 48k→16k）。
//  回放走 audio_player.h 的 AQ_PLAYFILE（gToneMode=true：播完就停，不续播不重连）。
// ============================================================
#include <SD_MMC.h>

static void showToast(const char *s);   // 定义在 main.cpp（本头文件先于它展开）

#define REC_DIR        "/REC"
#define REC_MAX        24            // 列表最多列 24 段（UI 一页 4 条，够翻）
#define REC_SR         16000
#define REC_BYTES_SEC  (REC_SR * 2)  // 16bit mono = 32000 B/s

struct RecItem {
  char     name[16];                 // REC0003.WAV（只存文件名）
  uint32_t bytes;
  uint16_t secs;
};

static RecItem gRec[REC_MAX];
static int  gRecN        = 0;        // 有效条数（新的在前）
static int  gRecSel      = 0;        // 当前选中下标
static bool gRecListOpen = false;    // 录音列表子页
static bool gRecScanned  = false;    // 进过录音页并成功枚举过一次卡里的录音
static int  gRecListPage = 0;
static bool gRecPlaying  = false;    // 本次播放是"回放录音"（不是电台/曲库）
static uint32_t gRecPlayStartMs = 0; // 播放命令发出时刻（队列还没被 audioTask 取走时用宽限期）
static char gRecMsg[28]  = "";       // 一次性提示：已存 12 秒 / NO TF CARD …
static uint32_t gRecMsgUntil = 0;    // 提示到期时刻（3 秒后让位给"共 N 段"）

static void recSetMsg(const char *s) {
  snprintf(gRecMsg, sizeof(gRecMsg), "%s", s);
  gRecMsgUntil = millis() + 3000;
}

// 目录里已有编号的最大值 +1
static int recNextNum() {
  int mx = 0;
  for (int i = 0; i < gRecN; i++) {
    int n = atoi(gRec[i].name + 3);
    if (n > mx) mx = n;
  }
  return mx + 1;
}

static void recScan() {
  gRecN = 0;
  if (!tfEnsure()) { gRecPlaying = false; return; }
  File d = SD_MMC.open(REC_DIR);
  if (!d || !d.isDirectory()) return;
  for (;;) {
    File f = d.openNextFile();
    if (!f) break;
    if (!f.isDirectory() && gRecN < REC_MAX) {
      String n = f.name();
      String l = n; l.toLowerCase();
      if (l.endsWith(".wav")) {
        // 有的核心给的是带路径的名字，取最后一段
        int sl = n.lastIndexOf('/');
        if (sl >= 0) n = n.substring(sl + 1);
        RecItem &it = gRec[gRecN++];
        strncpy(it.name, n.c_str(), sizeof(it.name) - 1);
        it.name[sizeof(it.name) - 1] = 0;
        it.bytes = f.size();
        it.secs = (it.bytes > 44) ? (uint16_t)((it.bytes - 44) / REC_BYTES_SEC) : 0;
      }
    }
    f.close();
  }
  d.close();
  // 编号大的新 → 倒序排，"最新"永远在 [0]
  for (int i = 1; i < gRecN; i++) {
    RecItem key = gRec[i]; int j = i - 1;
    while (j >= 0 && strcmp(gRec[j].name, key.name) < 0) { gRec[j + 1] = gRec[j]; j--; }
    gRec[j + 1] = key;
  }
  if (gRecSel >= gRecN) gRecSel = 0;
}

static const char *recPathOf(int idx, char *buf, size_t cap) {
  if (idx < 0 || idx >= gRecN) { buf[0] = 0; return buf; }
  snprintf(buf, cap, REC_DIR "/%s", gRec[idx].name);
  return buf;
}

// 把 gMicBuf 里 frames 个采样写成 WAV。返回写入的秒数（0 = 失败）
static int recSaveFromMic(int frames) {
  gRecMsg[0] = 0;
  if (!tfEnsure()) { recSetMsg("NO TF CARD"); return 0; }
  if (!SD_MMC.exists(REC_DIR)) SD_MMC.mkdir(REC_DIR);
  recScan();
  char name[16];
  snprintf(name, sizeof(name), "REC%04d.WAV", recNextNum());
  char path[32];
  snprintf(path, sizeof(path), REC_DIR "/%s", name);

  uint32_t dataLen = (uint32_t)frames * 2;
  uint8_t h[44] = {0};
  memcpy(h + 0, "RIFF", 4);  uint32_t riff = 36 + dataLen; memcpy(h + 4, &riff, 4);
  memcpy(h + 8, "WAVE", 4);  memcpy(h + 12, "fmt ", 4);
  uint32_t fmtLen = 16;      memcpy(h + 16, &fmtLen, 4);
  uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  uint32_t sr = REC_SR, byteRate = REC_BYTES_SEC;
  memcpy(h + 20, &pcm, 2);   memcpy(h + 22, &ch, 2);    memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byteRate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataLen, 4);

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    recSetMsg("WRITE FAIL");
    Serial.printf("[REC] open %s failed\n", path);
    return 0;
  }
  bool written = f.write(h, sizeof(h)) == sizeof(h);
  // 分块写：一次 1.9MB 的写会让主循环卡住好几秒（刷屏/触摸都停）
  const int CH = 4096;
  for (int i = 0; written && i < frames; i += CH) {
    size_t bytes = min(frames - i, CH) * sizeof(int16_t);
    written = f.write((const uint8_t *)(gMicBuf + i), bytes) == bytes;
  }
  f.close();
  if (!written) {
    SD_MMC.remove(path);
    recSetMsg("录音写入失败");
    Serial.printf("[REC] incomplete write %s\n", path);
    return 0;
  }
  int secs = frames / REC_SR;
  Serial.printf("[REC] saved %s (%u bytes, %d s)\n", path, (unsigned)(44 + dataLen), secs);
  recScan();                     // 重新枚举，最新一条排到 [0]
  gRecSel = 0;
  {
    char m[24]; snprintf(m, sizeof(m), "已存 %d 秒", secs);
    recSetMsg(m);
  }
  return secs;
}

// 读第 idx 段录音的 PCM（跳过 44 字节头）到 PSRAM，给 ASR 用。返回帧数，失败返回 nullptr
static int16_t *recLoadPcm(int idx, int *framesOut) {
  *framesOut = 0;
  char path[32];
  recPathOf(idx, path, sizeof(path));
  if (!path[0]) return nullptr;
  File f = SD_MMC.open(path);
  if (!f) { Serial.printf("[REC] open %s failed\n", path); return nullptr; }
  uint8_t header[44];
  uint32_t dataLen = 0, sampleRate = 0;
  if (f.read(header, sizeof(header)) == sizeof(header)) {
    memcpy(&dataLen, header + 40, sizeof(dataLen));
    memcpy(&sampleRate, header + 24, sizeof(sampleRate));
  }
  if (dataLen == 0 || dataLen % 2 || dataLen > MIC_MAX_SAMPLES * sizeof(int16_t) ||
      dataLen > f.size() - sizeof(header) || sampleRate != REC_SR ||
      memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVEfmt ", 8) ||
      memcmp(header + 36, "data", 4) || header[20] != 1 || header[21] != 0 ||
      header[22] != 1 || header[23] != 0 || header[34] != 16 || header[35] != 0) {
    Serial.printf("[REC] unsupported or incomplete WAV %s\n", path);
    return nullptr;
  }
  int16_t *pcm = (int16_t *)ps_malloc(dataLen);
  if (!pcm) { Serial.println("[REC] pcm alloc failed"); return nullptr; }
  size_t got = f.read((uint8_t *)pcm, dataLen);
  if (got != dataLen) { free(pcm); return nullptr; }
  *framesOut = dataLen / sizeof(*pcm);
  Serial.printf("[REC] loaded %s: %d frames (%.1f s)\n", path, *framesOut, *framesOut / 16000.0f);
  return pcm;
}

// ---------------- 播放 / 停止 ----------------
static bool recIsPlaying() {
  if (!gRecPlaying) return false;
  if (gAudioState == A_PLAYING || gAudioState == A_CONNECTING) return true;
  // 命令刚塞进队列、audioTask 还没处理完时 gAudioState 仍是 IDLE —— 这时绝不能
  // 把 gRecPlaying 清掉，否则按一下回放键界面立刻又变回"回放"，看着像没反应。
  if (millis() - gRecPlayStartMs < 1500) return true;
  gRecPlaying = false;                  // 播完/出错自动落回
  return false;
}

static void recPlaySel() {
  if (gRecN == 0) { showToast("还没有录音"); return; }
  if (gAiState == AVS_RECORDING) { showToast("先停止录音"); return; }
  char path[32];
  recPathOf(gRecSel, path, sizeof(path));
  audSend(AQ_PLAYFILE, 0, 0, path);
  gRecPlaying = true;
  gRecPlayStartMs = millis();
  gRecMsg[0] = 0;
  Serial.printf("[REC] play %d/%d %s\n", gRecSel + 1, gRecN, path);
}

static void recStopPlay() {
  if (!recIsPlaying()) return;
  audSend(AQ_STOP);
  gRecPlaying = false;
  Serial.println("[REC] stop playback");
}

// 回放键：播 <-> 停
static void recTogglePlay() {
  if (recIsPlaying()) recStopPlay();
  else recPlaySel();
}

// ---------------- 录音 / 停止 ----------------
// 与 aiVoiceToggle 的区别：**不自动转写**。停 = micStop + 写卡 + 回列表。
static void recToggleRecord() {
  if (gAiState == AVS_PROCESSING) return;
  if (gAiState == AVS_RECORDING) {
    if (!gAiRecToFile) { showToast("请先结束语音录音"); render(); return; }
    micStop();
    int frames = gMicN.load();
    gAiState = AVS_IDLE;
    if (frames < 3200) {                 // 不足 0.2 秒当误触，别往卡里写垃圾
      recSetMsg(frames ? "太短 没存" : "麦克风无采样");
      render(); return;
    }
    recSaveFromMic(frames);
    render(); return;
  }
  // 开始：先停回放（同一组 I2S0，边放边录就是啸叫），再开麦
  recStopPlay();
  gAiTranscript = ""; gAiResult = "";    // 新的一段清掉上一段的纪要
  gAiMode = AVM_MEETING;
  gAiRecToFile = true;                                // 停 = 存卡，不自动转写
  gAiRecLimitMs = (uint32_t)(MIC_MAX_SAMPLES / 16);   // 60 秒自动停
  if (!micBegin()) { showToast("麦克风初始化失败"); return; }
  gRecMsg[0] = 0;
  gAiRecStartMs = millis();
  gAiState = AVS_RECORDING;
  render();
}

// 纪要（可选）：把选中的 WAV 读回 PCM → 后台 ASR + Qwen
static void recMakeMinutes() {
  if (gAiState == AVS_PROCESSING) { showToast("正在生成"); return; }
  if (gAiState == AVS_RECORDING) { showToast("先停止录音"); return; }
  if (gRecN == 0) { showToast("还没有录音"); return; }
  if (!tfEnsure()) { showToast("NO TF CARD"); return; }
  int frames = 0;
  int16_t *pcm = recLoadPcm(gRecSel, &frames);
  if (!pcm || frames < 1600) {
    if (pcm) free(pcm);
    showToast("读取失败");
    return;
  }
  gAiTranscript = ""; gAiResult = "";
  if (!aiQueueVoice(AVM_MEETING, frames, pcm)) {
    showToast("内存不足");
    return;
  }
  Serial.printf("[REC] minutes for %d/%d (%d frames)\n", gRecSel + 1, gRecN, frames);
}

// ---------------- 串口验收 ----------------
static void recCmd(const String &cmd) {
  if (cmd == "REC" || cmd == "RECLIST") {
    recScan();
    Serial.printf("[REC] %s, %d file(s):\n",
                  tfEnsure() ? "TF OK" : "NO TF", gRecN);
    for (int i = 0; i < gRecN; i++)
      Serial.printf("  %d) %s  %u bytes  %u s%s\n", i + 1, gRec[i].name,
                    (unsigned)gRec[i].bytes, (unsigned)gRec[i].secs, i == gRecSel ? "  *" : "");
    return;
  }
  if (cmd.startsWith("RECREC")) {              // RECREC [秒]：无手指录一段并落盘
    int secs = cmd.length() > 6 ? cmd.substring(6).toInt() : 5;
    if (secs < 1) secs = 5;
    if (secs > 60) secs = 60;
    if (!micBegin()) { Serial.println("[REC] mic begin failed"); return; }
    delay(secs * 1000);
    micStop();
    int peak, rms; micLevel(peak, rms);
    Serial.printf("[REC] captured %d samples peak=%d rms=%d\n", gMicN.load(), peak, rms);
    recSaveFromMic(gMicN.load());
    return;
  }
  if (cmd.startsWith("RECPLAY")) {             // RECPLAY [序号，1 起]
    recScan();
    int i = cmd.length() > 7 ? cmd.substring(7).toInt() - 1 : 0;
    if (i < 0) i = 0;
    gRecSel = i;
    recPlaySel();
    return;
  }
  if (cmd == "RECSTOP") { audSend(AQ_STOP); gRecPlaying = false; Serial.println("[REC] stopped"); return; }
  if (cmd.startsWith("RECMIN")) {              // RECMIN [序号]：跑纪要
    recScan();
    int i = cmd.length() > 6 ? cmd.substring(6).toInt() - 1 : 0;
    if (i < 0) i = 0;
    gRecSel = i;
    recMakeMinutes();
    return;
  }
  if (cmd.startsWith("RECDEL")) {              // RECDEL [序号]：删一段
    recScan();
    int i = cmd.length() > 6 ? cmd.substring(6).toInt() - 1 : 0;
    if (i < 0 || i >= gRecN) { Serial.println("[REC] bad index"); return; }
    char path[32]; recPathOf(i, path, sizeof(path));
    Serial.printf("[REC] remove %s -> %d\n", path, (int)SD_MMC.remove(path));
    recScan();
    return;
  }
}

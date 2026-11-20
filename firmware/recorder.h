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
#include <memory>

static void showToast(const char *s);   // 定义在 main.cpp（本头文件先于它展开）

#define REC_DIR        "/REC"
#define REC_MAX        24            // 列表最多列 24 段（UI 一页 4 条，够翻）
#define REC_SR         16000
static constexpr size_t REC_PATH_SIZE = 72;
#define REC_BYTES_SEC  (REC_SR * 2)  // 16bit mono = 32000 B/s

struct RecItem {
  char     name[64];                 // REC0003.WAV（只存文件名）
  uint32_t number;
  uint32_t bytes;
  uint32_t secs;
  bool     minutes;                  // 同名 .TXT 纪要是否存在（v2.9）
};

static RecItem gRec[REC_MAX];
static int  gRecN        = 0;        // 有效条数（新的在前）
static int  gRecSel      = 0;        // 当前选中下标
static bool gRecListOpen = false;    // 录音列表子页
static bool gMinuteOpen  = false;    // 纪要阅读子页（v2.9）
static int  gMinutePage  = 0;
static String gMinuteText;           // 当前阅读的纪要全文（主循环里用，最大几十 KB 无压力）
static uint32_t gRecConfirmUntil = 0; // 删除二次确认窗口（点一次变"确认"，再点才删）
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

// 编号只接受本机格式，列表也保留能完整打开的其他 WAV。
static uint32_t recNumber(const char *name) {
  if (strncasecmp(name, "REC", 3) != 0) return 0;
  const char *p = name + 3;
  uint32_t number = 0;
  while (*p >= '0' && *p <= '9') {
    const unsigned digit = *p++ - '0';
    if (number > (UINT32_MAX - digit) / 10) return 0;
    number = number * 10 + digit;
  }
  return p - (name + 3) >= 4 && strcasecmp(p, ".WAV") == 0 ? number : 0;
}

static uint32_t recScan() {
  gRecN = 0;
  uint32_t maxNumber = 0;
  if (!tfEnsure()) { gRecPlaying = false; gRecSel = 0; return 0; }
  File dir = SD_MMC.open(REC_DIR);
  if (!dir || !dir.isDirectory()) { gRecSel = 0; return 0; }

  for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    const char *name = file.name();
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    const size_t len = strlen(name);
    if (len < 5 || strcasecmp(name + len - 4, ".WAV") != 0) continue;

    const uint32_t number = recNumber(name);
    maxNumber = max(maxNumber, number);  // 每个 WAV 都纳入编号，显示窗口只保留最新的 24 条。
    if (len >= sizeof(gRec[0].name)) continue;
    int pos = 0;
    while (pos < gRecN && (gRec[pos].number > number ||
           (gRec[pos].number == number && strcasecmp(gRec[pos].name, name) > 0))) ++pos;
    if (pos >= REC_MAX) continue;
    if (gRecN < REC_MAX) ++gRecN;
    for (int i = gRecN - 1; i > pos; --i) gRec[i] = gRec[i - 1];
    RecItem &item = gRec[pos];
    strlcpy(item.name, name, sizeof(item.name));
    item.number = number;
    item.bytes = file.size();
    item.secs = item.bytes > 44 ? (item.bytes - 44) / REC_BYTES_SEC : 0;
    char mp[REC_PATH_SIZE];
    snprintf(mp, sizeof(mp), REC_DIR "/%.*s.TXT", (int)(len - 4), name);
    item.minutes = SD_MMC.exists(mp);
  }
  if (gRecSel >= gRecN) { gRecSel = 0; gMinuteOpen = false; gMinuteText = ""; }
  return maxNumber;
}

static const char *recPathOf(int idx, char *buf, size_t cap) {
  if (idx < 0 || idx >= gRecN) { buf[0] = 0; return buf; }
  snprintf(buf, cap, REC_DIR "/%s", gRec[idx].name);
  return buf;
}

// ---------------- 纪要（v2.9）：与 WAV 同编号的 /REC/RECxxxx.TXT ----------------
// 纯 UTF-8 文本，生成成功时由 cloudApplyJobs() 在主循环写卡；阅读/删除都走这里。
static char *recMinutesPath(const char *wavPath, char *buf, size_t cap) {
  if (!wavPath || !*wavPath) { buf[0] = 0; return buf; }
  snprintf(buf, cap, "%s", wavPath);
  size_t n = strlen(buf);
  if (n >= 4 && strcasecmp(buf + n - 4, ".wav") == 0) n -= 4;
  if (n + 5 > cap) { buf[0] = 0; return buf; }
  strcpy(buf + n, ".TXT");
  return buf;
}

static const char *recMinutesPathOf(int idx, char *buf, size_t cap) {
  if (idx < 0 || idx >= gRecN) { buf[0] = 0; return buf; }
  char wav[REC_PATH_SIZE];
  recPathOf(idx, wav, sizeof(wav));
  return recMinutesPath(wav, buf, cap);
}

static bool recMinutesSave(const char *wavPath, const String &result) {
  char path[REC_PATH_SIZE];
  recMinutesPath(wavPath, path, sizeof(path));
  if (!path[0] || !result.length() || !tfEnsure()) return false;
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) { Serial.printf("[REC] minutes write failed: %s\n", path); return false; }
  size_t written = f.print(result);
  f.close();
  const char *slash = strrchr(wavPath, '/');
  const char *base = slash ? slash + 1 : wavPath;
  for (int i = 0; i < gRecN; i++)
    if (!strcasecmp(gRec[i].name, base)) { gRec[i].minutes = true; break; }
  Serial.printf("[REC] minutes saved %s (%u chars)\n", path, (unsigned)written);
  return true;
}

static bool recMinutesLoad(int idx, String &out) {
  char path[REC_PATH_SIZE];
  recMinutesPathOf(idx, path, sizeof(path));
  out = "";
  if (!path[0] || !tfEnsure()) return false;
  File f = SD_MMC.open(path);
  if (!f || f.size() > 16384) { if (f) f.close(); return false; }
  out = f.readString();
  f.close();
  return out.length() > 0;
}

static bool recMinutesRemove(int idx) {
  char path[REC_PATH_SIZE];
  recMinutesPathOf(idx, path, sizeof(path));
  if (!path[0] || !SD_MMC.exists(path)) return false;
  bool ok = SD_MMC.remove(path);
  if (ok && idx >= 0 && idx < gRecN) gRec[idx].minutes = false;
  return ok;
}

// 删录音同时删同名纪要；失败也重新扫一次，让 UI 与卡保持一致
static bool recRemoveSel() {
  char wav[REC_PATH_SIZE], txt[REC_PATH_SIZE];
  recPathOf(gRecSel, wav, sizeof(wav));
  recMinutesPath(wav, txt, sizeof(txt));
  if (!wav[0]) return false;
  bool ok = true;
  if (SD_MMC.exists(wav)) ok = SD_MMC.remove(wav);
  if (SD_MMC.exists(txt)) SD_MMC.remove(txt);
  recScan();
  return ok;
}

struct RecWriteJob {
  TfTransfer card;
  File file;
  int16_t *pcm = nullptr;
  int frames = 0, offset = 0;
  uint32_t number = 0;
  char path[REC_PATH_SIZE], temporary[REC_PATH_SIZE];
  ~RecWriteJob() { file.close(); free(pcm); }
};
static RecWriteJob *gRecWrite = nullptr;

// 先建立文件，再把采音缓冲交给主循环分批写入。
static bool recSaveFromMic(int frames) {
  gRecMsg[0] = 0;
  if (gRecWrite || !gMicBuf || frames <= 0) return false;
  std::unique_ptr<RecWriteJob> job(new (std::nothrow) RecWriteJob);
  if (!job) { recSetMsg("内存不足"); return false; }
  if (!job->card) { recSetMsg("NO TF CARD"); return false; }
  if (!SD_MMC.exists(REC_DIR) && !SD_MMC.mkdir(REC_DIR)) {
    recSetMsg("录音目录创建失败");
    return false;
  }

  job->number = recScan();
  do {
    if (job->number == UINT32_MAX) { recSetMsg("录音编号已满"); return false; }
    ++job->number;
    snprintf(job->path, sizeof(job->path), REC_DIR "/REC%04lu.WAV", (unsigned long)job->number);
    snprintf(job->temporary, sizeof(job->temporary), REC_DIR "/REC%04lu.TMP", (unsigned long)job->number);
  } while (SD_MMC.exists(job->path) || SD_MMC.exists(job->temporary));

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

  job->file = SD_MMC.open(job->temporary, FILE_WRITE);
  if (!job->file) { recSetMsg("录音写入失败"); return false; }
  if (job->file.write(h, sizeof(h)) != sizeof(h)) {
    job->file.close();
    SD_MMC.remove(job->temporary);
    recSetMsg("录音写入失败");
    return false;
  }
  job->frames = frames;
  job->pcm = micTakeBuffer();
  gRecWrite = job.release();
  recSetMsg("保存中");
  return true;
}

static void recPollSave() {
  if (!gRecWrite) return;
  RecWriteJob &job = *gRecWrite;
  const int frames = min(job.frames - job.offset, 4096);
  const size_t bytes = frames * sizeof(int16_t);
  const bool written = job.file.write((const uint8_t *)(job.pcm + job.offset), bytes) == bytes;
  job.offset += frames;
  if (written && job.offset < job.frames) return;

  job.file.close();
  if (!written) {
    SD_MMC.remove(job.temporary);
    recSetMsg("录音写入失败");
    Serial.printf("[REC] incomplete write %s\n", job.temporary);
  } else if (SD_MMC.exists(job.path) || !SD_MMC.rename(job.temporary, job.path)) {
    recSetMsg("录音保存失败");
    Serial.printf("[REC] rename failed, PCM kept in %s\n", job.temporary);
  } else {
    const uint32_t totalBytes = 44 + job.frames * sizeof(int16_t);
    const int secs = job.frames / REC_SR;
    if (gRecN < REC_MAX) ++gRecN;
    for (int i = gRecN - 1; i > 0; --i) gRec[i] = gRec[i - 1];
    RecItem &item = gRec[0];
    strlcpy(item.name, job.path + sizeof(REC_DIR), sizeof(item.name));
    item.number = job.number;
    item.bytes = totalBytes;
    item.secs = secs;
    item.minutes = false;            // 新录音尚无纪要
    gRecSel = 0;
    char message[24];
    snprintf(message, sizeof(message), "已存 %d 秒", secs);
    recSetMsg(message);
    Serial.printf("[REC] saved %s (%u bytes, %d s)\n", job.path, (unsigned)totalBytes, secs);
  }
  delete gRecWrite;
  gRecWrite = nullptr;
}

// 打开本机录音并验证 PCM 头，文件停在 PCM 数据起点。
static bool recOpenPcm(const char *path, File &f, int &frames) {
  f = SD_MMC.open(path);
  if (!f) { Serial.printf("[REC] open %s failed\n", path); return false; }
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
    return false;
  }
  frames = dataLen / sizeof(int16_t);
  return frames >= 1600;
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
  char path[REC_PATH_SIZE];
  recPathOf(gRecSel, path, sizeof(path));
  if (!path[0] || !audSend(AQ_PLAYFILE, 0, 0, path)) {
    showToast("播放请求失败");
    return;
  }
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
static void recToggleRecord(uint32_t limitMs) {
  if (gRecWrite) { showToast("录音保存中"); render(); return; }
  if (gAiState == AVS_PROCESSING) return;
  if (gAiState == AVS_RECORDING) {
    if (!gAiRecToFile) { showToast("请先结束语音录音"); render(); return; }
    micStop();
    int frames = gMicN.load();
    int peak, rms;
    micLevel(peak, rms);
    Serial.printf("[REC] captured %d samples peak=%d rms=%d\n", frames, peak, rms);
    gAiState = AVS_IDLE;
    if (frames < 3200) {                 // 不足 0.2 秒当误触，别往卡里写垃圾
      micReleaseBuffer();
      recSetMsg(frames ? "太短 没存" : "麦克风无采样");
      render(); return;
    }
    recSaveFromMic(frames);
    micReleaseBuffer();
    render(); return;
  }
  // 开始：先停回放（同一组 I2S0，边放边录就是啸叫），再开麦
  recStopPlay();
  gAiTranscript = ""; gAiResult = "";    // 新的一段清掉上一段的纪要
  gAiMode = AVM_MEETING;
  gAiRecToFile = true;                                // 停 = 存卡，不自动转写
  gAiRecLimitMs = limitMs;
  if (!micBegin(30, limitMs * 16U)) {
    micReleaseBuffer();
    showToast("麦克风初始化失败");
    return;
  }
  gRecMsg[0] = 0;
  gAiRecStartMs = millis();
  gAiState = AVS_RECORDING;
  render();
}

// 纪要（可选）：把选中的 WAV 读回 PCM → 后台 ASR + Qwen
static void recMakeMinutes() {
  if (gRecWrite || gAiState == AVS_PROCESSING) { showToast("任务进行中"); return; }
  if (gAiState == AVS_RECORDING) { showToast("先停止录音"); return; }
  if (gRecN == 0) { showToast("还没有录音"); return; }
  if (!tfEnsure()) { showToast("NO TF CARD"); return; }
  char path[REC_PATH_SIZE];
  recPathOf(gRecSel, path, sizeof(path));
  if (!path[0]) { showToast("读取失败"); return; }
  gAiTranscript = ""; gAiResult = "";
  if (!aiQueueVoice(AVM_MEETING, 0, nullptr, "", path)) {
    showToast("任务启动失败");
    return;
  }
  Serial.printf("[REC] minutes for %s\n", path);
}

// ---------------- 串口验收 ----------------
static void recCmd(const String &cmd) {
  if (cmd == "REC" || cmd == "RECLIST") {
    recScan();
    Serial.printf("[REC] %s, %d file(s):\n",
                  tfEnsure() ? "TF OK" : "NO TF", gRecN);
    for (int i = 0; i < gRecN; i++)
      Serial.printf("  %d) %s  %u bytes  %u s%s%s\n", i + 1, gRec[i].name,
                    (unsigned)gRec[i].bytes, (unsigned)gRec[i].secs,
                    gRec[i].minutes ? "  [M]" : "", i == gRecSel ? "  *" : "");
    return;
  }
  if (cmd.startsWith("RECREC")) {              // RECREC [秒]：无手指录一段并落盘
    int secs = cmd.length() > 6 ? cmd.substring(6).toInt() : 5;
    if (secs < 1) secs = 5;
    if (secs > 60) secs = 60;
    if (gAiState == AVS_RECORDING || gAiState == AVS_PROCESSING || gRecWrite) {
      Serial.println("[REC] busy");
      return;
    }
    recToggleRecord(secs * 1000UL);
    return;
  }
  if (cmd.startsWith("RECPLAY")) {             // RECPLAY [序号，1 起]
    recScan();
    int i = cmd.length() > 7 ? cmd.substring(7).toInt() - 1 : 0;
    if (i < 0 || i >= gRecN) { Serial.println("[REC] bad index"); return; }
    gRecSel = i;
    recPlaySel();
    return;
  }
  if (cmd == "RECSTOP") { audSend(AQ_STOP); gRecPlaying = false; Serial.println("[REC] stopped"); return; }
  if (cmd.startsWith("RECMIN")) {              // RECMIN [序号]：跑纪要
    recScan();
    int i = cmd.length() > 6 ? cmd.substring(6).toInt() - 1 : 0;
    if (i < 0 || i >= gRecN) { Serial.println("[REC] bad index"); return; }
    gRecSel = i;
    recMakeMinutes();
    return;
  }
  if (cmd.startsWith("RECDEL")) {              // RECDEL [序号]：删一段（含同名纪要）
    recScan();
    int i = cmd.length() > 6 ? cmd.substring(6).toInt() - 1 : 0;
    if (i < 0 || i >= gRecN) { Serial.println("[REC] bad index"); return; }
    if (recIsPlaying() || gRecWrite || gAiState == AVS_PROCESSING) { Serial.println("[REC] busy"); return; }
    gRecSel = i;
    Serial.printf("[REC] remove #%d -> %d\n", i + 1, (int)recRemoveSel());
    return;
  }
  if (cmd.startsWith("RECMREAD")) {            // RECMREAD [序号]：从 TF 卡读纪要并打开阅读页
    recScan();
    int i = gRecSel;
    if (cmd.length() > 8) { int v = cmd.substring(8).toInt(); if (v >= 1) i = v - 1; }
    if (i < 0 || i >= gRecN) { Serial.println("[REC] bad index"); return; }
    if (recMinutesLoad(i, gMinuteText)) {
      gRecSel = i; gMinutePage = 0; gRecConfirmUntil = 0;
      gMinuteOpen = true; gRecListOpen = false;
      Serial.printf("[REC] minutes read %d chars\n", gMinuteText.length());
    } else Serial.println("[REC] no minutes for this file");
    return;
  }
  if (cmd.startsWith("RECMDEL")) {             // RECMDEL [序号]：只删纪要，保留录音
    recScan();
    int i = gRecSel;
    if (cmd.length() > 7) { int v = cmd.substring(7).toInt(); if (v >= 1) i = v - 1; }
    if (i < 0 || i >= gRecN) { Serial.println("[REC] bad index"); return; }
    Serial.printf("[REC] remove minutes %d -> %d\n", i + 1, (int)recMinutesRemove(i));
    return;
  }
  if (cmd == "RECMFAKE") {                     // 无网络时写入示例纪要，验收阅读/翻页/删除 UI
    recScan();
    if (!gRecN) { Serial.println("[REC] record first"); return; }
    String demo;
    for (int i = 1; i <= 4; i++) {
      demo += "第" + String(i) + "段：本地验收示例，未调用云端。\n";
      demo += "行动项：核对录音编号与时长，检查识别摘要，验证翻页与删除。\n";
    }
    char wav[REC_PATH_SIZE]; recPathOf(gRecSel, wav, sizeof(wav));
    Serial.printf("[REC] fake minutes -> %d\n", (int)recMinutesSave(wav, demo));
    return;
  }
}

#pragma once
// ============================================================================
//  音频引擎 v0.14：电台 / TF 卡 / 播客 三合一
//  硬件（微雪 ESP32-S3-Touch-LCD-1.85B 官方引脚表）：
//    ES8311   I2C SDA=11 SCL=10；I2S MCLK=2 BCLK=48 LRCK=38 DOUT=47 DIN=39
//    PA        GPIO9 高有效
//    microSD  SDMMC 4-bit：CLK=15 CMD=14 D0=16 D1=17 D2=12 D3=13
//
//  【v0.14 修复：有点声音马上就没有了】六个叠加原因（1~3 是静音，4~6 是崩溃/死锁）
//  1. netTask 原来也钉在 Core0，和库内部的解码任务(优先级2)、我们的喂流任务(优先级1)
//     挤在同一个核上。天气/航班的 HTTPS 长请求会抢 lwIP/TLS 的共享锁，网络流的
//     client->read() 被卡住，InBuffer 被解码器抽空后没有新数据 -> 静音，
//     但 m_f_running 仍为 true，STATUS 看起来仍是 PLAYING（极具迷惑性）。
//     现在 netTask 移到 Core1，Core0 专供音频。
//  2. 喂流任务原来 delay(10) 且优先级最低；改成优先级 3 / 2ms，并且任何时候只要
//     状态不是 IDLE/ERROR 就持续调用 gAudio->loop()（旧版一旦 isRunning() 偶发
//     变 false 就切 IDLE 并彻底停止喂流，声音再也不会回来）。
//  3. 增加断流看护：InBuffer 15s 无变化或 isRunning() 变 false 时，电台自动重连
//     （退避重试 6 次），本地文件/播客自动播下一首，绝不静默停住。
//  4. Audio 对象原先在 setup()（Core1 的 loopTask）里 new，I2S/GDMA 中断就注册在
//     Core1，播放时抢占同样在 Core1 的 QSPI 刷屏，spi_master.c:setup_priv_desc
//     处 StoreProhibited。现在 audioTask() 在 Core0 上 new Audio()。
//  5. 喂流任务优先级一度提到 3，压过库内解码任务(2)，playChunk() 里
//     i2s_channel_write(50ms) 反复超时，库把 m_validSamples 永久卡住 -> 只出几声。
//     现在喂流 1 < 解码 2；并给外部库 Audio.cpp 打补丁（见 patches/）。
//  6. audStart() 里曾多做一次 setPinout() 试图「重启 I2S」，反而让
//     zeroI2Sbuff()->i2s_channel_preload_data() 写空 DMA 描述符 -> Core0 panic。
//     现在只在 audioTask() 初始化时 setPinout() 一次。
//  另外把 HTTPS socket 超时从 2s 放宽到 8s（setConnectionTimeout 同时作用于 read，
//  直播流偶尔的 chunk 间隔 >2s 会被误判断连）。
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <FFat.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <string.h>
#include "Audio.h"
#include "es8311.h"

extern bool wifiOK;

// ------------------------------------------------------------------ 回调日志
static uint32_t gLastAudioLog = 0;
// 库的弱符号回调：每次 playChunk() 准备写 I2S 前都会调它。
// gI2sChunks 是否增长 = 解码任务有没有真的推进到输出阶段（客观探针）。
static volatile uint32_t gI2sChunks = 0;
// 由 ESP32-audioI2S 的 Audio.cpp（RoundClock patch 3/3）定义，链接期解析
extern volatile uint32_t gI2sDiag[10];
void audio_process_i2s(int16_t *outBuff, int32_t validSamples, bool *continueI2S) {
  (void)outBuff; (void)validSamples;
  *continueI2S = true;               // true = 继续走 i2s_channel_write
  gI2sChunks++;
}
void audio_info(const char *info) {
  if (!info) return;
  bool key = strstr(info, "MP3") || strstr(info, "WAV") || strstr(info, "SampleRate") ||
             strstr(info, "Bitrate") || strstr(info, "Channels") || strstr(info, "Error") ||
             strstr(info, "failed") || strstr(info, "slow stream") || strstr(info, "i2s") ||
             strstr(info, "I2S") || strstr(info, "timeout") || strstr(info, "stream ready") ||
             strstr(info, "Stream lost") || strstr(info, "redirect") ||
             strstr(info, "End of") || strstr(info, "PSRAM");
  uint32_t now = millis();
  if (!key && now - gLastAudioLog < 1500) return;   // 限流，别把 USB CDC 打爆
  gLastAudioLog = now;
  Serial.printf("[AUD.i] %s\n", info);
}
void audio_showstation(const char *info) { Serial.printf("[AUD.i] station: %s\n", info); }
void audio_bitrate(const char *info)     { Serial.printf("[AUD.i] bitrate: %s\n", info); }
void audio_eof_mp3(const char *info)     { Serial.printf("[AUD.i] eof: %s\n", info); }
void audio_eof_stream(const char *info)  { Serial.printf("[AUD.i] stream eof: %s\n", info); }

#define I2S_BCLK_PIN   48
#define I2S_LRCK_PIN   38
#define I2S_DOUT_PIN   47
#define I2S_MCLK_PIN    2
#define PA_EN_PIN       9
#define AUDIO_SR    44100

// ------------------------------------------------------------------ 音源模型
enum AudSrc : uint8_t { AS_RADIO = 0, AS_TF = 1, AS_POD = 2, AS_COUNT = 3 };
// v0.16：TF 卡栏目按用户要求改名 MUSIC；中文栏目名在 UI 的 tab 上单独用 16px 字库绘制
static const char *const AUD_SRC_NAME[] = {"RADIO", "MUSIC", "PODCAST"};
#define AUD_MAX_TRACKS 20
#define AUD_NAME_LEN   64       // v0.16 40->64：中文标题是 3 字节/字，40 字节只能装 13 个字
#define AUD_URL_LEN   384
#define AUD_FEED_LEN  256

// ------------------------------------------------------------------ 中文播客（小宇宙）
// 小宇宙官方不提供公开 RSS，但节目页可经 RSSHub 路由导出标准 RSS（含 enclosure 音频直链），
// 也有节目自托管的 feed.xyzfm.space。三条均已 PC 实测 200：
// 小宇宙中文播客（v0.16b 扩到 7 档）。两种 feed 来源，PC 2026-11-15 全部实测 HTTP 200：
//   ① 小宇宙官方 RSS 托管 feed.xyzfm.space（李诞、半拿铁周刊）—— 最稳，无第三方依赖；
//   ② RSSHub /xiaoyuzhou/podcast/<id>（无聊斋、谐星聊天会、忽左忽右、半拿铁）——
//      bestblogs.dev 公共实例实测 200/8s，enclosure 已是 media.xyzcdn.net/*.m4a 直链。
// 前 130KB 即可拿满每档 15~34 集，m4a 由 ESP32-audioI2S 3.4.0 CODEC_M4A 流式解码。
// 李诞官方 feed 的 enclosure 带 dts-api.xiaoyuzhoufm.com 跟踪前缀，由 podNormalizeUrl() 剥成直链。
struct PodFeed { const char *name; const char *url; };
static const PodFeed POD_FEEDS[] = {
  // 用户点名的四档排最前，FEED 按钮能最快轮到
  {"无聊斋",             "https://rsshub.bestblogs.dev/xiaoyuzhou/podcast/5e280fac418a84a0461fb129"},
  {"谐星聊天会",         "https://rsshub.bestblogs.dev/xiaoyuzhou/podcast/5e280fa7418a84a0461f912b"},
  {"李诞",               "https://feed.xyzfm.space/l3c8em3l8hfn"},
  {"忽左忽右",           "https://rsshub.bestblogs.dev/xiaoyuzhou/podcast/5e4ee557418a84a0466737b7"},
  // 原有两档保留
  {"半拿铁 / 商业沉浮录", "https://rsshub.bestblogs.dev/xiaoyuzhou/podcast/62382c1103bea1ebfffa1c00"},
  {"半拿铁周刊",         "https://feed.xyzfm.space/vylham8uw3ay"},
};
#define POD_FEED_COUNT 6
static int gPodFeedIdx = 0;
#define AUD_DEFAULT_FEED (POD_FEEDS[0].url)

struct AudTrack { char name[AUD_NAME_LEN]; char url[AUD_URL_LEN]; };

// 12 条直链均经 PC 实测 200 / audio/mpeg（2026-11-15）。
// 前 6 条走国内蜻蜓 CDN，连接快；后 6 条是国际台，TLS 握手慢，排在后面作备用。
// 注意：SomaFM 会按 User-Agent 拒流，库用的是 VLC/3.0.21 UA，实测可通。
static const char *const STREAM_URLS[] = {
  "https://lhttp.qtfm.cn/live/15318317/64k.mp3",
  "https://lhttp.qtfm.cn/live/4915/64k.mp3",
  "https://lhttp.qtfm.cn/live/4804/64k.mp3",
  "https://lhttp.qtfm.cn/live/1301/64k.mp3",
  "https://lhttp.qtfm.cn/live/1136/64k.mp3",
  "https://lhttp.qtfm.cn/live/1122/64k.mp3",
  "https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3",
  "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service",
  "https://icecast.omroep.nl/radio1-bb-mp3",
  "https://streams.kqed.org/kqedradio",
  "https://ice1.somafm.com/groovesalad-128-mp3",
  "https://ice1.somafm.com/dronezone-128-mp3"
};
// v0.16：中文电台名（16px 全量字库，任意汉字都能显示）。
// 前 6 条是蜻蜓 CDN 的流 ID，服务端不返回 icy-name，故按频道序号如实标注，不虚构台名。
static const char *const STREAM_LABELS[] = {
  "蜻蜓FM 频道一", "蜻蜓FM 频道二", "蜻蜓FM 频道三",
  "蜻蜓FM 频道四", "蜻蜓FM 频道五", "蜻蜓FM 频道六",
  "法国国际广播 华语", "BBC 世界广播", "荷兰 NPO 一台",
  "KQED 公共广播", "SomaFM 律动", "SomaFM 氛围"
};
#define STREAM_COUNT 12

// 兼容旧 UI 变量名：gStation 只表示「电台」下标
static int gStation = 0;
static AudTrack gRadio[AUD_MAX_TRACKS];
static int      gRadioN = STREAM_COUNT;
static AudTrack gTf[AUD_MAX_TRACKS];
static volatile int  gTfN = 0;
static bool          gTfMounted = false;
static AudTrack gPod[AUD_MAX_TRACKS];
static volatile int  gPodN = 0;
static volatile AudSrc gSrc = AS_RADIO;
static volatile int    gIdx = 0;
static volatile bool   gPodBusy = false;

// ------------------------------------------------------------------ 运行时状态
static Audio           *gAudio      = nullptr;
static es8311_handle_t  gCodec      = nullptr;
static bool             gAudioReady = false;
static bool             gConnected  = false;
enum AudioState : uint8_t { A_IDLE = 0, A_CONNECTING, A_PLAYING, A_PAUSED, A_ERROR };
static volatile AudioState gAudioState = A_IDLE;
static volatile int     gAudioVol   = 16;
static volatile int     gToneFreq   = 440;
static volatile int     gToneMs     = 1500;

// 看护 / 健康计数（STATUS 可见，用来客观判断「是不是真在出声」）
static volatile uint32_t gAudLoops    = 0;      // audioTask 累计喂 loop 次数
static volatile uint32_t gAudBuf      = 0;      // InBuffer 当前字节数
static volatile uint32_t gAudElapsed  = 0;      // 当前曲目已播秒数
static volatile uint32_t gAudDuration = 0;      // 当前曲目总秒数
static uint32_t gAudLastBuf   = 0xFFFFFFFF;
static uint32_t gAudLastI2s  = 0;       // 上一次观测到的 I2S 真实输出字节数
static uint32_t gAudStartI2s = 0;       // 本次 audStart 时的字节数，用来判断「到底响过没有」
static uint32_t gAudLastData = 0;
static uint32_t gAudStartMs  = 0;
static uint32_t gAudRetryAt  = 0;
static uint8_t  gAudRetry    = 0;
static uint32_t gAudConnectGraceMs = 15000;  // 播客重定向链长，连接宽限按音源区分
static bool     gMuteApplied = false;        // 暂停/停止时数字静音，杜绝 DMA 回放最后一帧
static volatile bool gVolDirty = false;
static bool     gToneMode     = false;  // TONE 是离线自测，播完必须停，不能当电台重连

// ------------------------------------------------------------------ 请求队列
enum : uint8_t {
  AQ_NONE = 0, AQ_PLAY, AQ_TOGGLE, AQ_NEXT, AQ_STOP, AQ_TONE, AQ_VOL,
  AQ_SOURCE, AQ_SELECT, AQ_RESCAN_TF, AQ_PODFEED, AQ_PODADD, AQ_PODCLEAR, AQ_TFTEST
};
struct AudCmd {
  uint8_t cmd; int arg; int arg2;
  char text[AUD_URL_LEN];
  char name[AUD_NAME_LEN];
};
static QueueHandle_t gAudQ = nullptr;
static bool audSend(uint8_t cmd, int arg = 0, int arg2 = 0,
                    const char *text = nullptr, const char *name = nullptr) {
  if (!gAudQ) return false;
  AudCmd c; memset(&c, 0, sizeof(c));
  c.cmd = cmd; c.arg = arg; c.arg2 = arg2;
  if (text) strncpy(c.text, text, sizeof(c.text) - 1);
  if (name) strncpy(c.name, name, sizeof(c.name) - 1);
  return xQueueSend(gAudQ, &c, 0) == pdTRUE;
}

// ------------------------------------------------------------------ 小工具
// v0.16：以前把所有非 ASCII 字节压成 '?'，中文歌名 / 播客标题全变问号。
// 现在原样保留 UTF-8，按「完整字符」逐个拷贝，绝不把 3 字节汉字从中间切断。
// 控制字符与 XML 尖括号仍过滤掉；字库未收录的码点由 drawText() 静默跳过。
static void audUtf8Name(char *dst, size_t cap, const char *src) {
  const unsigned char *p = (const unsigned char *)src;
  size_t w = 0;
  while (*p && w + 5 < cap) {
    unsigned char ch = *p;
    if (ch == '<' || ch == '>' || ch == '&' || ch == '\r' || ch == '\n' || ch == '\t') { p++; continue; }
    int n = 1;
    if      ((ch & 0xE0) == 0xC0) n = 2;
    else if ((ch & 0xF0) == 0xE0) n = 3;
    else if ((ch & 0xF8) == 0xF0) n = 4;
    if (n > 1) {   // 续字节不合法就当单字节处理，避免把乱码灌进曲目表
      for (int k = 1; k < n; k++)
        if (((unsigned char)p[k] & 0xC0) != 0x80) { n = 1; break; }
    }
    for (int k = 0; k < n; k++) dst[w++] = (char)p[k];
    p += n;
  }
  dst[w] = 0;
  while (w && (dst[w - 1] == ' ' || dst[w - 1] == '\t')) dst[--w] = 0;
  if (!w) strncpy(dst, "TRACK", cap - 1);
}
static void audUnescape(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
    else *w++ = *r++;
  }
  *w = 0;
}
// 播客直链常套着 2~3 层统计跳转：
//   https://prfx.byspotify.com/e/play.podtrac.com/npr-510289/npr.simplecastaudio.com/...
//   -> https://play.podtrac.com/npr-510289/npr.simplecastaudio.com/...
//   -> https://npr.simplecastaudio.com/...
// 每层都要重新 TLS 握手（实测 800ms~5s），播客因此「点一下要等半分钟」，
// 连接看护还会在等待期间误判断流反复重连。这里在解析 RSS 时直接把已知
// 跟踪前缀剥掉，只留真实 CDN 域名；最后那层 hash_redirect 由库自己跟。
static bool podIsTrackerHost(const char *h) {
  return strstr(h, "podtrac.com") || strstr(h, "byspotify.com") ||
         strstr(h, "swap.fm") || strstr(h, "pdst.fm") ||
         strstr(h, "podtail.com") || strstr(h, "chartable.com") ||
         strstr(h, "podscribe.com") ||
         // 小宇宙自托管 feed 把真实 CDN 地址编在跟踪 URL 的路径里：
         // https://dts-api.xiaoyuzhoufm.com/track/<pid>/<eid>/media.xyzcdn.net/<pid>/<file>.m4a
         // podNormalizeUrl() 按 '/' 分段找到 media.xyzcdn.net 就能一次还原直链。
         strstr(h, "dts-api.xiaoyuzhoufm.com");
}
// 把跟踪跳转链一次性压到真实 CDN 域名：
//   https://tracking.swap.fm/track/<id>/prfx.byspotify.com/e/play.podtrac.com/
//   npr-510289/npr.simplecastaudio.com/.../default.mp3?...
//   -> https://npr.simplecastaudio.com/.../default.mp3?...
// 做法：按 '/' 分段，找第一个「含点、且不是跟踪域名」的段作为真正主机。
// 比「去掉第一段」更稳，因为 swap.fm 有两段前缀、podtrac 有一段、byspotify 有一段。
static void podNormalizeUrl(char *url, size_t cap) {
  char out[768];
  for (int pass = 0; pass < 4; pass++) {
    const char *sep = strstr(url, "://");
    if (!sep) break;
    const char *body = sep + 3;
    const char *slash = strchr(body, '/');
    if (!slash) break;
    size_t hlen = (size_t)(slash - body);
    if (hlen >= 96) break;
    char host[96]; memcpy(host, body, hlen); host[hlen] = 0;
    if (!podIsTrackerHost(host)) break;
    const char *seg = slash + 1, *real = nullptr;
    while (seg && *seg) {
      const char *end = strchr(seg, '/');
      size_t L = end ? (size_t)(end - seg) : strlen(seg);
      if (L >= 4 && L < 96) {
        char s[96]; memcpy(s, seg, L); s[L] = 0;
        if (strchr(s, '.') && !strchr(s, '?') && !strchr(s, '=') &&
            !strchr(s, ' ') && !podIsTrackerHost(s)) { real = seg; break; }
      }
      if (!end) break;
      seg = end + 1;
    }
    if (!real) break;
    snprintf(out, sizeof(out), "https://%s", real);
    strncpy(url, out, cap - 1); url[cap - 1] = 0;
  }
}

static AudTrack *audTrackAt(AudSrc s, int i) {
  if (i < 0 || i >= AUD_MAX_TRACKS) return nullptr;
  if (s == AS_RADIO) return (i < gRadioN) ? &gRadio[i] : nullptr;
  if (s == AS_TF)    return (i < gTfN)    ? &gTf[i]    : nullptr;
  return                (i < gPodN)   ? &gPod[i]   : nullptr;
}
static int audCount(AudSrc s) {
  return s == AS_RADIO ? gRadioN : s == AS_TF ? (int)gTfN : (int)gPodN;
}

// ------------------------------------------------------------------ TF 卡
static bool tfEnsure() {
  if (gTfMounted) return true;
  // 官方 4-bit 引脚；失败再退 1-bit（只用 CLK/CMD/D0）
  SD_MMC.setPins(15, 14, 16, 17, 12, 13);
  if (SD_MMC.begin("/sdcard", false)) { gTfMounted = true; return true; }
  SD_MMC.end();
  SD_MMC.setPins(15, 14, 16);
  if (SD_MMC.begin("/sdcard", true)) {
    gTfMounted = true;
    Serial.println("[TF] mounted in 1-bit fallback");
    return true;
  }
  gTfMounted = false;
  return false;
}
static bool tfIsAudio(const String &n) {
  String l = n; l.toLowerCase();
  return l.endsWith(".mp3") || l.endsWith(".wav") || l.endsWith(".m4a") ||
         l.endsWith(".aac") || l.endsWith(".flac") || l.endsWith(".ogg");
}
static int gTfDirs = 0, gTfFiles = 0;
static bool tfSkipDir(const String &n) {
  return n.startsWith(".") || n.equalsIgnoreCase("SYSTEM VOLUME INFORMATION") ||
         n.equalsIgnoreCase("FOUND.000") || n.equalsIgnoreCase("LOST.DIR") ||
         n.equalsIgnoreCase("Android");
}
static void tfScanDir(const char *dir, int depth) {
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) return;
  gTfDirs++;
  for (;;) {
    File f = d.openNextFile();
    if (!f) break;
    String n = f.name();
    if (f.isDirectory()) {
      // 递归到第 2 层子目录（/、/music、/music/Artist），别把整张卡翻一遍
      if (depth < 2 && !tfSkipDir(n) && gTfN < AUD_MAX_TRACKS) {
        String sub = String(dir);
        if (!sub.endsWith("/")) sub += "/";
        sub += n;
        tfScanDir(sub.c_str(), depth + 1);
      }
    } else {
      gTfFiles++;
      if (tfIsAudio(n) && gTfN < AUD_MAX_TRACKS) {
        String full = String(dir);
        if (!full.endsWith("/")) full += "/";
        full += n;
        AudTrack &t = gTf[gTfN++];
        audUtf8Name(t.name, sizeof(t.name), n.c_str());
        strncpy(t.url, full.c_str(), sizeof(t.url) - 1);
        t.url[sizeof(t.url) - 1] = 0;
      }
    }
    f.close();
  }
  d.close();
}
static void tfScan() {
  gTfN = 0; gTfDirs = 0; gTfFiles = 0;
  // 热插拔换卡后强制重新枚举；调用方（AQ_RESCAN_TF / AQ_TFTEST）已先 stopSong()
  SD_MMC.end();
  gTfMounted = false;
  if (!tfEnsure()) {
    Serial.println("[TF] no card / mount failed");
    return;
  }
  tfScanDir("/", 0);                 // 递归会覆盖 /music 及其子目录，不用再单独扫
  // 按文件名排序，播放顺序稳定
  for (int i = 1; i < gTfN; i++) {
    AudTrack key = gTf[i]; int j = i - 1;
    while (j >= 0 && strcmp(gTf[j].name, key.name) > 0) { gTf[j + 1] = gTf[j]; j--; }
    gTf[j + 1] = key;
  }
  Serial.printf("[TF] card=%s %llu MB, %d dirs / %d files -> %d tracks (max %d)\n",
                SD_MMC.cardType() == CARD_NONE ? "NONE" : "OK",
                (unsigned long long)(SD_MMC.cardSize() / 1024 / 1024),
                gTfDirs, gTfFiles, (int)gTfN, AUD_MAX_TRACKS);
}

// TFTEST：往卡里写一个 2 秒 440Hz WAV，让「SD_MMC -> 解码 -> I2S」链路
// 在没有用户音频文件时也能端到端验收。文件放 /music，扫描器本来就会扫这个目录。
static bool tfMakeTestWav(const char *path = "/music/test-tone.wav") {
  if (!tfEnsure()) { Serial.println("[TF] no card / mount failed"); return false; }
  if (!SD_MMC.exists("/music")) SD_MMC.mkdir("/music");
  const uint32_t SR = 44100, ms = 2000;
  uint32_t n = SR * ms / 1000, dataLen = n * 2;
  uint8_t h[44] = {0};
  memcpy(h + 0, "RIFF", 4);  uint32_t riff = 36 + dataLen; memcpy(h + 4, &riff, 4);
  memcpy(h + 8, "WAVE", 4);  memcpy(h + 12, "fmt ", 4);
  uint32_t fmtLen = 16;      memcpy(h + 16, &fmtLen, 4);
  uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  uint32_t sr = SR, byteRate = SR * 2;
  memcpy(h + 20, &pcm, 2);   memcpy(h + 22, &ch, 2);     memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byteRate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataLen, 4);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) { Serial.printf("[TF] cannot create %s\n", path); return false; }
  f.write(h, 44);
  int16_t buf[256];
  const uint32_t fade = SR / 50;
  for (uint32_t i = 0; i < n;) {
    uint32_t k = (n - i < 256) ? (n - i) : 256;
    for (uint32_t j = 0; j < k; j++) {
      uint32_t s = i + j;
      float env = 1.0f;
      if (s < fade) env = (float)s / fade;
      else if (n - s < fade) env = (float)(n - s) / fade;
      buf[j] = (int16_t)(12000.0f * env * sinf(2.0f * PI * 440.0f * (float)s / SR));
    }
    f.write((const uint8_t *)buf, k * 2);
    i += k;
  }
  f.close();
  Serial.printf("[TF] wrote %s (%u bytes)\n", path, (unsigned)(44 + dataLen));
  return true;
}

// ------------------------------------------------------------------ 播客清单（NVS）
static void podSave() {
  String blob;
  for (int i = 0; i < gPodN; i++) { blob += gPod[i].name; blob += '|'; blob += gPod[i].url; blob += '\n'; }
  Preferences p; p.begin("rcfg", false); p.putString("podlist", blob); p.end();
}
static void podLoad() {
  Preferences p; p.begin("rcfg", true);
  String blob = p.getString("podlist", "");
  p.end();
  gPodN = 0;
  int pos = 0;
  while (pos < (int)blob.length() && gPodN < AUD_MAX_TRACKS) {
    int nl = blob.indexOf('\n', pos);
    if (nl < 0) nl = blob.length();
    String line = blob.substring(pos, nl);
    int bar = line.indexOf('|');
    if (bar > 0) {
      AudTrack &t = gPod[gPodN++];
      strncpy(t.name, line.substring(0, bar).c_str(), sizeof(t.name) - 1); t.name[sizeof(t.name) - 1] = 0;
      strncpy(t.url,  line.substring(bar + 1).c_str(), sizeof(t.url) - 1); t.url[sizeof(t.url) - 1] = 0;
      podNormalizeUrl(t.url, sizeof(t.url));   // 老版本 NVS 里存的带跟踪前缀 URL 也一并精简
    }
    pos = nl + 1;
  }
  Serial.printf("[POD] loaded %d episodes from NVS\n", (int)gPodN);
}
static void podAdd(const char *name, const char *url) {
  if (!name || !url || !*url) return;
  if (gPodN >= AUD_MAX_TRACKS) {                 // 满了就把最旧一条挤掉
    for (int i = 1; i < gPodN; i++) gPod[i - 1] = gPod[i];
    gPodN--;
  }
  AudTrack &t = gPod[gPodN++];
  audUtf8Name(t.name, sizeof(t.name), name);
  strncpy(t.url, url, sizeof(t.url) - 1); t.url[sizeof(t.url) - 1] = 0;
  audUnescape(t.url);
  podNormalizeUrl(t.url, sizeof(t.url));
  podSave();
  Serial.printf("[POD] added '%s' (%d chars url)\n", t.name, (int)strlen(t.url));
}

// 轻量 RSS 解析：只取前 128 KB（最新剧集在文件开头），不引 XML 库、不解析全文。
// 抽 <item> 里的 <title> 与 <enclosure url="...">，最多 8 条。
static int podParseXml(const char *xml) {
  const char *p = xml;
  int n = 0;
  while ((p = strstr(p, "<item")) != nullptr && n < 8) {
    const char *end = strstr(p, "</item>");
    if (!end) break;
    char title[AUD_NAME_LEN] = {0};
    const char *t = strstr(p, "<title>");
    if (t && t < end) {
      t += 7;
      if (strncmp(t, "<![CDATA[", 9) == 0) t += 9;
      const char *te = strstr(t, "</title>");
      if (te && te > t) {
        char raw[AUD_NAME_LEN * 2];
        size_t L = min((size_t)(te - t), sizeof(raw) - 1);
        memcpy(raw, t, L); raw[L] = 0;
        char *cd = strstr(raw, "]]>");
        if (cd) *cd = 0;
        char pretty[AUD_NAME_LEN];
        audUtf8Name(pretty, sizeof(pretty), raw);
        // v0.16b：先在足够大的缓冲里拼 "EPnn " 前缀，再走一遍 audUtf8Name 按完整字符边界
        // 截到 64 字节。直接 snprintf(title, 64, ...) 会按字节硬切，把末尾 3 字节汉字切断成乱码。
        char withEp[AUD_NAME_LEN * 2];
        snprintf(withEp, sizeof(withEp), "EP%02d %s", n + 1, pretty);
        audUtf8Name(title, sizeof(title), withEp);
      }
    }
    // 先用足够大的缓冲接住完整 URL（带 swap.fm / podtrac 前缀的直链常 >500 字符），
    // 规范化压短后再存进 384 字节的曲目表，避免 query 尾巴被截断导致 CDN 403。
    char url[640] = {0};
    const char *e = strstr(p, "<enclosure");
    if (e && e < end) {
      const char *u = strstr(e, "url=\"");
      if (u && u < end) {
        u += 5;
        const char *ue = strchr(u, '"');
        if (ue && ue < end) {
          size_t L = min((size_t)(ue - u), sizeof(url) - 1);
          memcpy(url, u, L); url[L] = 0;
          audUnescape(url);
          podNormalizeUrl(url, sizeof(url));   // 省掉 podtrac / byspotify 两层 TLS 跳转
        }
      }
    }
    if (url[0]) {
      if (!title[0]) snprintf(title, sizeof(title), "EP%02d", n + 1);
      strncpy(gPod[n].name, title, sizeof(gPod[n].name) - 1);
      strncpy(gPod[n].url, url, sizeof(gPod[n].url) - 1);
      n++;
    }
    p = end + 7;
  }
  return n;
}
static bool podFetchFeed(const char *feedUrl) {
  if (!wifiOK) { Serial.println("[POD] no WiFi"); return false; }
  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(15000);
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(cli, feedUrl)) { Serial.println("[POD] http.begin failed"); return false; }
  int code = http.GET();
  if (code != 200 && code != 206) {
    Serial.printf("[POD] feed HTTP %d\n", code);
    http.end();
    return false;
  }
  char *xml = (char *)ps_malloc(131072);
  if (!xml) { http.end(); Serial.println("[POD] PSRAM alloc failed"); return false; }
  uint32_t len = 0;
  int items = 0;
  WiFiClient *st = http.getStreamPtr();
  uint8_t tmp[512];
  uint32_t t0 = millis(), lastData = t0;
  // 不要拿 http.connected()、「连续 N 次空读」或 st->available() 当终止条件：
  //  - 旧版 40×10ms=400ms 太短，2MB 的 NPR feed 只读到 15KB -> 1 集
  //  - WiFiClientSecure::available() 在底层 socket 有数据但 mbedTLS 尚未解密时
  //    也会返回 0，实测 24KB / 2 集就停
  // 直接 read()（setTimeout(15000) 会阻塞等 TLS 记录）。v0.16：小宇宙 RSS 单个
  // <item> 的 shownotes 常有 15~20 KB，服务端分块之间可能静默 3~5 s，
  // 原来 3 s 静默就收尾，实测只读到 40 KB / 2 集；放宽到 5 s + 30 s 总时限。
  while (len < 130000 && millis() - t0 < 30000) {
    int got = st->read(tmp, sizeof(tmp));
    if (got > 0) {
      lastData = millis();
      memcpy(xml + len, tmp, got); len += got; xml[len] = 0;
      // 数 enclosure：够 8 条就停，避免把 2 MB 的 feed 全下完
      items = 0;
      for (char *q = xml; (q = strstr(q, "<enclosure")) != nullptr; q += 10) items++;
      if (items >= 6) break;        // 6 集够用，别为了凑 8 集把 250 KB 全下完
      continue;
    }
    if (millis() - lastData > 5000) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  http.end();
  Serial.printf("[POD] feed read %u bytes in %u ms\n", (unsigned)len, (unsigned)(millis() - t0));
  if (len < 200) { free(xml); Serial.println("[POD] feed too short"); return false; }
  gPodN = podParseXml(xml);
  free(xml);
  podSave();
  Serial.printf("[POD] feed parsed: %d bytes -> %d episodes\n", (unsigned)len, (int)gPodN);
  return gPodN > 0;
}

// 第三方 RSSHub 实例随时可能失效，三条中文播客依次试，任一成功就停。
// start 指定从哪条开始（UI 的 FEED 按钮靠它循环切换节目）。
static bool podFetchAny(int start) {
  for (int k = 0; k < POD_FEED_COUNT; k++) {
    int i = (start + k) % POD_FEED_COUNT;
    Serial.printf("[POD] feed %d/%d '%s'\n", i + 1, POD_FEED_COUNT, POD_FEEDS[i].name);
    if (podFetchFeed(POD_FEEDS[i].url)) { gPodFeedIdx = i; return true; }
  }
  Serial.println("[POD] all feeds failed");
  return false;
}

// ------------------------------------------------------------------ 前向声明
static void audioTask(void *);
static bool doTone(int freqHz = 440, int ms = 1500);
static bool audStart(int idx);
static void audStop();
static void audExec(const AudCmd &c);
static void audScheduleRetry();
static void audMonitor();
static void audNext(int dir);

// ------------------------------------------------------------------ 播放动作
static bool audStart(int idx) {
  if (!gAudioReady) return false;
  AudTrack *t = audTrackAt(gSrc, idx);
  if (!t) return false;
  gIdx = idx;
  if (gSrc == AS_RADIO) gStation = idx;
  // 播客 URL 常带 3~4 层统计重定向，每层都要重新 TLS 握手；电台是无限流，
  // 读超时太短会把正常的 chunk 间隔误判断连。按音源给不同超时与连接宽限。
  if (gSrc == AS_POD) {
    gAudio->setConnectionTimeout(15000, 15000);
    gAudConnectGraceMs = 45000;
  } else {
    gAudio->setConnectionTimeout(8000, 8000);
    gAudConnectGraceMs = 15000;
  }
  gAudio->stopSong();
  // 快速连按切台时，上一条 HTTPS socket / lwIP PCB 还没完全释放，
  // connecttohost() 会直接返回 false（实测 TRACK 8 -> TRACK 1 必现）。
  // 给 60ms 让库内部收尾，再建新连接；下面还有一次失败重试兜底。
  vTaskDelay(pdMS_TO_TICKS(60));
  // 【绝对不要在这里再调一次 setPinout()】Audio 构造时已 new_channel + init_std_mode +
  // enable，audioTask() 里那一次 setPinout() 负责把 GPIO 从 I2S_GPIO_UNUSED 改成实际引脚。
  // 第二次 setPinout() 会走 I2Sstop -> reconfig_std_gpio -> I2Sstart -> zeroI2Sbuff()
  // -> i2s_channel_preload_data()，此时 DMA 描述符已被 disable 释放，
  // ROM memcpy 向 NULL 写 2 字节 -> Core0 StoreProhibited + 整机重启。
  // 实测 backtrace: i2s_channel_preload_data -> Audio::zeroI2Sbuff(Audio.cpp:314)
  //               -> Audio::setPinout -> audStart(audio_player.h) -> audioTask。
  // 本地 WAV(TONE) 不走 audStart 所以不会触发，只有 connecttohost 第一条电台会崩。
  bool ok = false;
  if (gSrc == AS_TF) {
    if (!tfEnsure()) { Serial.println("[AUD] TF not available"); gAudioState = A_ERROR; return false; }
    ok = gAudio->connecttoFS(SD_MMC, t->url);
    gConnected = false;
    Serial.printf("[AUD] TF %d/%d '%s' -> %s\n", idx + 1, (int)gTfN, t->name, ok ? "OK" : "FAILED");
  } else {
    if (!wifiOK) { Serial.println("[AUD] no WiFi"); gAudioState = A_ERROR; return false; }
    ok = gAudio->connecttohost(t->url);
    if (!ok) {                              // socket 释放竞态：等一下再试一次
      vTaskDelay(pdMS_TO_TICKS(180));
      ok = gAudio->connecttohost(t->url);
    }
    gConnected = ok;
    Serial.printf("[AUD] %s %d/%d '%s' -> %s\n", AUD_SRC_NAME[gSrc], idx + 1,
                  audCount(gSrc), t->name, ok ? "connecting" : "FAILED");
  }
  gAudioState = ok ? A_CONNECTING : A_ERROR;
  gToneMode = false;
  gAudStartMs = millis();
  gAudLastData = millis();
  gAudLastBuf = 0xFFFFFFFF;
  gAudLastI2s = gI2sDiag[3];
  gAudStartI2s = gI2sDiag[3];
  gAudRetryAt = 0;
  return ok;
}
static void audStop() {
  if (gAudioReady) gAudio->stopSong();
  gAudioState = A_IDLE; gConnected = false;
  gAudRetryAt = 0; gAudRetry = 0;
  Serial.println("[AUD] stop");
}
static void audNext(int dir) {
  int n = audCount(gSrc);
  if (n <= 0) { Serial.println("[AUD] empty source"); return; }
  int ni = ((int)gIdx + dir + n) % n;
  gAudRetry = 0;
  audStart(ni);
}
static void audScheduleRetry() {
  gAudio->stopSong();
  gAudRetry++;
  const uint8_t limit = (gSrc == AS_POD) ? 3 : 6;
  if (gAudRetry > limit) {
    // 播客重连一次就要重走整条重定向链，反复重试只会让界面「一直卡在连接中」。
    // 三次拿不到就直接跳下一集，别把用户钉死在同一条坏链上。
    if (gSrc == AS_POD && audCount(gSrc) > 1) {
      Serial.println("[AUD] podcast stream dead -> next episode");
      audNext(1);
      return;
    }
    gAudioState = A_ERROR;
    gAudRetryAt = 0;
    Serial.printf("[AUD] stream dead after %d retries -> ERROR\n", (int)limit);
    return;
  }
  gAudioState = A_CONNECTING;
  gAudRetryAt = millis() + (gAudRetry <= 2 ? 1000 : 3000);
  Serial.printf("[AUD] reconnect scheduled #%d for '%s'\n", gAudRetry,
                AUD_SRC_NAME[gSrc]);
}

// 断流看护：isRunning() 掉下去 / InBuffer 长期无变化 都要自愈
static void audMonitor() {
  bool running = gAudio->isRunning();
  uint32_t buf = gAudio->inBufferFilled();
  uint32_t i2sBytes = gI2sDiag[3];       // 真实送到 I2S 的字节数，比 InBuffer 可靠
  gAudBuf = buf;
  gAudElapsed = gAudio->getAudioCurrentTime();
  gAudDuration = gAudio->getAudioFileDuration();

  if (gAudioState == A_CONNECTING) {
    if (running) {
      gAudioState = A_PLAYING;
      gAudLastData = millis();
      gAudLastBuf = buf;
      gAudLastI2s = i2sBytes;
      gAudRetry = 0;
      Serial.printf("[AUD] playing %s '%s' (buf=%u)\n", AUD_SRC_NAME[gSrc],
                    audTrackAt(gSrc, gIdx) ? audTrackAt(gSrc, gIdx)->name : "?", (unsigned)buf);
    } else if (millis() - gAudStartMs > gAudConnectGraceMs && !gAudRetryAt) {
      // 旧值固定 15s：播客 4 层重定向常常 30s 才拿到首字节，于是在连接阶段
      // 不停 stopSong -> 重连 -> 再等 30s，表现为「播客经常卡住」。
      Serial.printf("[AUD] connect timed out after %us\n", (unsigned)(gAudConnectGraceMs / 1000));
      audScheduleRetry();
    }
    return;
  }
  if (gAudioState != A_PLAYING) return;

  // 以前只看 InBuffer：播客是固定大文件，下载完成后 buf 只减不增，慢速 CDN
  // 下会被误判成断流。现在改为「InBuffer 变化 或 I2S 真在吐字节」都算活着。
  if (buf != gAudLastBuf || i2sBytes != gAudLastI2s) {
    gAudLastBuf = buf; gAudLastI2s = i2sBytes; gAudLastData = millis();
  }
  if (!running) {
    // 播客跟随 3xx 重定向时，库会把 isRunning() 短暂拉低。旧代码一看到
    // running=NO 就 audNext(1)，于是「点播放 -> 立刻跳到下一集 -> 又连接 -> 又跳」，
    // 用户感觉就是播客一直卡住。只有本次真的往 I2S 吐过 >4KB 才算「播完」。
    bool everSounded = (i2sBytes - gAudStartI2s) > 4096;
    if (gToneMode) {               // 离线测试音：本地文件，播完就收，别触发电台重连
      if (!everSounded && millis() - gAudStartMs < 5000) return;
      gToneMode = false; gAudioState = A_IDLE;
      Serial.println("[AUD] TONE finished");
      return;
    }
    if (!everSounded) {
      if (millis() - gAudStartMs > gAudConnectGraceMs) {
        Serial.println("[AUD] never produced sound within grace -> reconnect");
        audScheduleRetry();
      }
      return;                       // 还在重定向 / 建连，耐心等
    }
    Serial.println("[AUD] decoder stopped");
    if (gSrc == AS_RADIO) audScheduleRetry();
    else if (audCount(gSrc) > 1) audNext(1);   // TF / 播客：自动续播下一首
    else {                                      // 只有一首时别无限循环重播
      gAudioState = A_IDLE;
      Serial.println("[AUD] playlist finished (single track)");
    }
    return;
  }
  const uint32_t stallMs = (gSrc == AS_POD) ? 25000 : 15000;
  if (millis() - gAudLastData > stallMs) {
    Serial.printf("[AUD] stream stalled %us -> reconnect\n", (unsigned)(stallMs / 1000));
    audScheduleRetry();
  }
}

// ------------------------------------------------------------------ 命令执行
static void audExec(const AudCmd &c) {
  if (!gAudioReady) return;
  switch (c.cmd) {
    case AQ_PLAY:    gAudRetry = 0; audStart(c.arg >= 0 ? c.arg : gIdx); break;
    case AQ_SELECT:  gAudRetry = 0; audStart(c.arg); break;
    case AQ_TOGGLE:
      if (gAudioState == A_PLAYING) {
        gAudio->pauseResume(); gAudioState = A_PAUSED;
        Serial.println("[AUD] pause");
      } else if (gAudioState == A_PAUSED) {
        gAudio->pauseResume(); gAudioState = A_PLAYING;
        gAudLastData = millis();
        Serial.println("[AUD] resume");
      } else {
        gAudRetry = 0; audStart(gIdx);
      }
      break;
    case AQ_NEXT:    gAudRetry = 0; audNext(c.arg); break;
    case AQ_STOP:    audStop(); break;
    case AQ_TONE:    gToneFreq = c.arg; gToneMs = c.arg2; doTone(gToneFreq, gToneMs); break;
    case AQ_VOL:
      gAudioVol = c.arg;
      gVolDirty = true;                 // 由主循环统一同步，暂停态下仍保持静音
      Serial.printf("[AUD] volume -> %d/21\n", (int)gAudioVol);
      break;
    case AQ_SOURCE: {
      AudSrc ns = (AudSrc)constrain(c.arg, 0, (int)AS_COUNT - 1);
      bool wasPlaying = (gAudioState == A_PLAYING || gAudioState == A_CONNECTING);
      // 切源可能要扫卡 / 抓 RSS（同步 IO 数秒）。先停播，否则这段时间音频任务
      // 被占住、InBuffer 抽空，会听到一次明显断音。
      if (gAudioReady) { gAudio->stopSong(); gAudioState = A_IDLE; }
      gSrc = ns; gIdx = 0; gAudRetry = 0;
      if (ns == AS_TF && gTfN == 0) tfScan();
      if (ns == AS_POD && gPodN == 0) {
        gPodBusy = true;
        podFetchAny(0);
        gPodBusy = false;
      }
      Serial.printf("[AUD] source -> %s (%d tracks)\n", AUD_SRC_NAME[ns], audCount(ns));
      if (wasPlaying) {
        if (audCount(ns) > 0) audStart(0);
        else audStop();
      } else {
        gAudio->stopSong(); gAudioState = A_IDLE;
      }
      break;
    }
    case AQ_RESCAN_TF:
      if (gAudioReady) { gAudio->stopSong(); gAudioState = A_IDLE; }
      tfScan();
      if (gSrc == AS_TF) gIdx = 0;
      break;
    case AQ_PODFEED:
      if (gAudioReady) { gAudio->stopSong(); gAudioState = A_IDLE; }
      gPodBusy = true;
      if (c.text[0])          podFetchFeed(c.text);              // PODFEED <自定义 RSS URL>
      else if (c.arg > 0)      podFetchAny(c.arg - 1);            // FEED 按钮：切下一条中文播客
      else                     podFetchAny(gPodFeedIdx);          // 刷新当前 feed（失败自动换源）
      gIdx = 0;
      gPodBusy = false;
      break;
    case AQ_PODADD:
      podAdd(c.name, c.text);
      break;
    case AQ_PODCLEAR:
      gPodN = 0; podSave(); gIdx = 0;
      Serial.println("[POD] list cleared");
      break;
    case AQ_TFTEST:
      if (gAudioReady) { gAudio->stopSong(); gAudioState = A_IDLE; }
      // arg=1 写一个中文文件名，用来端到端验收「TF 卡中文歌名」链路
      if (tfMakeTestWav(c.arg ? "/music/中文歌名测试.wav" : "/music/test-tone.wav")) {
        tfScan();
        gSrc = AS_TF; gIdx = 0; gAudRetry = 0;
        if (gTfN > 0) audStart(0);           // 直接播刚生成的 WAV，端到端验收
        else Serial.println("[TF] scan found no audio after write");
      }
      break;
    default: break;
  }
}

static void audioTask(void *) {
  // 在 Core0 上创建 Audio：I2S 与 GDMA 中断随之注册到 Core0，
  // 与 Core1 的 QSPI 刷屏 / Wi-Fi 网络任务彻底隔离。
  gAudio = new Audio();
  if (gAudio) {
    gAudio->setPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_MCLK_PIN);
    gAudio->setVolume(16);
    // setConnectionTimeout 同时作用于 WiFiClientSecure 的 connect **和 read**。
    // 旧值 (3000,2000) 让直播流一次 chunk 间隔 >2s 就被误判断连。
    gAudio->setConnectionTimeout(8000, 8000);
    gAudioReady = true;
    Serial.println("[AUD] Audio/I2S initialized on Core0 (codec 44.1k -> lib resamples 48k)");
  } else {
    Serial.println("[AUD] Audio alloc FAILED");
  }
  for (;;) {
    AudCmd c;
    int budget = 4;
    while (budget-- && xQueueReceive(gAudQ, &c, 0) == pdTRUE) audExec(c);

    if (gAudRetryAt && millis() >= gAudRetryAt) {
      gAudRetryAt = 0;
      if (audCount(gSrc) > 0) audStart(gIdx);
    }

    // pauseResume() 只让库停止喂数据，I2S/GDMA 仍在跑；auto_clear=true 会发零，
    // 但再叠一层数字静音最保险，彻底杜绝「暂停后一直重复同一个音」。
    // 集中在这里按状态同步，TOGGLE / STOP / SOURCE / VOL 各路径都不会漏。
    bool wantMute = (gAudioState == A_PAUSED || gAudioState == A_IDLE || gAudioState == A_ERROR);
    if (gAudioReady && (wantMute != gMuteApplied || gVolDirty)) {
      gAudio->setVolume(wantMute ? 0 : (uint8_t)gAudioVol);
      gMuteApplied = wantMute;
      gVolDirty = false;
    }

    if (gAudioReady && gAudioState != A_IDLE && gAudioState != A_ERROR) {
      gAudio->loop();
      gAudLoops++;
      audMonitor();
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ------------------------------------------------------------------ 对外 API
static void audioPlay(int idx)            { audSend(AQ_PLAY, idx); }
static void audioSelect(int idx)          { audSend(AQ_SELECT, idx); }
static void audioNext(int dir)            { audSend(AQ_NEXT, dir); }
static void audioToggle()                 { audSend(AQ_TOGGLE); }
static void audioStop()                   { audSend(AQ_STOP); }
static void audioToneRequest(int f, int ms) { audSend(AQ_TONE, f, ms); }
static void audioVolume(int v)            { audSend(AQ_VOL, constrain(v, 0, 21)); }
static void audioSetSource(int s)         { audSend(AQ_SOURCE, s); }
static void audioRescanTF()               { audSend(AQ_RESCAN_TF); }
static void audioTfTest()                 { audSend(AQ_TFTEST); }
static void audioTfTestCN()               { audSend(AQ_TFTEST, 1); }   // 中文歌名验收
static void audioPodFeed(const char *url) { audSend(AQ_PODFEED, 0, 0, url); }
static void audioPodFeedNext()            { audSend(AQ_PODFEED, (gPodFeedIdx + 1) % POD_FEED_COUNT + 1); }
static void audioPodFeedSelect(int idx)   { audSend(AQ_PODFEED, constrain(idx, 0, POD_FEED_COUNT - 1) + 1); }
static int  audioPodFeedIndex()           { return gPodFeedIdx; }
static const char *audioPodFeedName()     { return POD_FEEDS[gPodFeedIdx].name; }
static void audioPodAdd(const char *name, const char *url) { audSend(AQ_PODADD, 0, 0, url, name); }
static void audioPodClear()               { audSend(AQ_PODCLEAR); }

static AudSrc  audioSource()       { return gSrc; }
static int     audioIndex()        { return gIdx; }
static int     audioCount(int s)   { return audCount((AudSrc)constrain(s, 0, 2)); }
static const char *audioTrackName(int s, int i) {
  AudTrack *t = audTrackAt((AudSrc)constrain(s, 0, 2), i);
  return t ? t->name : "";
}
static const char *audioCurrentName() {
  AudTrack *t = audTrackAt(gSrc, gIdx);
  return t ? t->name : "NO TRACK";
}
static bool    audioTfReady()      { return gTfN > 0; }
static uint32_t audioElapsed()     { return gAudElapsed; }
static uint32_t audioDuration()    { return gAudDuration; }
static void audioLoop() { /* no-op: 音频由独立任务喂 */ }

static void audioStatusDump() {
  const char *as = gAudioState == A_IDLE ? "IDLE" :
                   gAudioState == A_CONNECTING ? "CONNECTING" :
                   gAudioState == A_PLAYING ? "PLAYING" :
                   gAudioState == A_PAUSED ? "PAUSED" : "ERROR";
  Serial.printf("[AUD] state=%s src=%s idx=%d/%d running=%s loops=%u buf=%u pos=%us/%us i2s=%u hwm=%u retry=%d\n",
                as, AUD_SRC_NAME[gSrc], (int)gIdx + 1, audCount(gSrc),
                (gAudioReady && gAudio->isRunning()) ? "YES" : "NO",
                (unsigned)gAudLoops, (unsigned)gAudBuf,
                (unsigned)gAudElapsed, (unsigned)gAudDuration,
                (unsigned)gI2sChunks, (unsigned)(gAudioReady ? gAudio->getHighWatermark() : 0),
                gAudRetry);
  Serial.printf("[AUD] i2sdiag w=%u to=%u zero=%u bytes=%u smp=%u cbsmp=%u\n",
                (unsigned)gI2sDiag[0], (unsigned)gI2sDiag[1], (unsigned)gI2sDiag[2],
                (unsigned)gI2sDiag[3], (unsigned)gI2sDiag[4], (unsigned)gI2sDiag[5]);
  // 0x1000 表示 ESP_OK(0)，其余是 esp_err_t + 0x1000
  Serial.printf("[AUD] i2sinit init=%X gpio=%X preload=%X enable=%X\n",
                (unsigned)gI2sDiag[6], (unsigned)gI2sDiag[7],
                (unsigned)gI2sDiag[8], (unsigned)gI2sDiag[9]);
  Serial.printf("[AUD] radio=%d tf=%d pod=%d vol=%d/21 gain=%u mute=%s\n",
                gRadioN, (int)gTfN, (int)gPodN, (int)gAudioVol,
                (unsigned)(gAudioReady ? gAudio->getVolume() : 0),
                gMuteApplied ? "YES" : "NO");
  for (int i = 0; i < gPodN; i++)
    Serial.printf("  POD %2d %-28s %s\n", i + 1, gPod[i].name, gPod[i].url);
}

// ------------------------------------------------------------------ 初始化
static bool audioInit() {
  for (int i = 0; i < STREAM_COUNT; i++) {
    strncpy(gRadio[i].name, STREAM_LABELS[i], sizeof(gRadio[i].name) - 1);
    strncpy(gRadio[i].url,  STREAM_URLS[i],  sizeof(gRadio[i].url) - 1);
  }
  podLoad();

  gCodec = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (!gCodec) { Serial.println("[AUD] ES8311 create failed"); return false; }
  es8311_clock_config_t clk;
  clk.mclk_inverted      = false;
  clk.sclk_inverted      = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency     = AUDIO_SR * 256;
  clk.sample_frequency   = AUDIO_SR;
  esp_err_t e = es8311_init(gCodec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (e != ESP_OK) { Serial.printf("[AUD] ES8311 init failed %d\n", (int)e); return false; }
  es8311_voice_volume_set(gCodec, 70, nullptr);
  es8311_microphone_config(gCodec, false);
  es8311_voice_mute(gCodec, false);

  pinMode(PA_EN_PIN, OUTPUT);
  digitalWrite(PA_EN_PIN, HIGH);

  // 【关键】Audio 对象（进而 I2S / GDMA 中断）必须在 Core0 上创建。
  // 原来在 setup() 里 new Audio()，而 setup 跑在 Core1 的 loopTask 上，
  // I2S DMA 回调就注册在 Core1；播放一开始它会抢占同样在 Core1 的 QSPI 刷屏，
  // 在 spi_master.c:setup_priv_desc 处触发 StoreProhibited 空指针写 -> 整机重启。
  // 实测 backtrace: i2s_dma_tx_callback -> gdma_default_tx_isr -> _xt_lowint1，
  // 被打断的正是 flushScreen() -> LCD_addWindow() -> spi_device_queue_trans()。
  gAudQ = xQueueCreate(8, sizeof(AudCmd));
  // 优先级必须**低于**库内部解码任务（优先级 2）。解码任务在 playChunk() 里
  // 用 i2s_channel_write(timeout=50ms) 等 I2S DMA；喂流任务若用更高优先级每 2ms
  // 抢占它，I2S 写会超时，库把 m_validSamples 永久卡住 -> 再也不解码 -> 静音。
  // 这就是"有点声音马上就没有了"的最后一环。Core0 现在只有这两个任务，
  // 让解码优先(2)、喂流次之(1) 才是库作者的设计。
  xTaskCreatePinnedToCore(audioTask, "audfeed", 16384, nullptr, 1, nullptr, 0);
  for (int i = 0; i < 60 && !gAudioReady; i++) delay(50);   // 等 Core0 完成 I2S 初始化
  Serial.printf("[AUD] ES8311 ready, I2S on Core0 = %s\n", gAudioReady ? "OK" : "FAILED");
  return gAudioReady;
}

// ------------------------------------------------------------------ 离线自测音
// 生成 44.1kHz/16bit/mono WAV 写到 FFat，再 connecttoFS 走完整 I2S 链路。
static bool doTone(int freqHz, int ms) {
  if (!gAudioReady) { Serial.println("[AUD] codec not ready"); return false; }
  if (freqHz < 50) freqHz = 50;
  if (freqHz > 8000) freqHz = 8000;
  if (ms < 200) ms = 200;
  if (ms > 8000) ms = 8000;
  if (!FFat.begin(true)) { Serial.println("[AUD] FFat mount failed"); gAudioState = A_ERROR; return false; }

  const uint32_t SR = 44100;
  uint32_t n = SR * (uint32_t)ms / 1000;
  uint32_t dataLen = n * 2;
  uint8_t h[44] = {0};
  memcpy(h + 0, "RIFF", 4);  uint32_t riff = 36 + dataLen; memcpy(h + 4,  &riff, 4);
  memcpy(h + 8, "WAVE", 4);  memcpy(h + 12, "fmt ", 4);
  uint32_t fmtLen = 16;      memcpy(h + 16, &fmtLen, 4);
  uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  uint32_t sr = SR, byteRate = SR * 2;
  memcpy(h + 20, &pcm, 2);   memcpy(h + 22, &ch, 2);     memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byteRate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataLen, 4);

  File f = FFat.open("/tone.wav", FILE_WRITE);
  if (!f) { Serial.println("[AUD] FFat open /tone.wav failed"); gAudioState = A_ERROR; return false; }
  f.write(h, 44);
  int16_t buf[256];
  const uint32_t fade = SR / 50;
  for (uint32_t i = 0; i < n;) {
    uint32_t k = (n - i < 256) ? (n - i) : 256;
    for (uint32_t j = 0; j < k; j++) {
      uint32_t s = i + j;
      float env = 1.0f;
      if (s < fade) env = (float)s / fade;
      else if (n - s < fade) env = (float)(n - s) / fade;
      buf[j] = (int16_t)(12000.0f * env * sinf(2.0f * PI * (float)freqHz * (float)s / SR));
    }
    f.write((const uint8_t *)buf, k * 2);
    i += k;
  }
  f.close();
  Serial.printf("[AUD] TONE %dHz %dms -> /tone.wav (%u bytes)\n", freqHz, ms, dataLen);
  gAudio->stopSong();
  bool ok = gAudio->connecttoFS(FFat, "/tone.wav");
  gConnected = false;
  gSrc = AS_RADIO;                 // TONE 是离线自测，仍归属电台 tab
  gToneMode = true;                // 播完直接收工，见 audMonitor()
  gAudioState = ok ? A_CONNECTING : A_ERROR;
  gAudStartMs = millis(); gAudLastData = millis();
  Serial.printf("[AUD] connecttoFS %s\n", ok ? "OK" : "FAILED");
  return ok;
}

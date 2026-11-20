// ============================================================================
//  ESP32-S3-Touch-LCD-1.85B  ·  圆屏演示固件
//  机械表盘 / 电子表盘 / 音乐播放 / 航班雷达 / 番茄钟
//  自绘 RGB565 帧缓冲（不用 LVGL）；仅音频依赖 ESP32-audioI2S
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "config.h"
#include "gfx.h"
#include "font_aero.h"     // AERO_LOGO_H：表盘标志的自适应 y 要用
#include "Display_ST77916.h"
#include "I2C_Driver.h"
#include "Touch_CST816.h"
#include "runtime_config.h"
#include "audio_player.h"
#include "provision.h"

extern uint8_t Touch_interrupts;

#define COL(r, g, b) ((uint16_t)((((uint16_t)(r) >> 3) << 11) | (((uint16_t)(g) >> 2) << 5) | ((uint16_t)(b) >> 3)))

// 调色板（与 HTML 原型一致）
#define C_ORANGE    COL(226, 98, 46)
#define C_ORANGE_D  COL(141, 63, 39)
#define C_WHITE     COL(242, 245, 250)
#define C_PAPER     COL(244, 241, 233)
#define C_INK       COL(32, 38, 43)
#define C_CREAM     COL(207, 194, 168)
#define C_DARK      COL(11, 16, 20)
#define C_DARK2     COL(16, 23, 30)
#define C_SLATE     COL(43, 53, 61)
#define C_GREY      COL(139, 151, 169)
#define C_GREEN     COL(126, 230, 171)
#define C_GREEN_D   COL(32, 80, 60)
#define C_RADAR_BG  COL(6, 16, 14)
#define C_YELLOW    COL(240, 180, 41)
#define C_BLUE      COL(88, 168, 224)
#define C_VINYL     COL(13, 17, 24)

#include "ui_kit.h"       // 二期统一 UI 组件（页面底 / 圆键 / 胶囊键 / 蛋形）
#include "pet.h"          // v2.0 拓麻歌子整机（依赖上面的颜色宏 + ui_kit）
#include "words.h"        // v1.0 百词斩式背单词（内含 wordbank.h）
#include "mic_recorder.h" // v1.0 ES7210 双麦录音（官方驱动）
#include "ai_cloud.h"     // v1.0 百炼 ASR / Qwen / Todo / 凭据
#include "xiaozhi_client.h" // v2.7 小智真协议：opus 双向 + 连续对话 + TTS 出声
#include "recorder.h"     // v2.5 录音机：WAV 存 TF 卡 + 回放（纪要按需）

// v1.0（二期）：保留 0..6 的历史编号，串口 `PAGE n` 命令不变；新页从 7 起追加。
enum Page : uint8_t {
  P_ANALOG = 0, P_DIGITAL, P_MUSIC, P_RADAR, P_POMO, P_WIFI, P_SETTINGS,
  P_APPS,   // 7  应用中心（除表盘外所有入口，Apple Watch 蜂窝）
  P_CHAT,   // 8  小智语音问答
  P_WORDS,  // 9  背单词
  P_MEMO,   // 10 语音备忘录 / Todo
  P_PET,    // 11 电子宠物
  P_RECORDER, // 12 录音机 / AI 会议记录
  P_TODO,   // 13 Todo 清单
  PAGE_COUNT
};
static Page curPage = P_ANALOG;
static int  detailIdx = -1;                   // 雷达选中目标详情卡索引

// v2.1：主界面 = 两块表盘（模拟 / 数字），原地切换，不再各占一个入口。
//   0 = 模拟（机械表盘，默认）  1 = 数字（大字号电子表盘）
// 切换手势：点轴心（r≤34）或上滑（表盘上下滑是唯一还没被占用的方向，下拉=设置）。
// 选择写 NVS "face"，掉电保留。
static int gFace = 0;

// v2.2：数字表盘的文字语言。0 = 中文（星期 + 天气描述）  1 = English
// **只作用于数字表盘** —— 用户明确「模拟表盘不要改」，机械盘固定保留衬线两字母星期窗
// 与中文天气副盘（模板做出来就是这个样子，动了反而破坏盘面）。
// 入口：设置页第 4 行右侧 LANG 卡片（点一下切换），写 NVS "lang"，掉电保留。
static int gLang = 0;

// v0.16：天气/雷达共用的运行时位置。设置页 CITY 选择后写 NVS，掉电保留；
// 换城市会同时让天气与 ADS-B 立刻按新坐标重拉。
int   gCityIdx = 0;
float gHomeLat = (float)HOME_LAT, gHomeLon = (float)HOME_LON;
static bool   gCityList  = false;              // 设置页里的城市选择浮层
static int    gCityScroll = 0;
#define CITY_ROWS 4
#define CITY_ROW_Y0 112
#define CITY_ROW_H  36

// ---------------------------------------------------------------- 锁屏 + 提示
static bool gLocked = false;                       // 锁屏状态
static uint32_t gToastUntil = 0;                   // 临时提示消失时刻
static char gToast[44] = {0};
static int  gTodoPage = 0;                         // 待办清单当前页（5 条/页）
static int  gTodoDeadlineFor = -1;                 // >=0：正在给第 i 条设期限（清单的子页面）
static uint32_t gTodoClearArmUntil = 0;            // 清空二次确认：首次点后 3s 内再点才清
// v0.15：屏幕虚拟锁键 / 齿轮及其热区全部删除；息屏用 BOOT 实体键，设置用下拉手势。
// 右上实体按键 = BOOT 键（GPIO0，板载上拉、按下拉低）。
// 官方只有这两个实体键：BOOT 可编程，PWR 是硬件电源锁存开关（不支持自定义）。
#define BTN_PIN 0
#define BTN_DEBOUNCE_MS 25

static void render();                              // 前向声明（导航/锁屏里立即重绘）
static void handleTap(int x, int y);               // 前向声明（串口 TAP 注入用）
static void fitText(char *out, size_t outsz, const char *src, int font, int maxW);  // 定义在配网页之后
static void injectSwipe(int x1, int y1, int x2, int y2);  // 定义在触摸事件结构之后（串口 SWIPE 注入用）
static void showToast(const char *s) {
  strncpy(gToast, s, sizeof(gToast) - 1); gToast[sizeof(gToast) - 1] = 0;
  gToastUntil = millis() + 1800;
}
// 主界面换表盘：模拟 <-> 数字。两页共用 P_ANALOG 这个"家"，
// 所以进应用中心/回 home 的手势完全不受影响。
static void toggleClockFace() {
  gFace = gFace ? 0 : 1;
  cfgSetInt("face", gFace);
  showToast(gFace ? "数字表盘" : "模拟表盘");
  render();
  Serial.printf("[UI] clock face -> %s\n", gFace ? "DIGITAL" : "ANALOG");
}
// 数字表盘语言：中文 <-> English（模拟表盘不受影响）。切完立刻重绘。
static void toggleLang() {
  gLang = gLang ? 0 : 1;
  cfgSetInt("lang", gLang);
  showToast(gLang ? "LANG: ENGLISH" : "数字表盘 中文");
  render();
  Serial.printf("[UI] clock lang -> %s\n", gLang ? "EN" : "CN");
}
// drawText() 的 y 是**字形顶部**（字形占 y..y+font*scale-1），不是中心也不是基线。
// 按钮里若直接把按钮中心 y 传进去，12px 文字会整体偏下 (12/2 - boxH/2 ... ) 约 5~6px。
// 所有按钮标签一律走这里：y = boxY + (boxH - 字高)/2。
static void drawBtnText(int x, int boxY, int boxH, const char *s, uint16_t col,
                        int font = 12, int scale = 1, int align = 1) {
  int h = (font == 12 ? 12 : font == 16 ? 16 : 24) * scale;
  drawText(x, boxY + (boxH - h) / 2, s, col, font, scale, align);
}

// ---------------------------------------------------------------- BQ27220 电量计（v0.16）
// 微雪官方引脚表：电量计 BQ27220 在共享 I2C(GPIO10/11) 上，7-bit 地址 0x55，
// 板载 500 mAh 锂电池经 MX1.25 接口接入，USB-C 供电时充电。
// 标准命令：0x2C StateOfCharge(低字节 %) / 0x08 Voltage(mV) / 0x0C Current(mA, 有符号)
//           0x0A BatteryStatus / 0x10 RemainingCapacity / 0x12 FullChargeCapacity
// 充电判定用电流 >20 mA（TI CEDV 约定：正=充电，负=放电），不依赖不存在的充电 GPIO。
#define BQ27220_ADDR 0x55
struct BatteryState {
  bool     valid = false;
  bool     charging = false;
  int      soc = 0, mv = 0, ma = 0, rm = 0, fcc = 0;
  uint16_t status = 0;
  uint32_t lastMs = 0;
};
static BatteryState gBat;
// 验收注入：串口 BATF <soc> [chg] 可强制电量显示（-1 = 用真实读数），
// 只为在没有真实低电/充电工况时验证颜色与格数，不写 NVS、不影响电量计本身。
static int gBatFakeSoc = -1, gBatFakeChg = -1;

static bool bqRead16(uint8_t reg, uint16_t &v) {
  // 触摸任务与主循环都会读 I²C（还有 RTC、ES8311），统一走 I2C_Lock 串行
  I2C_Lock();
  bool ok = true;
  Wire.beginTransmission(BQ27220_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) ok = false;                 // repeated START
  else if (Wire.requestFrom((uint8_t)BQ27220_ADDR, (uint8_t)2) != 2) ok = false;
  if (ok) {
    uint8_t lo = (uint8_t)Wire.read(), hi = (uint8_t)Wire.read();
    v = (uint16_t)lo | ((uint16_t)hi << 8);
  }
  I2C_Unlock();
  return ok;
}
static void batteryPoll(bool force = false) {
  if (!force && millis() - gBat.lastMs < 3000) return;
  gBat.lastMs = millis();
  uint16_t soc = 0, mv = 0, ma = 0, st = 0, rm = 0, fcc = 0;
  if (!bqRead16(0x2C, soc)) { gBat.valid = false; return; }   // NAK = 没焊芯片 / 没电池
  bqRead16(0x08, mv); bqRead16(0x0C, ma); bqRead16(0x0A, st);
  bqRead16(0x10, rm); bqRead16(0x12, fcc);
  // 电压不在单节锂电范围就认为电池没插，绝不显示编造的百分比
  gBat.valid   = (mv >= 2500 && mv <= 4500);
  gBat.soc     = constrain((int)(soc & 0xFF), 0, 100);
  gBat.mv      = (int)mv;
  gBat.ma      = (int)(int16_t)ma;
  gBat.rm      = (int)rm; gBat.fcc = (int)fcc;
  gBat.status  = st;
  gBat.charging = gBat.valid && (gBat.ma > 20);
}
// 满电且插着 USB 时电流≈0，既不是充电也不是放电，别硬报 DISCHARGING
static const char *batteryStateText() {
  if (!gBat.valid) return "NO BAT / USB";
  if (gBat.charging) return "CHARGING";
  if (gBat.ma < -20) return "DISCHARGING";
  return "FULL / IDLE";
}

// ---------------------------------------------------------------- 设置项（亮度 / 音量）
// 都存在 NVS（rcfg 命名空间），掉电保留；设置页里改完立即生效并落盘。
static int gBright = BACKLIGHT_LEVEL;   // 背光 10..100
static int gVolume = 16;                // 解码器音量 0..21
#define BRIGHT_MIN 10
#define BRIGHT_MAX 100
#define BRIGHT_STEP 10
#define VOL_MAX    21

// ---------------------------------------------------------------- Wi-Fi 配网（纯本机，脱离手机/电脑）
// 主页左上角 WiFi 图标 -> 进入本页；本页扫描周边网络、点选、屏上键盘输密码、写 NVS 重启联网。
enum WifiState : uint8_t { W_LIST = 0, W_KEY };
static WifiState gWifiState = W_LIST;
static bool gWifiScanning = false;
static bool gWifiScanFailed = false;   // 扫描被打断/失败（STA 后台重连时常见）
static int  gWifiScanRetry = 0;
static uint32_t gWifiScanRetryMs = 0;
static bool gWifiStoppedReconnect = false;   // 为了扫描临时掐掉了 STA 重连
static int  gWifiN = 0;
static int  gWifiScroll = 0;
static int  gWifiSel = -1;
static bool gWifiSecured = true;
static String gWifiSSID = "";
static String gWifiPass = "";
static bool gWifiSym = false;       // 字母 / 数字·符号 切换
static bool gWifiCaps = false;      // 字母大小写（Wi-Fi 密码区分大小写）
static char gNetSSID[16][33];
static int  gNetRssi[16];
static int  gNetEnc[16];
// v0.15：主页左上角的设置齿轮已删除，改由「任意页面下拉」进入设置页。

// ---------------------------------------------------------------- 设置页布局（圆屏安全区 r≤158）
// v2.2 重排：5 行（主界面表盘 / 亮度 / 音量 / [表盘语言|语言] / 城市）。
//   圆内能放整行 268 宽卡片的 y 区间是 96..264；再往下只有窄卡能进圆。
//   行高 34 / 间距 4：96, 134, 172, 210, 248（末行 248..282 用 236 宽窄卡）。
//   原来底部的 IP 状态行删掉（WIFI 行右侧已经显示 SSID/状态，IP 属冗余）。
#define ST_RX      47
#define ST_RW      266
#define ST_RH      34
#define ST_R1      96
#define ST_R2      134
#define ST_R3      172
#define ST_R4      210
#define ST_R5      248
// 第 4 行两颗半宽卡片（FACE 47..175 / LANG 185..313，间隙 10 居中）—— 外缘与上面三行严格对齐
#define ST_HALF_W  128
#define ST_FACE_X  47
#define ST_LANG_X  185
// 第 5 行城市卡：y=282 时圆内可用 x 只有 62..298，所以只做 236 宽
#define ST_CITY_X  62
#define ST_CITY_W  236
#define ST_BTN_MINUS_CX 238
#define ST_BTN_PLUS_CX  290
#define ST_BTN_R   15
#define ST_BAR_X   58
#define ST_BAR_W   150

// ---------------------------------------------------------------- 导航模型（v2.0 起）
// 除主界面（表盘）外，**所有入口都收进应用中心**，页面之间不再是十字/环形。
// 手势照手机来（v2.0 用户要求）：
//   · 表盘     ：左右滑 -> 应用中心          上滑 / 点轴心 -> 换表盘（模拟 <-> 数字，v2.1）
//   · 应用中心 ：任意方向滑 -> 回表盘
//   · 任意应用 ：上滑 -> 回表盘（home 手势）；其余方向 -> 回应用中心（上一层）
// 于是只有三层：表盘 <-> 应用中心 <-> 某个应用；上滑永远是"回桌面"。
// 表盘上唯一被特殊处理的是上滑 —— 这一层没有 home 可回，正好用来换表盘。
// 设置页是从哪一页下拉进来的 —— 退出设置要回原页（v2.8 起返回全靠手势，所以要提前声明）
static Page gSettingsFrom = P_ANALOG;

static bool navigate(char dir) {
  Page np = curPage;
  if (curPage == P_ANALOG) {
    if (dir == 'L' || dir == 'R') np = P_APPS;
  } else if (curPage == P_APPS) {
    np = P_ANALOG;
  } else if (dir == 'U') {
    np = P_ANALOG;                 // home：任意应用上滑回表盘
  } else {
    // v2.8：页面上的返回键全部撤掉，左右/下滑 = 返回上一层。
    // 上一层不一定是应用中心：设置来自表盘、配网来自设置、备忘可能来自待办。
    if      (curPage == P_SETTINGS)                    np = gSettingsFrom;
    else if (curPage == P_WIFI)                        np = P_SETTINGS;
    else if (curPage == P_MEMO && gMemoBackToTodo)     np = P_TODO;
    else                                               np = P_APPS;
  }
  if (np == curPage) return false;
  // 离开页面的收尾（原来挂在返回键的点击里，键撤了就得搬到这儿）
  if (curPage == P_RECORDER) recStopPlay();
  if (curPage == P_MEMO) gMemoBackToTodo = false;
  if (curPage == P_TODO) { gMemoLastAdded = 0; gTodoNewFrom = -1; }   // 清掉"新"高亮
  curPage = np; detailIdx = -1;
  Serial.printf("[UI] nav %c -> page %d\n", dir, (int)curPage);
  return true;
}

// ---------------------------------------------------------------- 软时钟
static int gHour = 10, gMin = 28, gSec = 42;
static int gDay = 14, gMon = 11, gYear = 2026, gWday = 6;
static bool gClockSynced = false;
static time_t lastClockEpoch = 0;
static std::atomic<bool> gNtpUpdated{false};
// v0.19：时间来源（BUILD 编译时刻 / RTC 板载时钟 / NTP 网络），STATUS 里可见
static const char *gTimeSrc = "BUILD";
// 开机那一刻实际用的是哪个源。USB-CDC 复位时开机日志常常抓不到（主机还没枚举完），
// 所以单独留一个字段给 STATUS 读，随时能验证"断网开机到底准不准"。
static char gBootTimeSrc[8] = "BUILD";
// 对时后由主循环写回 RTC。
static bool gRtcDirty = false;

static int daysInMonth(int y, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return d[m - 1];
}
// Sakamoto：0=周日，和 tm_wday 一致。RTC 自带的 weekday 寄存器各家写法的基准不一样，
// 一律不用它，按年月日自己算。
static int weekdayOf(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int yy = y - (m < 3 ? 1 : 0);
  return (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
}
static void clockTick() {
  time_t now = time(nullptr);
  if (now == lastClockEpoch) return;
  struct tm local;
  if (!localtime_r(&now, &local) || local.tm_year + 1900 < 2024) return;
  lastClockEpoch = now;
  const int year = local.tm_year + 1900, month = local.tm_mon + 1;
  if (gYear != year || gMon != month || gDay != local.tm_mday) gWday = local.tm_wday;
  gYear = year; gMon = month; gDay = local.tm_mday;
  gHour = local.tm_hour; gMin = local.tm_min; gSec = local.tm_sec;
}

static void clockSetSystem() {
  struct tm local = {};
  local.tm_year = gYear - 1900;
  local.tm_mon = gMon - 1;
  local.tm_mday = gDay;
  local.tm_hour = gHour;
  local.tm_min = gMin;
  local.tm_sec = gSec;
  local.tm_isdst = -1;
  const struct timeval timestamp = {mktime(&local), 0};
  settimeofday(&timestamp, nullptr);
  lastClockEpoch = 0;
}

static void onNtpSync(struct timeval *) { gNtpUpdated.store(true); }

static void clockApplyNtp() {
  if (!gNtpUpdated.exchange(false)) return;
  clockTick();
  gWday = weekdayOf(gYear, gMon, gDay);
  gClockSynced = true;
  gTimeSrc = "NTP";
  gRtcDirty = true;
  Serial.printf("[TIME] SNTP -> %04d-%02d-%02d %02d:%02d:%02d wday=%d\n",
                gYear, gMon, gDay, gHour, gMin, gSec, gWday);
}
static const char *WDAY_EN[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
// 模拟表盘左侧星期窗：中文数字，0=周日，与 tm_wday 一致
static const char *WDAY_CN[] = {"日", "一", "二", "三", "四", "五", "六"};
// v0.24：星期窗改两字母缩写（朗格 Datograph 的写法）。单个中文"一"就是一条横线，
// 在深底窗里几乎看不见；两个衬线字母才撑得起窗面。
static const char *WDAY_2[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
// 未联网时软时钟别停在写死的 2026-11-14 10:28 —— 那比真实时间差几小时，
// 用户会直接判定"没对时"。用固件编译时刻做起点，离线也基本是对的。
static void initSoftClockFromBuild() {
  static const char *MN[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char mon[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  int m = 1;
  for (int i = 0; i < 12; i++) if (!strcmp(mon, MN[i])) { m = i + 1; break; }
  gMon  = m;
  gDay  = atoi(__DATE__ + 4);
  gYear = atoi(__DATE__ + 7);
  gHour = atoi(__TIME__);
  gMin  = atoi(__TIME__ + 3);
  gSec  = atoi(__TIME__ + 6);
  if (gYear < 2024 || gMon < 1 || gMon > 12 || gDay < 1 || gDay > 31) {
    gYear = 2026; gMon = 1; gDay = 1; gHour = 0; gMin = 0; gSec = 0;
  }
  // 星期：Sakamoto 公式（0=周日，和 tm_wday 一致）
  gWday = weekdayOf(gYear, gMon, gDay);
  gTimeSrc = "BUILD";
  Serial.printf("[TIME] soft clock seeded from build: %04d-%02d-%02d %02d:%02d:%02d\n",
                gYear, gMon, gDay, gHour, gMin, gSec);
}

// ---------------------------------------------------------------- 断电兜底（NVS 记住最后一次时间）
// 板载 RTC 只有在装了备用电池时才真正"断电继续走"。没装电池时，一断电 RTC 就归零，
// 此时退回"编译时刻"可能差好几个月。这里把最后一次对好的时间写进 NVS（掉电不丢），
// 开机优先级：RTC（准）> NVS（最后一次已知，误差≈关机时长）> 编译时刻。
// 打包成 27 bit，字段按高位到低位排列，所以直接比大小就能判新旧。
static uint32_t packClock() {
  return ((uint32_t)(gYear - 2000) << 20) | ((uint32_t)gMon << 16) |
         ((uint32_t)gDay << 11) | ((uint32_t)gHour << 6) | (uint32_t)gMin;
}
static void persistClock() {
  cfgSetInt("lastclk", (int)packClock());
}
static bool restoreClockFromNvs() {
  uint32_t v = (uint32_t)cfgGetInt("lastclk", 0);
  if (!v) return false;
  int y  = 2000 + (int)((v >> 20) & 0x7F);
  int mo = (int)((v >> 16) & 0x0F);
  int d  = (int)((v >> 11) & 0x1F);
  int h  = (int)((v >> 6)  & 0x1F);
  int mi = (int)(v & 0x3F);
  if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo) ||
      h > 23 || mi > 59) return false;
  uint32_t cur = packClock();
  if (v <= cur) return false;              // 比编译时刻还旧就没意义
  gYear = y; gMon = mo; gDay = d; gHour = h; gMin = mi; gSec = 0;
  gWday = weekdayOf(y, mo, d);
  gTimeSrc = "NVS";
  snprintf(gBootTimeSrc, sizeof(gBootTimeSrc), "NVS");
  Serial.printf("[TIME] restored last known time from NVS: %04d-%02d-%02d %02d:%02d\n",
                gYear, gMon, gDay, gHour, gMin);
  return true;
}

// ---------------------------------------------------------------- 板载 RTC（PCF85063A @0x51）
// 板上有一颗 PCF85063，I²C 0x51，和触摸 / BQ27220 / ES8311 共用 GPIO10(SCL)/11(SDA)，
// 中断脚接 GPIO6（这里不用中断，轮询即可）。有了它，**断网开机时间也是准的**：
//   · 开机：先读 RTC，读到合法时间就盖掉"编译时刻"这个兜底
//   · 联网：NTP 对完时立刻写回 RTC；之后每 3 h 再对一次（见 NET_NTP_REFRESH_MS）
//   · 断网运行：每 10 min 读一次 RTC，发现软时钟漂了 ≥2 s 就按 RTC 纠正
// 注意：RTC 走时要靠板上的 RTC 备用电池（板上留有电池座/焊盘），没装电池的话
// 断电后时间会重置，此时按"时间不可信"处理，退回编译时刻并在串口如实报告。
//
// ⭐ 寄存器偏移是按**实测**定的，不是照抄数据手册：PCF85063A 手册写秒在 0x02，
//   但这块板上的实际器件时间是 0x04 起。判据（2026-11-15 实测）：
//     · 往 0x04..0x0A 写 55 58 23 15 00 11 26 → 原样读回；
//     · 等 8 秒：秒 0x55→0x06 且分 0x58→0x59（真实 1Hz 进位）；
//     · 往 0x08 写 0x26 → 读回 0x06（= 0x26 & 0x07，星期寄存器掩码）。
//   0x02/0x03 可写但从不走时，0x0B..0x0E 恒为 0x80（RAM 区），所以别用它们。
#define PCF85063_ADDR   0x51
#define RTC_REG_SEC     0x04   // 0x04..0x0A = 秒 分 时 日 星期 月 年（连续 7 字节）

static bool gRtcFound = false;   // 芯片有应答
static bool gRtcValid = false;   // 读出的时间可信（字段合法）
static uint32_t gRtcLastWrite = 0;
static uint32_t gRtcLastSync  = 0;

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)(((v >> 4) & 0x0F) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool rtcReadRegs(uint8_t reg, uint8_t *b, uint8_t n) {
  I2C_Lock();
  bool ok = true;
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) ok = false;    // repeated START
  else if (Wire.requestFrom((uint8_t)PCF85063_ADDR, n) != n) ok = false;
  if (ok) for (uint8_t i = 0; i < n; i++) b[i] = (uint8_t)Wire.read();
  I2C_Unlock();
  return ok;
}
static bool rtcReadBlock(uint8_t *b, uint8_t n) { return rtcReadRegs(RTC_REG_SEC, b, n); }
static bool rtcWriteBlock(uint8_t reg, const uint8_t *b, uint8_t n) {
  I2C_Lock();
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < n; i++) Wire.write(b[i]);
  bool ok = (Wire.endTransmission(true) == 0);
  I2C_Unlock();
  return ok;
}
static bool rtcWriteReg(uint8_t reg, uint8_t v) { return rtcWriteBlock(reg, &v, 1); }

// 把 RTC 七个寄存器解成时间；返回 false 表示不可信（字段越界 = 断电后没设过时）
static bool rtcDecode(const uint8_t *b, int &sec, int &min, int &hour,
                      int &day, int &mon, int &year) {
  sec  = bcd2bin(b[0] & 0x7F);
  min  = bcd2bin(b[1] & 0x7F);
  hour = bcd2bin(b[2] & 0x3F);          // 24 小时制（写的时候 bit6 也留 0）
  day  = bcd2bin(b[3] & 0x3F);
  mon  = bcd2bin(b[5] & 0x1F);
  year = 2000 + bcd2bin(b[6]);
  if (year < 2024 || year > 2099) return false;   // 断电重置后年份是垃圾值
  if (mon < 1 || mon > 12) return false;
  if (day < 1 || day > daysInMonth(year, mon)) return false;
  if (hour > 23 || min > 59 || sec > 59) return false;
  return true;
}

// 开机调用：探测 + 读时间。读到合法时间就覆盖软时钟。
static bool rtcBegin() {
  uint8_t b[7];
  if (!rtcReadBlock(b, 7)) {
    gRtcFound = gRtcValid = false;
    Serial.println("[RTC] PCF85063 @0x51 not found -> time falls back to build/SNTP");
    return false;
  }
  gRtcFound = true;
  // 本器件没有 OS 掉电标志，靠年份/月日范围判断是否可信

  int s, mi, h, d, mo, y;
  if (!rtcDecode(b, s, mi, h, d, mo, y)) {
    gRtcValid = false;
    Serial.printf("[RTC] time NOT usable (year=%02X mon=%02X day=%02X) -> will set from SNTP/manual\n",
                  b[6], b[5], b[3]);
    return false;
  }
  gSec = s; gMin = mi; gHour = h; gDay = d; gMon = mo; gYear = y;
  gWday = weekdayOf(y, mo, d);
  gClockSynced = true;
  gTimeSrc = "RTC";
  gRtcValid = true;
  snprintf(gBootTimeSrc, sizeof(gBootTimeSrc), "RTC");
  Serial.printf("[RTC] restored from PCF85063: %04d-%02d-%02d %02d:%02d:%02d wday=%d\n",
                gYear, gMon, gDay, gHour, gMin, gSec, gWday);
  return true;
}

// 主循环把当前时间写进 RTC（0x04..0x0A 七个 BCD 字节）。
static bool rtcWriteClock() {
  if (!gRtcFound) return false;
  uint8_t b[7];
  b[0] = bin2bcd((uint8_t)gSec);
  b[1] = bin2bcd((uint8_t)gMin);
  b[2] = bin2bcd((uint8_t)gHour);      // 24 小时制
  b[3] = bin2bcd((uint8_t)gDay);
  b[4] = (uint8_t)(gWday & 0x07);
  b[5] = bin2bcd((uint8_t)gMon);
  b[6] = bin2bcd((uint8_t)(gYear % 100));
  bool ok = rtcWriteBlock(RTC_REG_SEC, b, 7);
  if (ok) gRtcValid = true;
  gRtcLastWrite = millis();
  Serial.printf("[RTC] write %s -> %04d-%02d-%02d %02d:%02d:%02d\n",
                ok ? "ok" : "FAILED", gYear, gMon, gDay, gHour, gMin, gSec);
  return ok;
}

// 触摸任务里顺带跑：
// ① 有待写回的时间就写 RTC + 存 NVS；② 断网时每 10 min 用 RTC 校正软时钟漂移；
// ③ 每 30 min 把当前时间存一次 NVS（RTC 没电池时，断电重启也能给出接近的时间）
#define RTC_SOFT_SYNC_MS   (10UL * 60UL * 1000UL)
#define CLOCK_PERSIST_MS   (30UL * 60UL * 1000UL)
static uint32_t gClockPersistMs = 0;
static void rtcService() {
  if (gRtcDirty) {
    gRtcDirty = false;
    if (gRtcFound) rtcWriteClock();
    persistClock();
    gClockPersistMs = millis();
  }
  uint32_t now = millis();
  if (now - gClockPersistMs >= CLOCK_PERSIST_MS) {
    gClockPersistMs = now;
    persistClock();
  }
  if (now - gRtcLastSync < RTC_SOFT_SYNC_MS) return;
  gRtcLastSync = now;
  uint8_t b[7];
  if (!rtcReadBlock(b, 7)) { gRtcFound = false; gRtcValid = false; return; }
  gRtcFound = true;
  int s, mi, h, d, mo, y;
  if (!rtcDecode(b, s, mi, h, d, mo, y)) return;
  gRtcValid = true;
  if (y == gYear && mo == gMon && d == gDay) {
    int rtcSod = h * 3600 + mi * 60 + s;
    int sod    = gHour * 3600 + gMin * 60 + gSec;
    int diff   = rtcSod - sod;
    if (diff >  12 * 3600) diff -= 24 * 3600;
    if (diff < -12 * 3600) diff += 24 * 3600;
    if (diff >= 2 || diff <= -2) {       // 软时钟跑快了/慢了，按 RTC 兜回来
      gHour = h; gMin = mi; gSec = s;
      clockSetSystem();
      gTimeSrc = "RTC";
      Serial.printf("[RTC] soft clock drift %+ds -> corrected from RTC\n", diff);
    }
  } else if (gYear != y || gMon != mo || gDay != d) {
    gYear = y; gMon = mo; gDay = d; gWday = weekdayOf(y, mo, d);
    gHour = h; gMin = mi; gSec = s;
    clockSetSystem();
    gTimeSrc = "RTC";
    Serial.printf("[RTC] date mismatch -> resync %04d-%02d-%02d %02d:%02d:%02d\n",
                  gYear, gMon, gDay, gHour, gMin, gSec);
  }
}
static const char *ROMAN[] = {"XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI"};

// ---------------------------------------------------------------- 天气
enum WxIcon : uint8_t { WX_CLEAR = 0, WX_PARTLY, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM };
struct Weather {
  int   temp, lo, hi;
  char  desc[16];
  WxIcon icon;
  bool  live;
  uint32_t stamp;
  // 显式构造：C++11 下带默认成员初始化器的 struct 不是聚合类型，
  // 不能用 Weather{...} 初始化静态表
  Weather(int t = 16, int l = 15, int h = 19, const char *d = "阴",
          WxIcon ic = WX_CLOUD, bool lv = false, uint32_t st = 0)
    : temp(t), lo(l), hi(h), icon(ic), live(lv), stamp(st) {
    strncpy(desc, d, 15); desc[15] = 0;
  }
};
static Weather wx;

static void mapSkycon(const char *sky, Weather &w) {
  char s[32]; strncpy(s, sky, 31); s[31] = 0;
  for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
  if      (strstr(s, "CLEAR_DAY") || strstr(s, "CLEAR_NIGHT")) { w.icon = WX_CLEAR;  strcpy(w.desc, "晴"); }
  else if (strstr(s, "PARTLY"))                                { w.icon = WX_PARTLY; strcpy(w.desc, "多云"); }
  else if (strstr(s, "STORM") || strstr(s, "THUNDER"))         { w.icon = WX_STORM;  strcpy(w.desc, "雷雨"); }
  else if (strstr(s, "SNOW"))                                  { w.icon = WX_SNOW;   strcpy(w.desc, "雪"); }
  else if (strstr(s, "RAIN"))                                  { w.icon = WX_RAIN;   strcpy(w.desc, "雨"); }
  else if (strstr(s, "FOG") || strstr(s, "HAZE"))              { w.icon = WX_FOG;    strcpy(w.desc, "雾"); }
  else if (strstr(s, "CLOUDY"))                                { w.icon = WX_CLOUD;  strcpy(w.desc, "阴"); }
  else if (strstr(s, "WIND"))                                  { w.icon = WX_PARTLY; strcpy(w.desc, "风"); }
  else if (strstr(s, "DUST"))                                  { w.icon = WX_FOG;    strcpy(w.desc, "沙尘"); }
  else                                                         { w.icon = WX_CLOUD;  strcpy(w.desc, "阴"); }
}

// v2.2：表盘上的天气描述跟着表盘语言走。只按 icon 取字，不再依赖云端返回的原始 skycon 文案
// （mapSkycon/mapWmo 里写进 wx.desc 的中文继续留给串口日志）。
// 顺序必须与 enum WxIcon 一致：CLEAR / PARTLY / CLOUD / FOG / RAIN / SNOW / STORM。
static const char *const WX_DESC_CN[] = {"晴", "多云", "阴", "雾", "雨", "雪", "雷雨"};
static const char *const WX_DESC_EN[] = {"CLEAR", "PARTLY", "CLOUDY", "FOG", "RAIN", "SNOW", "STORM"};
static inline const char *wxDesc() {
  int i = (int)wx.icon;
  if (i < 0 || i > 6) i = 2;
  return gLang ? WX_DESC_EN[i] : WX_DESC_CN[i];
}

// 轻量 JSON：从 p 开始的 limit 字节内找 "key":value
static bool jStr(const char *p, const char *key, char *out, int cap, int limit = 200000) {
  char pat[40]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *q = strstr(p, pat);
  if (!q || q - p > limit) return false;
  q = strchr(q, ':'); if (!q) return false;
  q++; while (*q == ' ') q++;
  if (*q != '"') return false;
  q++;
  int i = 0;
  while (*q && *q != '"' && i < cap - 1) out[i++] = *q++;
  out[i] = 0;
  return i > 0;
}
static float jNum(const char *p, const char *key, float def, int limit = 200000) {
  char pat[40]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *q = strstr(p, pat);
  if (!q || q - p > limit) return def;
  q = strchr(q, ':'); if (!q) return def;
  q++; while (*q == ' ') q++;
  if (*q == '"') return def;            // "ground" 之类的字符串值
  return atof(q);
}

// ---------------------------------------------------------------- 航班
struct Plane {
  char  call[12];
  float r;            // 0..1 归一化半径
  float bearing;      // 方位角
  int   track;
  float alt_m;
  int   gs;
  char  type[8];
  bool  demo;
  bool  stale;
};
static Plane planes[RADAR_MAX_PLANES];
static Plane lastLive[RADAR_MAX_PLANES];   // 最近一次成功解析出的真实目标快照
static int planeCount = 0;
static int liveCount = 0;
static int lastLiveCount = 0;              // >0 表示这台设备已经真正联网成功过
static int radiusNm = RADAR_RADIUS_NM;
static const int RANGE_OPTIONS[] = RADAR_RANGES_NM;
static int rangeIdx = 2;
static char radarSrc[16] = "ADSB.LOL";

// v0.16：雷达只画真实 ADS-B 目标，演示飞机表已彻底删除。
// 之前开机预填 8 条固定呼号（CSZ9917 / CCA1831 ...），没网或请求失败时也会补满，
// 用户完全分不清屏上哪些是真航班。现在：请求失败就恢复上一次真实快照并标 STALE，
// 从没成功过就显示空雷达 + NO SIGNAL，绝不伪造目标。
static uint32_t lastAdsbOkMs = 0;          // 最近一次成功解析真实数据的时间

static void clearPlanes() {
  planeCount = liveCount = 0;
  Serial.println("[RADAR] no live contacts -> empty scope (no demo targets)");
}
static void markPlanesStale() {
  for (int i = 0; i < liveCount; i++) planes[i].stale = true;
}
static void adsbFailed(const char *why) {
  if (lastLiveCount > 0) {
    memcpy(planes, lastLive, (size_t)lastLiveCount * sizeof(Plane));
    liveCount = planeCount = lastLiveCount;
    markPlanesStale();
    Serial.printf("[ADSB] %s -> restore %d last-live contacts (stale)\n", why, lastLiveCount);
  } else {
    clearPlanes();
    Serial.printf("[ADSB] %s -> NO SIGNAL\n", why);
  }
}

static void parseAdsb(const char *json) {
  const char *p = json;
  int n = 0;
  while ((p = strstr(p, "\"flight\"")) && n < RADAR_MAX_PLANES) {
    char call[12] = {0};
    if (!jStr(p, "flight", call, sizeof(call), 40) || !call[0]) { p += 8; continue; }
    // 一条 ac 记录大致在 600 字节内
    float lat  = jNum(p, "lat", 0, 700);
    float lon  = jNum(p, "lon", 0, 700);
    float gs   = jNum(p, "gs", 0, 700);
    float trk  = jNum(p, "track", 0, 700);
    float altF = jNum(p, "alt_baro", -1, 700);
    char  type[8] = "----";
    jStr(p, "t", type, sizeof(type), 700);
    if (lat == 0 || lon == 0) { p += 8; continue; }
    // 经纬度差 * 111320 / 110540 得到的是【米】，必须先 /1000 才是公里。
    // 旧版漏了这一步，distNm 被放大 1000 倍，所有目标都因 r>1 被过滤 -> live=0。
    float dx = (lon - gHomeLon) * 111320.0f * cosf(gHomeLat * PI / 180.0f);
    float dy = (lat - gHomeLat) * 110540.0f;
    float distKm = sqrtf(dx * dx + dy * dy) / 1000.0f;
    float distNm = distKm / 1.852f;
    float r = distNm / (float)radiusNm;
    if (r < 0.03f || r > 1.0f) { p += 8; continue; }
    float bearing = atan2f(dx, dy) * 180.0f / PI;
    if (bearing < 0) bearing += 360.0f;
    Plane &a = planes[n];
    memset(&a, 0, sizeof(a));
    strncpy(a.call, call, 11);
    strncpy(a.type, type, 7);
    a.r = r; a.bearing = bearing; a.track = (int)trk;
    a.alt_m = altF > 0 ? altF * 0.3048f : 0;
    a.gs = (int)(gs * 1.852f);
    a.demo = false; a.stale = false;
    n++;
    p += 8;
  }
  if (n > 0) {
    lastLiveCount = n;
    lastAdsbOkMs = millis();
    memcpy(lastLive, planes, (size_t)n * sizeof(Plane));
  } else if (lastLiveCount > 0) {
    // 解析出 0 架（截断/异常响应）：恢复真实快照并标 STALE，不放假目标
    memcpy(planes, lastLive, (size_t)lastLiveCount * sizeof(Plane));
    liveCount = planeCount = lastLiveCount;
    markPlanesStale();
    Serial.printf("[ADSB] empty parse -> restore %d last-live (stale)\n", lastLiveCount);
    return;
  }
  // v0.16：真实目标有几架画几架，不再用 DEMO_PLANES 补满屏幕
  liveCount = planeCount = n;
  Serial.printf("[ADSB] LIVE=%d radius=%dnm city=%s src=adsb.lol\n",
                liveCount, radiusNm, CITIES[gCityIdx].name);
}

// ---------------------------------------------------------------- 网络任务（Core 0）
std::atomic<bool> wifiOK{false};                  // 非 static：audio_player.h 里 extern 复用
static std::atomic<bool> netBusy{false};
static std::atomic<uint32_t> lastWx{0}, lastAdsb{0};
// v0.21：Wi-Fi 断开原因快照。core 3.0.7 的 WiFiClass 只提供「给定 reason 码 → 名字」的
// disconnectReasonName()，取不到当前原因，只能自己挂事件回调把最后一次 reason 记下来。
// 排障时这一项直接区分「扫不到 SSID(201)」和「密码错(15/204)」——之前只能看个 OFFLINE 干瞪眼。
static volatile uint8_t  gWifiLastReason = 0;
static volatile uint32_t gWifiDiscCount  = 0;
static volatile bool     gNetHold = false;   // v0.21：手动扫描期间暂停后台重连，避免互相打断
static void onWifiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    gWifiLastReason = info.wifi_sta_disconnected.reason;
    gWifiDiscCount++;
  }
}

static char wxSrc[16] = "DEMO";     // CAIYUN / OPENMETEO / DEMO，界面状态栏显示

// 从 "[12.8, 13.1]" 这样的数组里取第一个数（Open-Meteo daily 字段）
static float jArrFirst(const char *p, const char *key, float def, int limit = 20000) {
  char pat[48]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *q = strstr(p, pat);
  if (!q || q - p > limit) return def;
  q = strchr(q, ':'); if (!q) return def;
  q++; while (*q == ' ' || *q == '[') q++;
  if (*q != '-' && *q != '+' && (*q < '0' || *q > '9')) return def;
  return atof(q);
}

// WMO weather code -> 图标 / 中文（Open-Meteo 用）
static void mapWmo(int c, Weather &w) {
  if      (c == 0)                          { w.icon = WX_CLEAR;  strcpy(w.desc, "晴"); }
  else if (c <= 2)                          { w.icon = WX_PARTLY; strcpy(w.desc, "多云"); }
  else if (c == 3)                          { w.icon = WX_CLOUD;  strcpy(w.desc, "阴"); }
  else if (c == 45 || c == 48)              { w.icon = WX_FOG;    strcpy(w.desc, "雾"); }
  else if (c >= 95)                         { w.icon = WX_STORM;  strcpy(w.desc, "雷雨"); }
  else if ((c >= 71 && c <= 77) || c == 85 || c == 86)
                                            { w.icon = WX_SNOW;   strcpy(w.desc, "雪"); }
  else if (c >= 51)                         { w.icon = WX_RAIN;   strcpy(w.desc, "雨"); }
  else                                      { w.icon = WX_CLOUD;  strcpy(w.desc, "阴"); }
}

// 主源：彩云天气 v2.6（需要用户自己的 token；官方 demo token 实测 429）
static bool fetchCaiyun(const String &token) {
  WiFiClientSecure cli;
  cloudUseCaBundle(cli);
  cli.setHandshakeTimeout(8);   // TLS 握手最多 8s，避免网络异常时把网络任务永久锁死
  HTTPClient http;
  http.setConnectTimeout(6000);
  char url[220];
  snprintf(url, sizeof(url),
           "https://api.caiyunapp.com/v2.6/%s/%.5f,%.5f/weather?dailysteps=1&hourlysteps=1",
           token.c_str(), gHomeLon, gHomeLat);
  if (!http.begin(cli, url)) { Serial.println("[WX] caiyun begin failed"); return false; }
  http.setTimeout(ADSB_HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", "esp32-round-clock/1.0");
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    BoundedHttpBody body(32768);
    if (!body.receive(http)) { http.end(); return false; }
    const char *s = body.c_str();
    char sky[32] = {0};
    float t = jNum(s, "temperature", 16, 6000);
    const char *daily = strstr(s, "\"daily\"");
    float lo = daily ? jNum(daily, "min", 15, 4000) : 15;
    float hi = daily ? jNum(daily, "max", 19, 4000) : 19;
    jStr(s, "skycon", sky, sizeof(sky), 6000);
    Weather nw;
    nw.temp = (int)roundf(t); nw.lo = (int)roundf(lo); nw.hi = (int)roundf(hi);
    mapSkycon(sky, nw);
    nw.live = true; nw.stamp = millis();
    wx = nw;
    strcpy(wxSrc, "CAIYUN");
    Serial.printf("[WX] CAIYUN %dC %s %d/%d skycon=%s\n", wx.temp, wx.desc, wx.lo, wx.hi, sky);
    ok = true;
  } else {
    Serial.printf("[WX] caiyun HTTP %d -> fallback Open-Meteo\n", code);
  }
  http.end();
  return ok;
}

// 兜底源：Open-Meteo，免密钥、实测约 500B / 1.9s。没 token 也能显示真实天气。
static bool fetchOpenMeteo() {
  WiFiClientSecure cli;
  cloudUseCaBundle(cli);
  cli.setHandshakeTimeout(8);   // TLS 握手最多 8s，避免网络异常时把网络任务永久锁死
  HTTPClient http;
  http.setConnectTimeout(6000);
  char url[260];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
           "&current=temperature_2m,weather_code"
           "&daily=temperature_2m_max,temperature_2m_min"
           "&timezone=Asia%%2FShanghai&forecast_days=1", gHomeLat, gHomeLon);
  if (!http.begin(cli, url)) { Serial.println("[WX] open-meteo begin failed"); return false; }
  http.setTimeout(ADSB_HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", "esp32-round-clock/1.0");
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    BoundedHttpBody body(32768);
    if (!body.receive(http)) { http.end(); return false; }
    const char *s = body.c_str();
    const char *cur   = strstr(s, "\"current\"");
    const char *daily = strstr(s, "\"daily\"");
    float t   = cur   ? jNum(cur, "temperature_2m", 16, 400) : 16;
    int   wmo = cur   ? (int)jNum(cur, "weather_code", 3, 400) : 3;
    float hi  = daily ? jArrFirst(daily, "temperature_2m_max", t + 3, 600) : t + 3;
    float lo  = daily ? jArrFirst(daily, "temperature_2m_min", t - 3, 600) : t - 3;
    Weather nw;
    nw.temp = (int)roundf(t); nw.hi = (int)roundf(hi); nw.lo = (int)roundf(lo);
    mapWmo(wmo, nw);
    nw.live = true; nw.stamp = millis();
    wx = nw;
    strcpy(wxSrc, "OPENMETEO");
    Serial.printf("[WX] OPENMETEO %dC %s %d/%d wmo=%d (%d bytes)\n",
                  wx.temp, wx.desc, wx.lo, wx.hi, wmo, body.length());
    ok = true;
  } else {
    Serial.printf("[WX] open-meteo HTTP %d\n", code);
  }
  http.end();
  return ok;
}

// 第二兜底：MET Norway（挪威气象局）公开 API，免密钥、UTF-8、覆盖全球。
// 加它的原因：api.open-meteo.com 在某些网络环境下整段不通（实测 51.81.167.44 /
// 188.40.99.226 都超时），而 api.met.no 同一时刻 1.8 s 就返回了。
// 它的天气用英文 symbol_code（clearsky_day / partlycloudy_night / rain …），
// 映射到中文图标不用处理 GBK，比国内那些返回 GBK HTML 的源干净得多。
static void mapMetSymbol(const char *sym, Weather &w) {
  char s[40]; strncpy(s, sym, 39); s[39] = 0;
  for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
  if      (strstr(s, "thunder"))                      { w.icon = WX_STORM;  strcpy(w.desc, "雷雨"); }
  else if (strstr(s, "sleet"))                        { w.icon = WX_RAIN;   strcpy(w.desc, "雨夹雪"); }
  else if (strstr(s, "snow"))                         { w.icon = WX_SNOW;   strcpy(w.desc, "雪"); }
  else if (strstr(s, "rain") || strstr(s, "shower"))  { w.icon = WX_RAIN;   strcpy(w.desc, "雨"); }
  else if (strstr(s, "fog"))                          { w.icon = WX_FOG;    strcpy(w.desc, "雾"); }
  else if (strstr(s, "partlycloudy"))                 { w.icon = WX_PARTLY; strcpy(w.desc, "多云"); }
  else if (strstr(s, "cloudy"))                       { w.icon = WX_CLOUD;  strcpy(w.desc, "阴"); }
  else if (strstr(s, "clear") || strstr(s, "fair"))   { w.icon = WX_CLEAR;  strcpy(w.desc, "晴"); }
  else                                                { w.icon = WX_CLOUD;  strcpy(w.desc, "阴"); }
}

static bool fetchMetNo() {
  WiFiClientSecure cli;
  cloudUseCaBundle(cli);
  cli.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(6000);
  char url[180];
  snprintf(url, sizeof(url),
           "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%.4f&lon=%.4f",
           gHomeLat, gHomeLon);
  if (!http.begin(cli, url)) { Serial.println("[WX] met.no begin failed"); return false; }
  http.setTimeout(ADSB_HTTP_TIMEOUT_MS);
  // met.no 硬性要求带可识别的 User-Agent，不设会 403
  http.addHeader("User-Agent", "esp32-round-clock/1.0 (waveshare-esp32s3)");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[WX] met.no HTTP %d\n", code);
    http.end();
    return false;
  }
  BoundedHttpBody body(65536);
  const bool received = body.receive(http);
  http.end();
  if (!received) return false;
  const char *s = body.c_str();
  const char *ts = strstr(s, "\"timeseries\"");
  if (!ts) { Serial.println("[WX] met.no parse: no timeseries"); return false; }

  // 当前温度 = 第一条 timeseries 的 air_temperature
  float t = jNum(ts, "air_temperature", 16, 1200);
  // 未来 24 条里取 min/max 当高低温
  float lo = t, hi = t;
  const char *p = ts;
  for (int i = 0; i < 24; i++) {
    p = strstr(p, "air_temperature");
    if (!p) break;
    const char *c = strchr(p, ':');
    if (!c) break;
    float v = atof(c + 1);
    if (v < -60 || v > 60) break;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    p = c + 1;
  }
  // 天气符号：第一条 symbol_code
  char sym[40] = {0};
  const char *sc = strstr(ts, "\"symbol_code\"");
  if (sc) {
    sc = strchr(sc, ':');
    if (sc) {
      sc++;
      while (*sc == ' ' || *sc == '"') sc++;
      int i = 0;
      while (*sc && *sc != '"' && i < 39) sym[i++] = *sc++;
      sym[i] = 0;
    }
  }
  Weather nw;
  nw.temp = (int)roundf(t);
  nw.lo = (int)roundf(lo);
  nw.hi = (int)roundf(hi);
  mapMetSymbol(sym, nw);
  nw.live = true; nw.stamp = millis();
  wx = nw;
  strcpy(wxSrc, "METNO");
  Serial.printf("[WX] METNO %dC %s %d/%d sym=%s (%d bytes)\n",
                wx.temp, wx.desc, wx.lo, wx.hi, sym, body.length());
  return true;
}

static void fetchWeather() {
  if (!wifiOK) { wx.live = false; strcpy(wxSrc, "DEMO"); return; }
  bool ok = false;
  String token;
  {
    Preferences prefs;
    if (prefs.begin("rcfg", true)) token = prefs.getString("token", CAIYUN_TOKEN);
  }
  if (token.length()) ok = fetchCaiyun(token);
  if (!ok) ok = fetchOpenMeteo();
  if (!ok) ok = fetchMetNo();          // open-meteo 在部分网络整段不通，靠这条兜底
  if (!ok) { wx.live = false; strcpy(wxSrc, "DEMO"); }  // 保留上次数值，界面不空白
}

static void fetchAdsb() {
  WiFiClientSecure cli;
  cloudUseCaBundle(cli);
  cli.setHandshakeTimeout(8);   // TLS 握手最多 8s，避免网络异常时把网络任务永久锁死
  HTTPClient http;
  http.setConnectTimeout(6000);
  char url[160];
  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.5f/%.5f/%d",
           gHomeLat, gHomeLon, radiusNm);
  if (!http.begin(cli, url)) { adsbFailed("begin failed"); return; }
  http.setTimeout(ADSB_HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", "esp32-round-clock/1.0");
  int code = http.GET();
  Serial.printf("[ADSB] HTTP %d\n", code);
  if (code == 200) {
    const int CAP = 64 * 1024;
    char *buf = (char *)ps_malloc(CAP + 1);
    if (buf) {
      WiFiClient *st = http.getStreamPtr();
      int len = 0; uint32_t t0 = millis();
      // 读 body 单独给 15s：实测上海 250nm 约 28KB，8s 会截掉末尾几架
      while (len < CAP && millis() - t0 < 15000) {
        int av = st->available();
        if (av > 0) {
          int take = min(av, CAP - len);
          int got = st->readBytes(buf + len, take);
          if (got <= 0) break;
          len += got;
        } else if (!http.connected()) break;
        else delay(2);
      }
      buf[len] = 0;
      Serial.printf("[ADSB] read %d bytes\n", len);
      if (len < 100) adsbFailed("empty body");
      else           parseAdsb(buf);
      free(buf);
    } else {
      adsbFailed("out of memory");
    }
  } else {
    char m[24]; snprintf(m, sizeof(m), "HTTP %d", code);
    adsbFailed(m);
  }
  http.end();
}

#define NET_WIFI_RETRY_MS  30000UL    // 一直连不上时，每隔多久主动重连（原 15s 会打断进行中的握手）
#define NET_RETRY_ADSB_MS  20000UL    // 航班没拿到数据时的重试间隔
#define NET_RETRY_WX_MS    15000UL    // 天气没拿到数据时的重试间隔

// Wi-Fi 连线控制由主循环推进，语音/HTTPS 阻塞时仍可检测断线并重连。
static void netPollConnection() {
  static uint32_t lastPoll = 0;
  static uint32_t lastRetry = 0;
  static uint32_t resumeAt = 0;
  static int retry = 0;
  static uint8_t reconnectPhase = 0;
  const uint32_t now = millis();
  if (now - lastPoll < 100) return;
  lastPoll = now;

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (wifiOK.exchange(connected) != connected) {
    Serial.printf("[NET] wifi %s%s\n", connected ? "UP " : "DOWN",
                  connected ? WiFi.localIP().toString().c_str() : "");
    if (connected) {
      configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.windows.com");
      lastWx.store(0);
      lastAdsb.store(0);
      retry = 0;
    } else {
      lastRetry = now;
    }
  }
  if (connected || gProvActive || gNetHold || curPage == P_WIFI || !gCfg.ssid.length()) {
    reconnectPhase = 0;
    return;
  }

  if (!reconnectPhase) {
    if (now - lastRetry < NET_WIFI_RETRY_MS) return;
    lastRetry = now;
    Serial.printf("[NET] wifi retry #%d reason=%u %s\n", ++retry, (unsigned)gWifiLastReason,
                  WiFi.disconnectReasonName((wifi_err_reason_t)gWifiLastReason));
    WiFi.disconnect(true, false);
    resumeAt = now + 200;
    reconnectPhase = 1;
  } else if ((int32_t)(now - resumeAt) >= 0) {
    if (reconnectPhase == 1) {
      WiFi.mode(WIFI_STA);
      resumeAt = now + 100;
      reconnectPhase = 2;
    } else {
      WiFi.setSleep(false);
      WiFi.begin(gCfg.ssid.c_str(), gCfg.pass.c_str());
      reconnectPhase = 0;
    }
  }
}

// 串行执行固件自己的 HTTPS 和语音请求，避免同时申请两份 TLS 缓冲。
static void netTask(void *) {
  bool preferWeather = false;
  for (;;) {
    if (!cloudPollJobs() && wifiOK.load() && !netBusy.load()) {
      const uint32_t now = millis();
      const uint32_t wxAt = lastWx.load(), adsbAt = lastAdsb.load();
      const bool wxDue = !wxAt || now - wxAt >= (wx.live ? WX_REFRESH_MS : NET_RETRY_WX_MS);
      const bool adsbDue = !adsbAt || now - adsbAt >= (lastLiveCount ? ADSB_REFRESH_MS : NET_RETRY_ADSB_MS);
      if (wxDue && (!adsbDue || preferWeather)) {
        lastWx.store(now);
        fetchWeather();
        preferWeather = false;
      } else if (adsbDue) {
        lastAdsb.store(now);
        fetchAdsb();
        preferWeather = true;
      }
    }
    delay(100);
  }
}

// ---------------------------------------------------------------- 番茄钟
enum PomoPhase : uint8_t { PH_FOCUS = 0, PH_SHORT, PH_LONG };
struct Pomo {
  PomoPhase phase = PH_FOCUS;
  bool running = false;
  uint32_t remainMs = POMO_FOCUS_MIN * 60000UL;
  uint32_t lastMs = 0;
  int rounds = 0;
  bool alarm = false;
  uint32_t alarmMs = 0;
};
static Pomo pomo;
// v0.17：专注时长三档（15 / 25 / 45 分钟），顶部小胶囊选择，存 NVS(rcfg/pomofocus)。
static int gPomoFocusMin = POMO_FOCUS_MIN;
static const int POMO_PRESETS[] = {15, 25, 45};
#define POMO_PRESET_COUNT ((int)(sizeof(POMO_PRESETS) / sizeof(POMO_PRESETS[0])))
static uint32_t pomoPhaseMs() {
  if (pomo.phase == PH_FOCUS) return (uint32_t)gPomoFocusMin * 60000UL;
  if (pomo.phase == PH_SHORT) return POMO_SHORT_BREAK_MIN * 60000UL;
  return POMO_LONG_BREAK_MIN * 60000UL;
}
static const char *pomoName() {
  if (pomo.phase == PH_FOCUS) return "专注中";
  if (pomo.phase == PH_SHORT) return "短休息";
  return "长休息";
}
static void pomoNext() {
  if (pomo.phase == PH_FOCUS) {
    pomo.rounds++;
    pomo.phase = (pomo.rounds % POMO_LONG_BREAK_EVERY == 0) ? PH_LONG : PH_SHORT;
  } else {
    pomo.phase = PH_FOCUS;
  }
  pomo.remainMs = pomoPhaseMs();
  pomo.running = false;
}
static void pomoTick() {
  uint32_t now = millis();
  uint32_t dt = now - pomo.lastMs;
  pomo.lastMs = now;
  if (!pomo.running) return;
  if (dt >= pomo.remainMs) {
    pomo.remainMs = 0; pomo.running = false;
    pomo.alarm = true; pomo.alarmMs = now;
    pomoNext();
  } else pomo.remainMs -= dt;
}

// ---------------------------------------------------------------- 音乐状态
// v0.14：三个音源（电台 / TF 卡 / 播客）共用这一页，顶部 tab 切换，底部 LIST 开曲库浮层。
static bool mPlaying = false;                 // 默认不自动连流，按中心键才开始播
static float vinylAngle = 0;
static float trackProgress = 0.38f;
static bool  gMusicList = false;              // 曲库浮层开关
static int   gMusicPage = 0;                 // 曲库每页 5 行
#define MUSIC_LIST_ROWS 5
#define MUSIC_LIST_Y0   84
#define MUSIC_LIST_ROWH 38

// ---------------------------------------------------------------- 天气图标
static void drawWxIcon(int cx, int cy, int sz, WxIcon ic, uint16_t col) {
  int r = sz / 2;
  uint16_t sun = C_YELLOW;
  auto cloud = [&](int oy, uint16_t c) {
    fillCircle(cx - r / 3, cy + oy, r / 3, c);
    fillCircle(cx + r / 5, cy + oy - r / 6, r * 2 / 5, c);
    fillCircle(cx + r / 2, cy + oy + r / 8, r / 4, c);
    fillRect(cx - r / 2, cy + oy + r / 8, r, r / 3, c);
  };
  switch (ic) {
    case WX_CLEAR:
      fillCircle(cx, cy, r * 0.55f, sun);
      for (int a = 0; a < 360; a += 45) {
        int x1, y1, x2, y2;
        polar(cx, cy, r * 0.72f, a, x1, y1);
        polar(cx, cy, r * 0.98f, a, x2, y2);
        line(x1, y1, x2, y2, sun, 2);
      }
      break;
    case WX_PARTLY:
      fillCircle(cx - r / 4, cy - r / 3, r * 0.45f, sun);
      cloud(r / 6, col);
      break;
    case WX_CLOUD: cloud(0, col); break;
    case WX_FOG:
      cloud(-r / 5, col);
      line(cx - r, cy + r / 2, cx + r, cy + r / 2, col, 2);
      line(cx - r * 2 / 3, cy + r * 3 / 4, cx + r * 2 / 3, cy + r * 3 / 4, col, 2);
      break;
    case WX_RAIN:
      cloud(-r / 5, col);
      for (int i = -1; i <= 1; i++)
        line(cx + i * r / 2, cy + r / 3, cx + i * r / 2 - r / 8, cy + r, C_BLUE, 2);
      break;
    case WX_SNOW:
      cloud(-r / 5, col);
      for (int i = -1; i <= 1; i++) fillCircle(cx + i * r / 2, cy + r * 3 / 4, 2, COL(159, 212, 245));
      break;
    case WX_STORM:
      cloud(-r / 4, col);
      fillTri(cx + 2, cy, cx - r / 2, cy + r * 3 / 4, cx + r / 8, cy + r * 3 / 4, sun);
      fillTri(cx - 2, cy + r / 2, cx + r / 2, cy + r, cx - r / 8, cy + r, sun);
      break;
  }
}

// ---------------------------------------------------------------- 通用装饰
// v2.0：底部那排页码点全部删除。页面不再是环形序列（表盘 <-> 应用中心 <-> 应用），
// 页码点既没有意义、15 个点也排不进圆里。
static bool hit(int x, int y, int cx, int cy, int r) {
  long dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= (long)r * r;
}

// v0.15 起屏幕虚拟锁键已移除（息屏只用实体 BOOT 键），对应绘制函数一并删除。
// 锁屏 = 真·息屏：先把帧缓冲整屏刷黑（关背光后不留残影），再把背光降到 0
static void lockScreen() {
  if (gLocked) return;
  gLocked = true;
  clear(0x0000);                 // 全黑一帧
  flushScreen();
  Set_Backlight(0);              // 背光彻底关掉 = 息屏
  // v0.17：锁屏只关显示，绝不碰音频。播放跑在 Core0 的独立 audioTask 上，
  // 电台 / MUSIC(TF) / 播客三种音源都会继续播（这就是"音乐页项目支持锁屏播放"）。
  Serial.printf("[UI] lock -> screen OFF (audio=%s)\n",
                gAudioState == A_PLAYING ? "still playing" : "idle");
}

// 解锁 = 亮屏：恢复用户设定亮度并重绘当前页（v0.17 起只由实体 BOOT 键触发）
static void wakeScreen() {
  if (!gLocked) return;
  gLocked = false;
  Set_Backlight((uint8_t)gBright);
  Serial.println("[UI] wake -> screen ON");
  showToast("UNLOCKED");
  render();
}

// 屏幕开关统一入口：实体键 / 屏幕锁键 / 串口 LOCK 都走这里
static void toggleLockScreen() {
  if (gLocked) wakeScreen(); else lockScreen();
}

// 实体 BOOT 键（GPIO0）轮询：按下沿切换 息屏 / 亮屏，带 25ms 消抖。
// 只在按下那一刻动作（长按不连发），松手不做任何事。
static void pollButton() {
  static uint8_t lastRead = HIGH, stable = HIGH;
  static uint32_t tChange = 0;
  uint8_t r = (uint8_t)digitalRead(BTN_PIN);
  uint32_t now = millis();
  if (r != lastRead) { lastRead = r; tChange = now; return; }
  if (now - tChange < BTN_DEBOUNCE_MS) return;    // 抖动窗口内先不动
  if (r == stable) return;
  stable = r;
  if (stable == LOW) {                            // 按下沿
    Serial.printf("[UI] btn BOOT -> %s\n", gLocked ? "wake" : "lock");
    toggleLockScreen();
  }
}

// v0.18：电量改成「弧形分段」—— 6 格沿 6 点天气副盘的圆边弯着排，凸向下，
// 弧心就是副盘圆心，所以格子天然贴合圆屏，不再是横平竖直的方块条。
// 点亮格数按 SOC 四舍五入；充电整排转黄；只剩最后 1 格（<=15%）转红。
// 数据仍来自板载 BQ27220（gBat），读不到芯片（没焊电池 / USB 供电）时如实显示 USB。
static void drawBatteryArc(int cx, int cy) {
  const int   N     = 6;      // 6 格：够分辨，又不至于碎成点
  const int   R     = 36;     // 弧半径（副盘 r=44，外侧留 ~4px，不贴边）
  const int   TH    = 4;      // 弧厚：细一点更像"仪表线"，粗了就成方块
  const int   PITCH = 13;     // 每格占的角度（共 6*13 = 78°，左右对称压在 6 点）
  const int   A0    = 180 - (N * PITCH) / 2;                  // 141°
  const float GAPF  = 0.10f;  // 格两端各让 0.10 格 ≈ 1.6px 缝

  bool avail = (gBatFakeSoc >= 0) || gBat.valid;
  int  soc   = (gBatFakeSoc >= 0) ? gBatFakeSoc : gBat.soc;
  bool chg   = (gBatFakeSoc >= 0) ? (gBatFakeChg > 0) : gBat.charging;
  if (!avail) {
    drawText(cx, cy + 26, "USB", COL(120, 132, 142), 12, 1, 1);
    return;
  }
  int lit = (soc * N + 50) / 100;                             // 四舍五入到格
  if (lit < 0) lit = 0;
  if (lit > N) lit = N;
  bool low = (!chg && soc <= 15);
  if (low && lit < 1) lit = 1;                 // 快没电时也留一格红，不要整条黑掉

  uint16_t on  = chg ? COL(248, 206, 84)                      // 充电：琥珀黄
                : (low ? COL(224, 76, 64)                      // 只剩最后一格：红
                       : COL(96, 200, 128));                   // 平常：绿
  uint16_t off = COL(46, 54, 64);              // 未点亮：暗板岩

  // 逐像素判定「环形扇区」：格宽由角度精确控制，两端是直的径向切口。
  // 不能用 arcRing 一格一格描 —— 极坐标描边会在格子两端啃出锯齿、宽窄不一。
  const float rIn = R - TH / 2.0f - 0.3f, rOut = R + TH / 2.0f + 0.3f;
  const float in2 = rIn * rIn, out2 = rOut * rOut;
  const int   bx0 = cx - (int)rOut - 1, bx1 = cx + (int)rOut + 1;
  const int   by0 = cy + 18,            by1 = cy + (int)rOut + 1;
  for (int y = by0; y <= by1; y++) {
    for (int x = bx0; x <= bx1; x++) {
      float dx = x - cx, dy = y - cy;
      float d2 = dx * dx + dy * dy;
      if (d2 < in2 || d2 > out2) continue;
      // atan2 的 0° 在正东，arcRing/polar 的 0° 在正北 —— +90 对齐
      float pos = (atan2f(dy, dx) * 57.29578f + 90.0f - A0) / PITCH;
      if (pos < 0.0f || pos >= N) continue;
      int   i = (int)pos;
      float f = pos - i;
      if (f < GAPF || f > 1.0f - GAPF) continue;               // 格间缝
      // pos 从右（141°）往左（219°）递增，翻转一下才是"从左往右填"的常规电量条
      setPixel(x, y, (i >= N - lit) ? on : off);
    }
  }
}

// 临时提示条（导航边界 / 启动教学）
static void drawToast() {
  if (millis() > gToastUntil || !gToast[0]) return;
  fillRoundRect(30, 330, 300, 20, 8, COL(20, 26, 32));
  // v0.16：改 16px，让中文提示（例如 "CITY / 上海"）也能显示，12px 字库只有 ASCII
  drawBtnText(180, 330, 20, gToast, C_WHITE, 16);
}

// ================================================================ 页 1 机械表盘
static void renderAnalog() {
  clear(C_DARK);
  // 表盘底：分层色块（RGB565 不用大面积渐变）
  fillCircle(180, 180, 172, COL(17, 26, 33));
  circle(180, 180, 168, C_SLATE, 2);
  circle(180, 180, 160, COL(35, 44, 52), 1);
  // 刻度
  for (int i = 0; i < 60; i++) {
    bool major = i % 5 == 0;
    int x1, y1, x2, y2;
    polar(180, 180, major ? 140 : 150, i * 6, x1, y1);
    polar(180, 180, 157, i * 6, x2, y2);
    line(x1, y1, x2, y2, major ? C_CREAM : COL(109, 120, 130), major ? 2 : 1);
  }
  // 罗马数字（跳过 3 点日期窗、6 点天气副盘；v0.24 起 9 点星期窗也占位，一并跳过 —— 否则 IX 紧贴窗边）
  for (int i = 0; i < 12; i++) {
    if (i == 3 || i == 6 || i == 9) continue;
    int x, y; polar(180, 180, 124, i * 30, x, y);
    drawText(x, y - 8, ROMAN[i], C_CREAM, 12, 1, 1);
  }
  // 品牌标识：v0.22 换成花体（Vladimir Script → 4-bit AA 位图，见 font_aero.h）。
  // 保持**原始细线**，不做任何加粗（加粗版在屏上又粗又钝，被否）。
  // 墨迹中心固定 y=110，随字高自适应，换字号不用改这里。
  drawAeroLogo(180, 110 - AERO_LOGO_H / 2, C_CREAM);

  // v0.17：电量条已并入 6 点天气小圈（见下方 drawBatteryCells），这里保持干净。

  // ── 双窗（v0.24 起，参考 A. Lange & Söhne Datograph）──
  // 深墨底 + 暖银细边 + 内凹暗框，字用衬线 AA 字模（font_dial.h），
  // 与罗马数字刻度、花体 AERO 同属衬线体系。
  // v0.25 两处修正（用户反馈「字母太大、数字太小」「数字字体太现代」）：
  //   ① 字体 Baskerville → **Palatino Bold**。Baskerville 是大写字母宽、数字细的
  //      transitional 体，字母撑满窗、数字反而发虚，看着像数码字。
  //      Palatino Bold 笔画粗、衬线宽厚，是文艺复兴手写体一脉，压得住深底窗。
  //   ② 数字 cap 22 → **27**（放大并加重），字母 cap 22 → **18**（收回小一号）。
  //      字母宽度 = 2.4×字高，同高时 "MO" 要 56px，被迫把窗撑到 68 —— 盘面被压。
  //      现在是 60x38（内宽 56），比 v0.24 窄 8px，盘面呼吸回来了。
  //   ③ 数字与字母**各自在窗内垂直居中**（不再共用基线）：字母底 y+28、数字底 y+32。
  //   ④ 窗框改成立体金属倒角（`drawDialWindow()`，gfx.cpp）：三圈同心边做出
  //      "外棱受光 → 金属面 → 槽壁阴影"的斜面，窗外右下 1px 投影。
  //   ⑤ 数字走**等宽槽**（每字 21px，"16" = 41px），字母标称宽再横向压 12%
  //      （"MO" = 43px）。这是让两窗视觉重量对齐的关键 —— 只调字高永远对不平，
  //      大写字母天然比数字宽 40%，宽度差才是"不平衡"的主因。窗因此收到 58 宽。
  //   9 点罗马数字仍跳过 —— 和 3 点对称，两窗把那个位置占了。
  const int WIN_W = 58, WIN_H = 40, WIN_Y = 160;   // 160..200，垂直中心 180
  drawDialWindow(260 - WIN_W / 2, WIN_Y, WIN_W, WIN_H);
  char dbuf[4]; snprintf(dbuf, sizeof(dbuf), "%d", gDay);
  drawDialText(260, WIN_Y + 32, dbuf, COL(238, 232, 214));   // 数字 cap24，等宽槽

  drawDialWindow(100 - WIN_W / 2, WIN_Y, WIN_W, WIN_H);
  // v2.2：语言开关**只管数字表盘**（用户明确「模拟表盘不要改」）。
  // 这里固定走衬线两字母 WDAY_2 —— 中文单字在深底窗里会退化成一条横线，两字母才撑得住窗面。
  drawDialText(100, WIN_Y + 30, WDAY_2[gWday], COL(238, 232, 214));  // 字母 cap21

  // 6 点天气副盘：图标 / AA 温度 / 16px 中文 / 电量小格，四行都收在 r=44 圆内
  // 先画副盘、后画指针 —— 指针压在最上层，扫过 6 点位置时不会被温度数字盖住
  fillCircle(180, 248, 44, C_DARK);
  circle(180, 248, 44, COL(74, 85, 96), 2);
  drawWxIcon(180, 215, 18, wx.icon, COL(180, 192, 200));
  char tbuf[10]; snprintf(tbuf, sizeof(tbuf), "%d", wx.temp);
  uint16_t warm = COL(242, 230, 207);
  drawMidText(180, 224, tbuf, warm, 1);
  drawDegree(180 + aaTextWidth(tbuf, false) / 2 + 3, 231, warm, 2);
  drawText(180, 258, wx.desc, COL(168, 180, 187), 16, 1, 1);
  // v0.18：电量做成沿副盘圆边弯着的 6 格弧（凸向下，贴着小圈底部走）
  drawBatteryArc(180, 248);

  // 指针（在天气副盘之上，保证任何角度都压住副盘不被遮）
  float hAng = (gHour % 12 + gMin / 60.0f) * 30.0f;
  float mAng = (gMin + gSec / 60.0f) * 6.0f;
  float sAng = gSec * 6.0f;
  int hx, hy, mx, my, sx, sy, tx, ty;
  polar(180, 180, 84, hAng, hx, hy); polar(180, 180, 18, hAng + 180, tx, ty);
  line(tx, ty, hx, hy, C_CREAM, 5);
  polar(180, 180, 108, mAng, mx, my); polar(180, 180, 20, mAng + 180, tx, ty);
  line(tx, ty, mx, my, COL(230, 220, 200), 3);
  polar(180, 180, 118, sAng, sx, sy); polar(180, 180, 22, sAng + 180, tx, ty);
  line(tx, ty, sx, sy, C_ORANGE, 1);
  fillCircle(180, 180, 5, C_CREAM);
  fillCircle(180, 180, 2, C_DARK);

  // v0.22：底部那排 "WX / CAIYUN / LIVE" 小字已删除（用户嫌破坏表盘观感）。
  // 数据源真伪在天气副盘里看得到（DEMO 时副盘温度本身是演示值），配网状态由 P_WIFI 页负责。
}

// ---------------------------------------------------------------- 数字表盘右上角小电池（v2.2）
// gfx 只有填充版 roundRect，所以用"外层填框色 + 内层填纸色"做出 2px 描边壳，
// 再按 soc 比例填内胆。配色与模拟盘 6 点副盘的弧形电量一致：
//   充电 琥珀黄 / ≤15% 红 / 平常 绿。没电池（只有 USB）就画灰壳空腔，不假装 0%。
static int battSocVal() {
  if (gBatFakeSoc >= 0) return gBatFakeSoc;
  return gBat.valid ? gBat.soc : -1;
}
// v2.2：数字表盘电量 = 4 个小方块（用户否掉了电池壳造型 —— 太小做不出"壳+极片+液面"）。
// 竖排在天气图标左侧，从下往上点亮：25%/格。配色沿用全机一套（≤15% 红 / 平常绿）。
// 充电：**流水灯**（用户要求，去掉原来那个小闪电）—— 一格亮琥珀从底往上跑，1.6s 一圈；
//   已达电量的格留淡琥珀底、未达的留淡灰空槽，于是"在充"和"充到哪"两个信息同时读得出。
static void drawBatteryBlocks(int x, int y) {
  const int N = 4, BW = 9, BH = 6, GAP = 2;
  int  soc0 = battSocVal();
  bool chg  = (gBatFakeSoc >= 0) ? (gBatFakeChg > 0) : gBat.charging;
  int  soc  = soc0 < 0 ? 0 : (soc0 > 100 ? 100 : soc0);
  bool low  = (soc0 >= 0 && !chg && soc <= 15);

  uint16_t on  = low ? COL(220, 60, 48) : COL(66, 178, 108);
  uint16_t off = COL(210, 205, 192);          // 未点亮：比纸色深一档，像"空槽"

  int lit = (soc0 < 0) ? 0 : (soc * N + 50) / 100;
  if (soc0 >= 0 && soc > 0 && lit < 1) lit = 1;

  // 充电流水：700ms 一格（用户："速度慢一点"，400 → 700）→ 2.8s 一圈。
  // 表盘 4fps 刷新，每格大约停在屏上 2~3 帧，光点是"上爬"而不是"闪"。
  const uint16_t c_hot = COL(240, 168, 40);   // 当前跑动的光点
  const uint16_t c_dim = COL(250, 214, 168);  // 已达电量的格（淡琥珀底）
  const uint16_t c_off = COL(226, 220, 204);  // 未达电量的空槽（淡灰）
  int phase = (int)((millis() / 700) % N);    // 0 = 最下面那格

  for (int i = 0; i < N; i++) {                // i=0 是最上面那格
    int idx = N - 1 - i;                       // 0 = 最下面那格
    int by  = y + i * (BH + GAP);
    uint16_t col;
    if (chg) col = (idx == phase) ? c_hot : (idx < lit ? c_dim : c_off);
    else     col = (idx < lit) ? on : off;
    fillRoundRect(x, by, BW, BH, 1, col);      // r=1：9×6 用 r=2 会圆成小胶囊
  }
}

// ================================================================ 主界面 · 数字表盘
// v2.1（用户要求）：秒的**数字去掉**，大时间挪到**正中心**。
//   秒没有丢——外圈那圈橙色圆弧本身就是秒针（0..60 秒绕满一圈），
//   一个两位数挤在冒号下面既抢戏又和中间的"重心"打架。
// 版式（360 圆，安全区 r≤158，上下两块重心距圆心各 88~89，视觉居中）：
//   星期 y=62 / 日期 y=92（24px，各叠绘一遍加粗）
//   大时间 148..208（AA 墨迹 57 高，中心 = 180）
//   分隔线 y=226 / 天气块 240..292
static void renderDigital() {
  clear(C_PAPER);
  // v0.27：整圈外扩 158/150 → **172/164**。省下来的边全部换成字号：星期/日期 16 → 24px，
  // 天气描述 16 → 24px，高低温 12 → 16px，天气图标 36 → 48。
  circle(180, 180, 172, COL(216, 212, 200));
  circle(180, 180, 164, COL(222, 217, 204), 5);
  arcRing(180, 180, 164, 0, (int)(360.0f * gSec / 60.0f), 5, C_ORANGE);   // 外弧 = 秒
  // 顶部星期 / 日期：24px 点阵在纸色底上偏细，各叠绘一遍做加粗（沿用 v0.22 手法）
  uint16_t dcol = COL(141, 136, 122);
  // v2.2：星期随表盘语言（中文「星期三」/ 英文 "WEDNESDAY"）。日期保持数字，两种语言通用。
  char wdbuf[16];                  // 「星期」+ 单字 = 10 字节 UTF-8，别按字面宽度开 8
  if (gLang) {
    drawText(180, 62, WDAY_EN[gWday], dcol, 24, 1, 1);
    drawText(181, 62, WDAY_EN[gWday], dcol, 24, 1, 1);
  } else {
    snprintf(wdbuf, sizeof(wdbuf), "星期%s", WDAY_CN[gWday]);
    drawText(180, 62, wdbuf, dcol, 24, 1, 1);
    drawText(181, 62, wdbuf, dcol, 24, 1, 1);
  }
  // 完整年月日来自 SNTP；未对时则用软时钟继续走，不再硬编码 2026-11
  char dbuf[24]; snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", gYear, gMon, gDay);
  drawText(180, 92, dbuf, dcol, 24, 1, 1);
  drawText(181, 92, dbuf, dcol, 24, 1, 1);

  // 天气块左侧 4 格小方块电量（v2.2）：9×6 方块 + 2px 间隙，总高 30，(82,250)..(91,280)
  // —— 竖中心 265 略高于天气图标中心 268（用户第三次反馈"电池还要往上一点"）。
  // 充电走流水灯，不再画闪电。外圈 r=172/164 完全没动。
  drawBatteryBlocks(82, 250);

  // 大时间：4-bit AA 数字，冒号墨迹中心精确落在 x=180；纵向墨迹中心落在 y=180
  // v2.2：用户反馈两次"时间再往上一点" —— 行框 148 → 142 → **138**（AA 墨迹 57 高）
  char tbuf[8]; snprintf(tbuf, sizeof(tbuf), "%02d:%02d", gHour, gMin);
  drawBigText(180, 138, tbuf, C_INK);
  line(112, 226, 248, 226, COL(200, 195, 180));

  // 天气：图标 48 / AA 加粗温度 / 24px 中文描述；高低温挂到温度**正下方**成一组
  // （原来挂在描述下面，画面成了 L 形，重心偏右下）
  drawWxIcon(134, 268, 48, wx.icon, COL(107, 123, 134));
  char wbuf[10]; snprintf(wbuf, sizeof(wbuf), "%d", wx.temp);
  drawMidTextBold(174, 244, wbuf, C_INK, 0);
  drawDegree(174 + aaTextWidth(wbuf, false) + 3, 251, C_INK, 3);
  char rbuf[12]; snprintf(rbuf, sizeof(rbuf), "%d/%d", wx.lo, wx.hi);
  drawText(174, 282, rbuf, COL(141, 136, 122), 16, 1, 0);
  drawDegree(174 + textWidth(rbuf, 16, 1) + 2, 288, COL(141, 136, 122), 2);
  // v2.2：描述起点 232 → 226。英文最长的 "PARTLY"/"CLOUDY" 是 6 字 ×12px = 72 宽，
  // 从 232 起右端到 304，已经贴到秒环内缘（y=250 处环在 x≈314）；左移 6px 留出安全余量。
  drawText(226, 250, wxDesc(), COL(58, 74, 68), 24, 1, 0);
}

// ================================================================ 页 3 音乐（三音源 + 黑胶转动）
// 曲库浮层：复用圆屏安全区布局（行 x 60..300，底部按钮 x 96..174 / 186..264，y 288..310）
static void renderMusicList() {
  clear(C_DARK2);
  int src = (int)audioSource();
  int n   = audioCount(src);
  char ttl[40], disp[40];
  snprintf(ttl, sizeof(ttl), "%s / %d TRACKS", AUD_SRC_NAME[src], n);
  fitText(disp, sizeof(disp), ttl, 12, 150);
  drawText(180, 40, disp, C_WHITE, 12, 1, 1);

  if (n == 0) {
    const char *hint = src == AS_TF ? "NO SD CARD / TAP SCAN" :
                       src == AS_POD ? "NO EPISODES / TAP FEED" : "NO STATION";
    drawText(180, 160, hint, C_GREY, 12, 1, 1);
  }
  int pages = (n + MUSIC_LIST_ROWS - 1) / MUSIC_LIST_ROWS;
  if (pages < 1) pages = 1;
  if (gMusicPage >= pages) gMusicPage = 0;
  for (int r = 0; r < MUSIC_LIST_ROWS; r++) {
    int idx = gMusicPage * MUSIC_LIST_ROWS + r;
    if (idx >= n) break;
    int y = MUSIC_LIST_Y0 + r * MUSIC_LIST_ROWH;
    bool cur = (idx == audioIndex() && gAudioState == A_PLAYING);
    fillRoundRect(60, y, 240, 32, 6, cur ? C_ORANGE_D : C_SLATE);
    char nm[80];
    // v0.16：中文歌名必须用 16px（FONT12 只有 ASCII，12px 会把汉字静默跳过）
    fitText(nm, sizeof(nm), audioTrackName(src, idx), 16, 240 - 24 - 20);
    drawBtnText(72, y, 32, nm, cur ? C_YELLOW : C_WHITE, 16, 1, 0);
    char num[8]; snprintf(num, sizeof(num), "%d", idx + 1);
    drawBtnText(288, y, 32, num, C_GREY, 12, 1, 2);
  }
  char pg[16]; snprintf(pg, sizeof(pg), "%d/%d", gMusicPage + 1, pages);
  drawText(180, 274, pg, C_GREY, 12, 1, 1);
  // v2.8：曲库浮层不再有"返回"键 —— 左右滑退出、上下滑翻页（见 pollTouch）
  fillRoundRect(141, 288, 78, 22, 8, C_ORANGE_D);
  drawBtnText(180, 288, 22, "更多", C_WHITE, 16);
}

static void renderMusic() {
  if (gMusicList) { renderMusicList(); return; }
  clear(C_VINYL);
  // 整屏黑胶：同心纹路 + 随角度转动的高光扇区
  fillCircle(180, 180, 176, COL(16, 20, 27));
  for (int r = 60; r <= 168; r += 7)
    circle(180, 180, r, (r % 14 == 0) ? COL(38, 43, 51) : COL(28, 33, 41), 1);
  for (int i = 0; i < 3; i++) {
    int a0 = (int)vinylAngle + i * 120;
    arcRing(180, 180, 120, a0, a0 + 26, 30, COL(30, 38, 50));
    arcRing(180, 180, 152, a0 + 60, a0 + 78, 12, COL(28, 35, 46));
  }
  int src = (int)audioSource();
  int n   = audioCount(src);
  uint32_t dur = audioDuration();
  float prog = dur ? (float)audioElapsed() / (float)dur : trackProgress;
  if (prog > 1) prog = 1;
  circle(180, 180, 170, COL(44, 53, 72));
  arcRing(180, 180, 170, 0, (int)(300.0f * prog), 3, C_ORANGE);

  // ── v0.27 控制区全圆化 ──────────────────────────────────────────
  // 圆屏里塞一排圆角矩形，边角会自己跟弧线打架。五个功能键全部改**圆形**，
  // 并且按同心圆排布：音源三颗骑在 r=136 的弧上（正上 + 左右各 24°），
  // 底部曲库/切台在 r=124 的弧上（左右各 30°），中央播放键 r=52 做视觉焦点。
  //   音源 r=23（外缘最远 158.7，基本贴安全区），曲名让到 y=86
  //   底部 r=25，中心 (118,288)/(242,288)，内缘离副信息 3px
  static const int TABX[3] = {125, 180, 235};
  static const int TABY[3] = {56, 44, 56};
  static const char *const TAB_CN[3] = {"电台", "音乐", "播客"};
  for (int i = 0; i < 3; i++) {
    bool on = (i == src);
    fillCircle(TABX[i], TABY[i], 23, on ? C_ORANGE_D : COL(34, 43, 59));
    circle(TABX[i], TABY[i], 23, on ? C_ORANGE : COL(58, 69, 96), 2);
    // drawBtnText 的 boxY 是垂直居中的盒子顶；字高 16 → 盒顶 = 圆心 - 8
    drawBtnText(TABX[i], TABY[i] - 8, 16, TAB_CN[i],
                on ? C_YELLOW : COL(176, 190, 210), 16);
  }

  const char *st;
  if      (gAudioState == A_PLAYING)    st = "PLAYING";
  else if (gAudioState == A_CONNECTING) st = "CONNECTING";
  else if (gAudioState == A_PAUSED)     st = "PAUSED";
  else if (gAudioState == A_ERROR)      st = "ERROR";
  else                                  st = (wifiOK ? "READY" : "OFFLINE");
  char nm[40], sub[56], subFit[56];
  // v0.27：曲名从 y=76 下移到 86 —— 左右两颗音源圆（r=23，底边 y=79）会压过来
  fitText(nm, sizeof(nm), audioCurrentName(), 16, 250);
  drawText(180, 90, nm, C_WHITE, 16, 1, 1);
  if (dur) snprintf(sub, sizeof(sub), "%s %d/%d %02u:%02u/%02u:%02u", st,
                    audioIndex() + 1, n, (unsigned)(audioElapsed() / 60), (unsigned)(audioElapsed() % 60),
                    (unsigned)(dur / 60), (unsigned)(dur % 60));
  else     snprintf(sub, sizeof(sub), "%s %s %d/%d", st, AUD_SRC_NAME[src], audioIndex() + 1, n);
  fitText(subFit, sizeof(subFit), sub, 12, 240);
  // 副信息（状态 + 序号 + 时长）挪到播放键**下方**：上方已被三颗音源圆占满，
  // 夹在曲名和播放键之间只剩 4px。底下 y=248..260 这段正好空着。
  drawText(180, 250, subFit, COL(160, 176, 198), 12, 1, 1);

  // 中心唱片标签 + 超大播放键（视觉焦点）
  fillCircle(180, 180, 64, COL(20, 24, 31));
  circle(180, 180, 64, COL(58, 66, 80), 2);
  bool playingNow = (gAudioState == A_PLAYING);
  fillCircle(180, 180, 52, playingNow ? C_ORANGE : COL(60, 70, 88));
  if (playingNow) {
    fillRect(164, 163, 10, 34, C_WHITE);
    fillRect(186, 163, 10, 34, C_WHITE);
  } else {
    fillTri(168, 160, 168, 200, 202, 180, C_WHITE);
  }

  // 左右切曲键（r=33，圆心外移到 x=72/288，仍在 r≤158 内）
  fillCircle(72, 180, 33, COL(34, 43, 59));
  circle(72, 180, 33, COL(58, 69, 96));
  fillTri(82, 168, 82, 192, 64, 180, C_WHITE);
  fillRect(62, 168, 4, 24, C_WHITE);
  fillCircle(288, 180, 33, COL(34, 43, 59));
  circle(288, 180, 33, COL(58, 69, 96));
  fillTri(278, 168, 278, 192, 296, 180, C_WHITE);
  fillRect(294, 168, 4, 24, C_WHITE);

  // 底部两键：曲库 / 源相关的维护动作（重扫卡 / 抓 feed / 切台）
  // v0.27：82x30 圆角矩形 → **r=25 圆**，圆心落在 r=124 的弧上（左右各 30°）
  fillCircle(118, 292, 25, COL(34, 43, 59));
  circle(118, 292, 25, COL(58, 69, 96), 2);
  drawBtnText(118, 284, 16, "曲库", C_WHITE, 16);
  fillCircle(242, 292, 25, C_ORANGE_D);
  circle(242, 292, 25, C_ORANGE, 2);
  // ⚠️ 必须显式传 16：drawBtnText 默认 font=12，而 FONT12 只有 ASCII，
  // 中文会静默画不出来（按钮变成空块）。
  drawBtnText(242, 284, 16, src == AS_TF ? "重扫" : src == AS_POD ? "更新" : "切台", C_WHITE, 16);
}

// ================================================================ 页 4 航班雷达
static void renderRadar() {
  // v0.26：整圈放大。旧版外框 r=156、扫描扇止于 150，四周空黑 24px；
  // 现在外框 r=172、刻度 163..171、扫描扇 160，飞机投影半径 150 → **158**，
  // 数据区基本顶到玻璃边（和音乐页同一档）。
  clear(C_RADAR_BG);
  // 扫描扇区
  int a0 = (int)((millis() / 28) % 360);
  for (int d = 0; d < 46; d += 2) {
    uint16_t c = mix(C_RADAR_BG, COL(60, 170, 110), (uint8_t)(d * 90 / 46));
    arcRing(180, 180, 160, a0 - 46 + d, a0 - 44 + d, 3, c);
  }
  int sx, sy; polar(180, 180, 160, a0, sx, sy);
  line(180, 180, sx, sy, COL(74, 222, 128), 1);

  circle(180, 180, 60, C_GREEN_D);
  circle(180, 180, 122, COL(29, 70, 54));
  circle(180, 180, 172, COL(43, 107, 78), 2);
  // 刻度：每 10° 一格、每 30° 加长加亮（旧版只有 12 个长刻度，最外圈发空）
  for (int a = 0; a < 360; a += 10) {
    bool major = (a % 30 == 0);
    int x1, y1, x2, y2;
    polar(180, 180, major ? 163 : 166, a, x1, y1);
    polar(180, 180, 171, a, x2, y2);
    line(x1, y1, x2, y2, major ? COL(52, 128, 94) : COL(32, 78, 58), major ? 2 : 1);
  }
  // 竖向十字：上端从 y=70 起（顶部留给标题/状态），下端止于 y=276（S 字母底片的顶边）。
  // 旧版一路画到 336，会从「41秒前」的「秒」字正中穿过去，还会插进页码点中间。
  line(180, 70, 180, 276, COL(29, 70, 54));
  line(22, 180, 338, 180, COL(29, 70, 54));
  for (int a = 0; a < 360; a += 30) {
    if (a % 90 == 0) {
      // 方位字母 r=90 → 104（跟着整圈放大）。仍给每个字母垫同背景色底片，
      // 把十字中线截断，字面才干净。
      int bx, by; polar(180, 180, 104, a, bx, by);
      fillRoundRect(bx - 8, by - 8, 16, 16, 4, C_RADAR_BG);
      const char *n = a == 0 ? "N" : a == 90 ? "E" : a == 180 ? "S" : "W";
      drawText(bx, by - 6, n, COL(75, 156, 114), 12, 1, 1);
    }
  }
  // 本机
  fillRect(175, 175, 10, 10, COL(233, 255, 242));
  circle(180, 180, 14, C_GREEN, 1);

  // 飞机：标签做保留区 + 两两避让（圆屏同屏只放得下 5~6 个呼号）
  int labelX[12], labelY[12], labelN = 0;
  for (int i = 0; i < planeCount; i++) {
    Plane &p = planes[i];
    int px, py; polar(180, 180, (int)(p.r * 158), p.bearing, px, py);
    uint16_t c = p.stale ? C_YELLOW : C_GREEN;   // v0.16：只有 LIVE / STALE 两种真实状态
    // 局部三角形尖端在 (0,-9)（屏幕正上方=北），track 0=北顺时针；
    // 所以旋转角就是 track 本身。旧代码减 90°，画出来整体逆时针偏 90°
    // （向东飞的画成朝北），README 写的"三角朝向=航向"与真机不符，v0.16b 修正。
    float ar = p.track * PI / 180.0f;
    float cr = cosf(ar), sr = sinf(ar);
    auto rot = [&](float ox, float oy, int &oxr, int &oyr) {
      oxr = px + (int)(ox * cr - oy * sr);
      oyr = py + (int)(ox * sr + oy * cr);
    };
    int ax, ay, bx, by, cx2, cy2;
    rot(0, -9, ax, ay); rot(6, 7, bx, by); rot(-6, 7, cx2, cy2);
    fillTri(ax, ay, bx, by, cx2, cy2, c);
    // 真实数据常成簇（上海西侧航路），同屏最多 5 个呼号，多了只留图标
    if (detailIdx >= 0 || labelN >= 5) continue;

    int lx = px + 10, ly = py - 10;
    // 顶部 100px 留给标题/状态/半径胶囊（胶囊底边 y=96）；底部 268px 起留给 S 方位 / 提示 / 页码点
    if (ly < 100 || ly > 268) continue;
    // ⭐ 标签宽度按**真实呼号墨迹**算，别写死 64。写死时六七个字符的呼号也占 64px，
    // 贴圆边的目标一律被"放不下"丢掉（10 架只剩 0~2 个呼号）。
    int lw = textWidth(p.call, 12, 1);
    int half = (int)sqrtf((float)max(0, 164 * 164 - (ly - 180) * (ly - 180)));
    // 默认挂图标右侧；右侧放不下就翻到左侧（上海航路的目标常成簇挤在 E 方向）
    if (lx + lw > 180 + half - 6) lx = px - 10 - lw;
    // 标签盒不能压住中心本机标记
    if (lx < 204 && lx + lw > 156 && ly < 196 && ly + 12 > 164) continue;
    if (lx < 180 - half + 6 || lx + lw > 180 + half - 6) continue;
    // 迭代避让：反复下移直到不与任何已放标签冲突，5 次仍冲突就干脆不画
    bool placed = false;
    for (int attempt = 0; attempt < 5; attempt++) {
      bool clash = false;
      for (int k = 0; k < labelN; k++) {
        if (abs(lx - labelX[k]) < 72 && abs(ly - labelY[k]) < 18) {
          ly = labelY[k] + 18;
          clash = true;
          break;
        }
      }
      if (!clash) { placed = true; break; }
      if (ly > 268) break;
    }
    if (!placed) continue;
    int half2 = (int)sqrtf((float)max(0, 164 * 164 - (ly - 180) * (ly - 180)));
    if (lx < 180 - half2 + 6 || lx + lw > 180 + half2 - 6) continue;
    // 深色底片：飞机三角/扫描线压过来时呼号仍然可读（比墨迹宽 8）
    fillRoundRect(lx - 3, ly - 2, lw + 8, 16, 4, C_RADAR_BG);
    drawText(lx, ly, p.call, p.stale ? C_YELLOW : C_GREEN, 12, 1, 0);
    labelX[labelN] = lx; labelY[labelN] = ly; labelN++;
  }

  // 顶部标题 + **数据真伪状态**（v0.16）：一眼看出是实时 / 过期快照 / 没有信号
  // v0.26：整圈放大到 172 后，顶部两行必须跟着上移，否则会同时出现两个问题：
  //   ① 标题 y=38 离刻度环（9..17）空了 20px，字像是浮在半空；
  //   ② 状态行 y=54..66 正好被 r=122 内圈的顶部弧（顶点 y=58）从「10」和「/」中间穿过去。
  // 现在标题 28 / 状态 44：标题离刻度环 3px，状态行墨迹底 55 与内圈顶点 58 留 3px，
  // 两行都在圆弧之上，不再有任何线穿过文字。
  // 标题 / 状态各自垫一块同背景暗底片：扫描线和扫描扇每 14s 扫过顶部一次，
  // 不垫底片就会从「字缝」里穿过去（N/E/S/W 和飞机呼号早就是这么处理的）。
  // 底片比墨迹宽 6px、高 14/16px，刚好不碰 r=162 刻度环内缘（y=26 处 x≈130）。
  fillRect(126, 24, 108, 18, C_RADAR_BG);
  drawText(180, 28, "FLIGHT RADAR", C_GREEN, 12, 1, 1);
  bool anyStale = false;
  for (int i = 0; i < planeCount; i++) if (planes[i].stale) { anyStale = true; break; }
  unsigned long ageS = lastAdsbOkMs ? (millis() - lastAdsbOkMs) / 1000UL : 0;
  char sub[36];
  if      (planeCount == 0) snprintf(sub, sizeof(sub), "NO SIGNAL / %d KM", (int)(radiusNm * 1.852f));
  else if (anyStale)        snprintf(sub, sizeof(sub), "STALE %d / %lus AGO", planeCount, ageS);
  else                      snprintf(sub, sizeof(sub), "LIVE %d / ADSB.LOL", planeCount);
  uint16_t subCol = planeCount == 0 ? C_GREY : (anyStale ? C_YELLOW : COL(63, 138, 104));
  int subW = textWidth(sub, 12, 1);
  fillRect(180 - subW / 2 - 6, 41, subW + 12, 16, C_RADAR_BG);
  drawText(180, 44, sub, subCol, 12, 1, 1);

  // 半径档位：v0.26 64x26 大胶囊（触控面从 56x22 放大），压在圆内 (62,70)
  fillRoundRect(62, 70, 64, 26, 12, COL(16, 42, 32));
  char rb[12]; snprintf(rb, sizeof(rb), "%d KM", (int)(radiusNm * 1.852f));
  drawBtnText(94, 70, 26, rb, C_GREEN);

  // 详情卡
  if (detailIdx >= 0 && detailIdx < planeCount) {
    Plane &p = planes[detailIdx];
    fillRoundRect(82, 226, 196, 74, 9, COL(10, 28, 21));
    rect(82, 226, 196, 74, COL(43, 107, 78));
    drawText(94, 232, p.call, COL(198, 255, 224), 24, 1, 0);
    char l1[36]; snprintf(l1, sizeof(l1), "FL %d / %d km/h / %d",
                          (int)(p.alt_m * 3.28084f / 100), p.gs, p.track);
    drawText(94, 256, l1, COL(95, 174, 134), 12, 1, 0);
    char l2[36]; snprintf(l2, sizeof(l2), "%d km / %03d / %s",
                          (int)(p.r * radiusNm * 1.852f), (int)p.bearing,
                          p.stale ? "STALE" : "LIVE");
    drawText(94, 270, l2, COL(95, 174, 134), 12, 1, 0);
    drawText(94, 284, p.type, COL(63, 138, 104), 12, 1, 0);
  } else {
    // 只保留真正有意义的信息「数据多少秒前更新」，精简成几个字直接画在雷达圆内底部；
    // "TAP" 这类操作提示全部去掉（顶部 sub 行已经有 LIVE n / NO SIGNAL 的状态）。
    if (planeCount > 0) {
      // 数字和「秒前」必须分开画。FONT16 的 ASCII 步进是 10px、字形画布却是 16px，
      // 中文字形又是满 16px，所以 "%lu秒前" 整串交给 drawText 居中时，数字的 canvas 会撞进
      // 后面「秒」的格子 —— 一位数（5秒前）看不出来，两位数（16秒前）直接糊成一团。
      char num[8]; snprintf(num, sizeof(num), "%lu", ageS);
      const char *cn = "秒前";
      // 数字字模画布 16px、步进却只有 10px，末字会往右多探 6px，间距必须把这段补回来。
      int nw = textWidth(num, 16, 1), cw = textWidth(cn, 16, 1);
      int x0 = 180 - (nw + 6 + cw) / 2;
      drawText(x0, 298, num, COL(63, 138, 104), 16, 1, 0);
      drawText(x0 + nw + 6, 298, cn, COL(63, 138, 104), 16, 1, 0);
    }
  }
}

// ================================================================ 页 5 番茄钟
static void renderPomo() {
  // ── v0.26 放大整圈 / v0.27 外圈改倒计时环 ────────────────────────
  // 旧版盘面 r=168、进度环 r=148，一圈下来屏幕四周留了 30 多像素空黑，
  // 和音乐页（铺到 r=176）比明显"小一号"。现在底圆 **178**（贴到玻璃边），
  // 外圈 r=156..168 整条给倒计时环，金属边框 172 单独一圈。
  clear(COL(18, 21, 27));
  fillCircle(180, 180, 178, COL(19, 23, 30));
  circle(180, 180, 172, COL(52, 60, 76), 2);

  // ── v0.27 外圈改成倒计时环 ──────────────────────────────────────
  // v0.26 那圈 60 格刻度只是装饰，用户要求「外圈用于倒计时环」——把最外那圈
  // （r=156..168）整条让给进度，原来 r=152 的内环撤销。环走**剩余量**：
  // 满格起、走完归零，视觉上就是一圈往下掉，比"越填越满"更像倒计时。
  // 环带宽 12：内缘 156 不会碰顶部胶囊（y=46 处环带在 x 79..281），
  // 外缘 168 距金属边框 172 留 4px 黑缝，两圈不粘连。
  uint16_t phaseCol = pomo.phase == PH_FOCUS ? C_ORANGE : COL(74, 190, 140);
  uint32_t denom = pomoPhaseMs(); if (denom < 1) denom = 1;
  float remainRatio = (float)pomo.remainMs / (float)denom;
  if (remainRatio < 0.0f) remainRatio = 0.0f;
  if (remainRatio > 1.0f) remainRatio = 1.0f;
  circle(180, 180, 162, COL(38, 44, 56), 10);                       // 轨道
  if (remainRatio > 0.0f)
    arcRing(180, 180, 162, 0, (int)(360.0f * remainRatio), 10, phaseCol);
  // ⚠️ 别在环内侧再画三角做"起点指示"：y=141..150 那段正好压在 AA 大数字的
  // 冒号上（冒号墨迹中心就是 x=180），实机看像冒号被换成了一个橙色三角。
  // 满环从 12 点起、顺时针缩短，本身就是倒计时，不需要额外标记。
  // 内侧一圈极淡的分隔环，把"内容区"和"环"分层，避免中间显得空
  circle(180, 180, 148, COL(34, 40, 52), 1);

  // 顶部三档专注时长：v0.27 跟音乐页统一成**圆形按钮 + 拱形排布**。
  // 这页比音乐页难摆 —— 外圈 r=156..168 被倒计时环吃掉，可用半径只剩 156。
  // 横排三颗试过：外缘离环只剩 2.6px，实机看着像黏在环上（截图确认失败）。
  // 最终按音乐页那套沿弧排：圆心骑在 r=132 上、角度 -22°/0°/+22°，
  // 中心 (131,58)/(180,48)/(229,58)，圆心距 50/50，直径 38 留 12px 空隙。
  // 外缘 = 132+19 = 151，离环内缘 156 留 5px；圆底 77 < 相位名顶 81，不压字。
  static const int PRESETX[3] = {131, 180, 229};
  static const int PRESETY[3] = {58, 48, 58};
  for (int i = 0; i < POMO_PRESET_COUNT; i++) {
    bool on = (POMO_PRESETS[i] == gPomoFocusMin);
    fillCircle(PRESETX[i], PRESETY[i], 19, on ? C_ORANGE_D : COL(40, 48, 62));
    circle(PRESETX[i], PRESETY[i], 19, on ? C_ORANGE : COL(70, 80, 98), 2);
    char pb[6]; snprintf(pb, sizeof(pb), "%d", POMO_PRESETS[i]);
    drawBtnText(PRESETX[i], PRESETY[i] - 8, 16, pb, on ? C_YELLOW : COL(160, 172, 192), 16);
  }
  drawText(180, 81, pomoName(), phaseCol, 24, 1, 1);

  uint32_t totalSec = (pomo.remainMs + 999) / 1000;
  char tb[8]; snprintf(tb, sizeof(tb), "%02d:%02d", totalSec / 60, totalSec % 60);
  drawBigText(180, 108, tb, C_WHITE);          // AA 大数字，冒号对齐圆心
  // v0.26：「阶段结束」不再单独在底部占一行（会和页码点抢位置），
  // 直接顶掉状态行 —— 那一格里原本写的 RUNNING / READY / PAUSED 此刻已无意义。
  bool alarmOn = pomo.alarm && millis() - pomo.alarmMs < 4000;
  const char *stTxt = alarmOn ? "PHASE COMPLETE"
                    : pomo.running ? "RUNNING"
                    : (pomo.remainMs == pomoPhaseMs() ? "READY" : "PAUSED");
  drawText(180, 196, stTxt, alarmOn ? C_YELLOW : COL(139, 151, 169), 12, 1, 1);

  // 轮次点
  int done = pomo.rounds % POMO_LONG_BREAK_EVERY;
  for (int i = 0; i < POMO_LONG_BREAK_EVERY; i++)
    fillCircle(180 - (POMO_LONG_BREAK_EVERY - 1) * 9 + i * 18, 220, 4,
               i < done ? C_ORANGE : COL(70, 80, 96));

  // 三大控制键：v0.26 主键 32 → 33，两侧 23 → 25
  fillCircle(180, 264, 33, pomo.running ? COL(58, 68, 88) : C_ORANGE);
  if (pomo.running) {
    fillRect(168, 250, 9, 28, C_WHITE);
    fillRect(184, 250, 9, 28, C_WHITE);
  } else {
    fillTri(171, 249, 171, 279, 197, 264, C_WHITE);
  }
  fillCircle(102, 264, 25, COL(40, 48, 62));
  circle(102, 264, 25, COL(70, 80, 98));
  drawText(102, 256, "R", C_WHITE, 24, 1, 1);
  fillCircle(258, 264, 25, COL(40, 48, 62));
  circle(258, 264, 25, COL(70, 80, 98));
  drawText(258, 256, ">", C_WHITE, 24, 1, 1);

  // 底部 "START / RESET / SKIP" 操作提示已删除（三个键自己会说话）。
  // 页码点放在内容区底部（r≈132），别贴到刻度环上——y=322 时正好落进环带，看着像脏点
}

// ================================================================ 页 6 本机 Wi-Fi 配网
// 小键盘布局：3 行键格（字母 / 数字符号）+ 底部 3 个功能键（CANCEL / SPACE / JOIN）。
// 圆屏安全区 r<=158，键盘整体压在 y 122..238 的中段，四角都在圆内。
#define KX0 41
#define KW  26
#define KGX 2
#define KY0 142
#define KH  24
#define KGY 2
static const char *WIFI_KEYS_LET[4][10] = {
  {"Q","W","E","R","T","Y","U","I","O","P"},
  {"A","S","D","F","G","H","J","K","L","BKSP"},
  {"CAPS","Z","X","C","V","B","N","M","SYM",""},
  {"CANCEL","SPACE","JOIN","","","","","","",""},
};
static const char *WIFI_KEYS_SYM[4][10] = {
  {"1","2","3","4","5","6","7","8","9","0"},
  {"@","#","$","%","&","*","-","+","(",")"},
  {"SYM","!","\"","'","/","?",";",":",".",","},
  {"CANCEL","SPACE","JOIN","","","","","","",""},
};

// 左上角 Wi-Fi 图标：点 + 两段向上开口弧
static void drawWifiIcon(int cx, int cy, uint16_t col) {
  fillCircle(cx, cy, 2, col);
  arcRing(cx, cy, 7,  270, 450, 2, col);
  arcRing(cx, cy, 13, 270, 450, 2, col);
}

// ---------------------------------------------------------------- Wi-Fi 密码簿
// 输一次就记住（最多 5 个网络），下次点同名 SSID 密码自动填好，只需点 JOIN。
// 存在 NVS 键 "wbook"，格式：每行 "ssid\tpass"，最近用过的排最前。
#define WBOOK_MAX 5
static String gWifiBook = "";          // 密码簿正文
static bool   gWifiPassFromBook = false;   // 当前键盘页里的密码是不是从簿里带出来的

static void wifiBookLoad() {
  gWifiBook = cfgGetStr("wbook", "");
}

static bool wifiBookFind(const String &ssid, String &pass) {
  int p = 0;
  while (p < (int)gWifiBook.length()) {
    int nl = gWifiBook.indexOf('\n', p);
    if (nl < 0) nl = (int)gWifiBook.length();
    String line = gWifiBook.substring(p, nl);
    int tab = line.indexOf('\t');
    if (tab > 0 && line.substring(0, tab) == ssid) {
      pass = line.substring(tab + 1);
      return true;
    }
    p = nl + 1;
  }
  return false;
}

static void wifiBookPut(const String &ssid, const String &pass) {
  if (!ssid.length()) return;
  String newest = ssid + "\t" + pass + "\n";
  if (gWifiBook.startsWith(newest)) return;      // 已是最新一条，不必重写 flash
  String out = newest;                           // 新记录置顶
  int p = 0, n = 1;
  while (p < (int)gWifiBook.length() && n < WBOOK_MAX) {
    int nl = gWifiBook.indexOf('\n', p);
    if (nl < 0) nl = (int)gWifiBook.length();
    String line = gWifiBook.substring(p, nl);
    int tab = line.indexOf('\t');
    if (!(tab > 0 && line.substring(0, tab) == ssid)) { out += line; out += '\n'; n++; }
    p = nl + 1;
  }
  gWifiBook = out;
  cfgSet("wbook", gWifiBook);                   // 立即落盘，掉电不丢
  Serial.printf("[WIFI] pass remembered for '%s' (%d saved)\n", ssid.c_str(), n);
}

static void wifiBookDump() {
  int cnt = 0, p = 0;
  while (p < (int)gWifiBook.length()) {
    int nl = gWifiBook.indexOf('\n', p);
    if (nl < 0) nl = (int)gWifiBook.length();
    String line = gWifiBook.substring(p, nl);
    int tab = line.indexOf('\t');
    if (tab > 0) {
      cnt++;
      Serial.printf("  %d. %-24s pass=%d chars\n", cnt,
                    line.substring(0, tab).c_str(), (int)(line.length() - tab - 1));
    }
    p = nl + 1;
  }
  Serial.printf("[WIFI] %d network(s) with remembered password\n", cnt);
}

static void wifiScanStart() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.scanNetworks(true, true);       // 异步扫描，不阻塞 UI
  gWifiScanning = true; gWifiScanFailed = false; gWifiScanRetry = 0;
  gWifiScanRetryMs = millis(); gWifiN = 0;
}

static void enterWifiList() {
  curPage = P_WIFI;
  gWifiState = W_LIST;
  gWifiScroll = 0; gWifiSel = -1; gWifiPass = "";
  gWifiSym = false; gWifiCaps = false; gWifiPassFromBook = false;
  wifiBookLoad();
  wifiScanStart();
}

static void wifiScrollBy(int d) {
  if (gWifiScanning) return;
  int maxScroll = max(0, gWifiN - 6);
  gWifiScroll = constrain(gWifiScroll + d, 0, maxScroll);
}

// 返回按键动作：单字符；或 BKSP / CAPS / SYM / CANCEL / SPACE / JOIN；空串=未命中
static String wifiKeyHit(int x, int y) {
  if (y >= 220 && y <= 244) {                 // 功能键行
    if (x >= 41  && x <= 125) return "CANCEL";
    if (x >= 125 && x <= 265) return "SPACE";
    if (x >= 265 && x <= 321) return "JOIN";
    return "";
  }
  if (y < KY0 || y > 218) return "";
  int row = (y - KY0) / (KH + KGY);
  if (row < 0 || row > 2) return "";
  int col = (x - KX0) / (KW + KGX);
  if (col < 0 || col > 9) return "";
  int cellLeft = KX0 + col * (KW + KGX), cellTop = KY0 + row * (KH + KGY);
  if (x < cellLeft + 1 || x > cellLeft + KW - 1 || y < cellTop + 1 || y > cellTop + KH - 1) return "";
  const char *cell = gWifiSym ? WIFI_KEYS_SYM[row][col] : WIFI_KEYS_LET[row][col];
  if (!cell || cell[0] == 0) return "";
  return String(cell);
}

static void drawWifiKeyboard() {
  const char *(*keys)[10] = gWifiSym ? WIFI_KEYS_SYM : WIFI_KEYS_LET;
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 10; c++) {
      const char *lab = keys[r][c];
      if (!lab || lab[0] == 0) continue;
      int left = KX0 + c * (KW + KGX), top = KY0 + r * (KH + KGY);
      bool special = (strcmp(lab, "BKSP") == 0 || strcmp(lab, "CAPS") == 0 || strcmp(lab, "SYM") == 0);
      uint16_t bg = (special || strcmp(lab, "JOIN") == 0) ? COL(40, 48, 62) : C_SLATE;
      if (strcmp(lab, "CAPS") == 0 && gWifiCaps) bg = C_ORANGE_D;
      if (strcmp(lab, "JOIN") == 0) bg = C_ORANGE;
      fillRoundRect(left, top, KW, KH, 5, bg);
      const char *disp = lab;
      char up[2] = {0};
      if (!gWifiSym && !special && strlen(lab) == 1) {
        char ch = lab[0];
        if (gWifiCaps && ch >= 'a' && ch <= 'z') ch -= 32;
        up[0] = ch; disp = up;
      }
      drawBtnText(left + KW / 2, top, KH, disp, C_WHITE, special ? 12 : 16);
    }
  }
  fillRoundRect(41,  220, 84,  24, 6, COL(40, 48, 62));
  drawBtnText(41 + 42, 220, 24, "CANCEL", C_WHITE);
  fillRoundRect(125, 220, 140, 24, 6, COL(60, 70, 88));
  drawBtnText(125 + 70, 220, 24, "SPACE", C_WHITE);
  fillRoundRect(265, 220, 56,  24, 6, C_ORANGE);
  drawBtnText(265 + 28, 220, 24, "JOIN", C_WHITE);
}

// 按像素宽度截断字符串（超宽补 ".."），避免 SSID 顶出行右边界 / 撑出圆边
static void fitText(char *out, size_t outsz, const char *src, int font, int maxW) {
  if (outsz == 0) return;
  strncpy(out, src, outsz - 1); out[outsz - 1] = 0;
  if (textWidth(out, font, 1) <= maxW) return;
  const char *ell = "..";
  int ew = textWidth(ell, font, 1);
  size_t n = strlen(out);
  // v0.16：一次回退一个**完整 UTF-8 字符**，不能逐字节砍。
  // 中文是 3 字节，砍到一半会留下残缺序列，drawText 会把它画成 '?' 或空白。
  while (n > 0 && textWidth(out, font, 1) + ew > maxW) {
    n--;
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
    out[n] = 0;
  }
  if (n + 2 < outsz) strcat(out, ell); else out[0] = 0;
}

static void renderWifi() {
  // 异步扫描完成后读结果（避免阻塞主循环）
  if (gWifiState == W_LIST && gWifiScanning) {
    int done = WiFi.scanComplete();
    if (done >= 0) {
      gWifiN = min(done, 16);
      for (int i = 0; i < gWifiN; i++) {
        strncpy(gNetSSID[i], WiFi.SSID(i).c_str(), 32); gNetSSID[i][32] = 0;
        gNetRssi[i] = WiFi.RSSI(i);
        gNetEnc[i]  = (int)WiFi.encryptionType(i);
      }
      WiFi.scanDelete();
      gWifiScanning = false; gWifiScanFailed = false; gWifiScanRetry = 0;
    } else if (done == WIFI_SCAN_FAILED) {
      // STA 在后台反复重连会把异步扫描打断（连不上网时必现），这里自动重来几次，
      // 否则页面会一直卡在 SCANNING...，用户连重新选网络的入口都没有。
      uint32_t nowMs = millis();
      if (gWifiScanRetry < 3) {
        if (nowMs - gWifiScanRetryMs > 700) {
          gWifiScanRetryMs = nowMs;
          gWifiScanRetry++;
          // 第 2 次重试前掐掉后台自动重连 —— 连不上网时它每秒重试一次，
          // 会把异步扫描反复打断，导致列表永远刷不出来（用户也就没法换网）。
          if (gWifiScanRetry == 2) {
            WiFi.setAutoReconnect(false);
            WiFi.disconnect(true, false);   // 关射频，彻底停掉后台重连
            delay(150);
            WiFi.mode(WIFI_STA);            // 重新开射频（STA）
            delay(150);
            gWifiStoppedReconnect = true;
          }
          WiFi.scanNetworks(true, true);
          Serial.printf("[WIFI] scan interrupted, retry %d/3%s status=%d\n",
                        gWifiScanRetry, gWifiStoppedReconnect ? " (reconnect paused)" : "",
                        (int)WiFi.status());
        }
      } else {
        gWifiScanning = false; gWifiScanFailed = true; gWifiN = 0;
        Serial.println("[WIFI] scan failed 3x -> tap RESCAN");
      }
    }
  }

  clear(C_DARK2);
  drawText(180, 34, "WIFI SETUP", C_WHITE, 16, 1, 1);

  if (gWifiState == W_LIST) {
    if (gWifiScanning) {
      drawText(180, 180, "SCANNING...", C_GREY, 16, 1, 1);
    } else if (gWifiN == 0) {
      drawText(180, 170, gWifiScanFailed ? "SCAN FAILED" : "NO NETWORK",
               gWifiScanFailed ? C_ORANGE : C_GREY, 16, 1, 1);
      drawText(180, 196, "tap RESCAN", C_GREY, 12, 1, 1);
    } else {
      int rowH = 34, top = 64, visible = 6;
      int maxScroll = max(0, gWifiN - visible);
      if (gWifiScroll > maxScroll) gWifiScroll = maxScroll;
      for (int i = 0; i < visible; i++) {
        int idx = i + gWifiScroll;
        if (idx >= gWifiN) break;
        int ry = top + i * rowH;
        fillRoundRect(46, ry, 268, rowH - 4, 6,
                      (idx == gWifiSel) ? C_SLATE : COL(28, 36, 44));
        int rssi = gNetRssi[idx];
        int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
        for (int b = 0; b < 4; b++) {
          int bh = 4 + b * 3;
          fillRect(56 + b * 5, ry + 23 - bh, 3, bh,   // 底边与 16px 文字底对齐
                   b < bars ? C_GREEN : COL(60, 70, 80));
        }
        if (gNetEnc[idx] != WIFI_AUTH_OPEN) {         // 加密锁标
          fillRoundRect(84, ry + 9, 9, 8, 1, C_GREY);
          arcRing(88, ry + 10, 4, 200, 340, 1, C_GREY);
        }
        String bkPass;
        bool   known = wifiBookFind(String(gNetSSID[idx]), bkPass);   // 密码簿里有记录
        char disp[34];
        fitText(disp, sizeof(disp), gNetSSID[idx], 16, known ? 314 - 104 - 26 : 314 - 104 - 12);
        drawBtnText(104, ry, rowH - 4, disp, C_WHITE, 16, 1, 0);
        if (known) {                                   // 已记住密码：行尾打绿勾
          line(299, ry + 15, 304, ry + 20, C_GREEN, 2);
          line(304, ry + 20, 311, ry + 9,  C_GREEN, 2);
        }
      }
      if (maxScroll > 0)
        drawText(180, 276, gWifiScroll > 0 ? "swipe / more ^v" : "more networks v", C_GREY, 12, 1, 1);
    }
    // v2.8：底部只留 RESCAN，回设置页靠左右滑（返回键撤掉了）
    fillRoundRect(141, 288, 78, 22, 8, C_ORANGE_D);
    drawBtnText(180, 288, 22, "RESCAN", C_WHITE);
  } else {
    char sdisp[34];
    fitText(sdisp, sizeof(sdisp), gWifiSSID.c_str(), 16, 186);   // y=56 处圆内可用宽度 ≈196，留边
    drawText(180, 56, sdisp, C_WHITE, 16, 1, 1);
    drawText(180, 76,
             !gWifiSecured      ? "OPEN NETWORK" :
             gWifiPassFromBook  ? "PASS SAVED / TAP JOIN" : "PASSWORD",
             gWifiPassFromBook ? C_GREEN : COL(139, 151, 169), 12, 1, 1);
    fillRoundRect(40, 92, 280, 26, 6, C_SLATE);        // 密码框
    String shown;
    int L = gWifiPass.length();
    if (L > 0) { for (int i = 0; i < L - 1; i++) shown += "*"; shown += gWifiPass.charAt(L - 1); }
    char pbuf[40]; strncpy(pbuf, shown.c_str(), 39); pbuf[39] = 0;
    if (L == 0) drawBtnText(52, 92, 26, "(type password)", COL(90, 100, 112), 16, 1, 0);
    else        drawBtnText(52, 92, 26, pbuf, C_WHITE, 16, 1, 0);
    drawWifiKeyboard();
  }
}

static void wifiConnect() {
  cfgSet("ssid", gWifiSSID);
  cfgSet("pass", gWifiPass);
  gCfg.ssid = gWifiSSID; gCfg.pass = gWifiPass;
  if (gWifiSecured) wifiBookPut(gWifiSSID, gWifiPass);   // 记住这次输入的密码
  clear(C_DARK2);
  drawText(180, 152, "SAVED", C_GREEN, 24, 1, 1);
  if (gWifiSecured) drawText(180, 184, "PASS REMEMBERED", COL(139, 151, 169), 12, 1, 1);
  drawText(180, 210, "REBOOTING...", C_WHITE, 16, 1, 1);
  drawText(180, 234, gWifiSSID.c_str(), C_GREY, 12, 1, 1);
  flushScreen();
  Serial.printf("[WIFI] saved ssid='%s' secured=%s, reboot to STA\n",
                gWifiSSID.c_str(), gWifiSecured ? "YES" : "NO");
  delay(800);
  ESP.restart();
}

// ================================================================ 设置页
// 一行一个设置项：Wi-Fi（进入配网页）/ 亮度 / 音量。后续新设置继续往这里加行即可。
// v0.15 起设置齿轮已移除：任意页面下拉即可呼出设置，表盘不再放图标。

static void drawSliderRow(int rowY, const char *label, int val, int vmax, const char *valTxt) {
  fillRoundRect(ST_RX, rowY, ST_RW, ST_RH, 10, COL(28, 36, 44));
  drawText(ST_BAR_X, rowY + 9, label, COL(150, 162, 182), 12, 1, 0);
  // 数值（右对齐到进度条末端）
  drawText(ST_BAR_X + ST_BAR_W, rowY + 9, valTxt, C_WHITE, 12, 1, 2);
  // 进度条
  fillRoundRect(ST_BAR_X, rowY + 22, ST_BAR_W, 8, 4, COL(52, 62, 74));
  int w = (vmax > 0) ? (val * ST_BAR_W / vmax) : 0;
  if (w < 6 && val > 0) w = 6;
  if (w > 0) fillRoundRect(ST_BAR_X, rowY + 22, w, 8, 4, C_ORANGE);
  // [-] [+]
  int cy = rowY + 17;
  fillCircle(ST_BTN_MINUS_CX, cy, ST_BTN_R, C_SLATE);
  fillRect(ST_BTN_MINUS_CX - 6, cy - 1, 12, 3, C_WHITE);
  fillCircle(ST_BTN_PLUS_CX, cy, ST_BTN_R, C_SLATE);
  fillRect(ST_BTN_PLUS_CX - 6, cy - 1, 12, 3, C_WHITE);
  fillRect(ST_BTN_PLUS_CX - 1, cy - 6, 3, 12, C_WHITE);
}

static void renderSettings() {
  clear(C_DARK2);
  // 标题行 y=68：5 行卡片从 y=96 起，标题留在 86 会被第一行盖住。
  // v2.8：左上角那颗 "<" 撤掉，标题独占一行；退出设置靠左右/下/上滑。
  drawText(180, 68, "SETTINGS", C_WHITE, 16, 1, 1);

  // ── 第 1 行：Wi-Fi（显示当前 SSID / 状态，点击进配网页）
  fillRoundRect(ST_RX, ST_R1, ST_RW, ST_RH, 10, COL(28, 36, 44));
  drawWifiIcon(ST_RX + 22, ST_R1 + 18, wifiOK ? C_GREEN : C_GREY);
  drawText(ST_RX + 40, ST_R1 + 11, "WIFI", C_WHITE, 12, 1, 0);
  const char *st = wifiOK ? gCfg.ssid.c_str() : (gCfg.ssid.length() ? "CONNECTING.." : "NOT SET");
  char wbuf[26];
  fitText(wbuf, sizeof(wbuf), st, 12, 150);
  drawText(ST_RX + ST_RW - 26, ST_R1 + 11, wbuf, wifiOK ? C_GREEN : C_GREY, 12, 1, 2);
  drawText(ST_RX + ST_RW - 12, ST_R1 + 11, ">", C_GREY, 12, 1, 1);

  // ── 第 2 行：亮度
  char b[8]; snprintf(b, sizeof(b), "%d%%", gBright);
  drawSliderRow(ST_R2, "BRIGHT", gBright - BRIGHT_MIN, BRIGHT_MAX - BRIGHT_MIN, b);

  // ── 第 3 行：音量
  char v[10]; snprintf(v, sizeof(v), "%d/%d", gVolume, VOL_MAX);
  drawSliderRow(ST_R3, "VOLUME", gVolume, VOL_MAX, v);

  // ── 第 4 行：左 FACE 主界面表盘（模拟 / 数字）/ 右 LANG 数字表盘语言
  fillRoundRect(ST_FACE_X, ST_R4, ST_HALF_W, ST_RH, 10, COL(28, 36, 44));
  drawText(ST_FACE_X + 14, ST_R4 + 11, "FACE", C_WHITE, 12, 1, 0);
  // 值就是表盘名本身：一眼看清 home 现在是哪块脸
  drawBtnText(ST_FACE_X + ST_HALF_W - 12, ST_R4, ST_RH, gFace ? "数字" : "模拟",
              gFace ? COL(126, 200, 240) : C_ORANGE, 16, 1, 2);

  fillRoundRect(ST_LANG_X, ST_R4, ST_HALF_W, ST_RH, 10, COL(28, 36, 44));
  drawText(ST_LANG_X + 14, ST_R4 + 11, "LANG", C_WHITE, 12, 1, 0);
  drawBtnText(ST_LANG_X + ST_HALF_W - 12, ST_R4, ST_RH, gLang ? "ENG" : "中文",
              gLang ? COL(126, 200, 240) : C_ORANGE, 16, 1, 2);

  // ── 第 5 行：天气城市（城市名是中文，必须走 16px 全量字库）
  fillRoundRect(ST_CITY_X, ST_R5, ST_CITY_W, ST_RH, 10, COL(28, 36, 44));
  drawText(ST_CITY_X + 16, ST_R5 + 11, "CITY", C_WHITE, 12, 1, 0);
  drawBtnText(ST_CITY_X + ST_CITY_W - 24, ST_R5, ST_RH, CITIES[gCityIdx].name, C_GREEN, 16, 1, 2);
  drawText(ST_CITY_X + ST_CITY_W - 12, ST_R5 + 11, ">", C_GREY, 12, 1, 1);
}

// ---------------------------------------------------------------- 天气城市选择浮层
static void citySelect(int idx) {
  idx = constrain(idx, 0, CITY_COUNT - 1);
  gCityIdx = idx;
  gHomeLat = CITIES[idx].lat;
  gHomeLon = CITIES[idx].lon;
  cfgSetInt("city", idx);
  // 旧航班快照是相对旧坐标算出来的，换城市必须丢弃，否则雷达会画错方位
  lastLiveCount = 0; planeCount = 0; liveCount = 0; detailIdx = -1;
  wx.live = false;
  lastWx = 0; lastAdsb = 0;        // 下一轮 netTask 立刻按新坐标重拉天气 + ADS-B
  char t[44]; snprintf(t, sizeof(t), "CITY / %s", CITIES[idx].name);
  showToast(t);
  Serial.printf("[CITY] -> %d %s (%.4f, %.4f)\n", idx + 1, CITIES[idx].name, gHomeLat, gHomeLon);
}

static void renderCityList() {
  clear(C_DARK2);
  drawText(180, 36, "CITY / WEATHER", C_WHITE, 12, 1, 1);
  drawText(180, 54, "TAP TO SET LOCATION", C_GREY, 12, 1, 1);
  // v2.8：左上 "<" 与底部 "< BACK" 都撤掉，滑一下回设置页（子页只关自己这层）
  int pages = (CITY_COUNT + CITY_ROWS - 1) / CITY_ROWS;
  if (gCityScroll >= pages) gCityScroll = 0;
  for (int r = 0; r < CITY_ROWS; r++) {
    int idx = gCityScroll * CITY_ROWS + r;
    if (idx >= CITY_COUNT) break;
    int y = CITY_ROW_Y0 + r * CITY_ROW_H;
    bool sel = (idx == gCityIdx);
    fillRoundRect(64, y, 232, 32, 6, sel ? C_ORANGE_D : COL(28, 36, 44));
    drawBtnText(80, y, 32, CITIES[idx].name, sel ? C_YELLOW : C_WHITE, 16, 1, 0);
    if (sel) drawBtnText(282, y, 32, "*", C_YELLOW, 16, 1, 2);
    else {
      char num[6]; snprintf(num, sizeof(num), "%d", idx + 1);
      drawBtnText(282, y, 32, num, C_GREY, 12, 1, 2);
    }
  }
  char pg[12]; snprintf(pg, sizeof(pg), "%d/%d", gCityScroll + 1, pages);
  drawText(180, 268, pg, C_GREY, 12, 1, 1);
  fillRoundRect(141, 288, 78, 22, 8, C_ORANGE_D);
  drawBtnText(180, 288, 22, "MORE >", C_WHITE);
}

static bool cityListTap(int x, int y) {
  int pages = (CITY_COUNT + CITY_ROWS - 1) / CITY_ROWS;
  if (y >= 284 && y <= 312) {
    if (x >= 141 && x <= 219) { gCityScroll = (gCityScroll + 1) % pages; render(); return true; }
    return true;
  }
  for (int r = 0; r < CITY_ROWS; r++) {
    int ry = CITY_ROW_Y0 + r * CITY_ROW_H;
    if (x >= 64 && x <= 296 && y >= ry && y <= ry + 32) {
      int idx = gCityScroll * CITY_ROWS + r;
      if (idx < CITY_COUNT) { citySelect(idx); gCityList = false; render(); }
      return true;
    }
  }
  return true;   // 浮层吞掉其余点击
}

static void brightStep(int d) {
  gBright += d;
  if (gBright < BRIGHT_MIN) gBright = BRIGHT_MIN;
  if (gBright > BRIGHT_MAX) gBright = BRIGHT_MAX;
  Set_Backlight((uint8_t)gBright);
  cfgSetInt("bright", gBright);
  Serial.printf("[UI] bright -> %d%%\n", gBright);
}
static void volumeStep(int d) {
  gVolume += d;
  if (gVolume < 0) gVolume = 0;
  if (gVolume > VOL_MAX) gVolume = VOL_MAX;
  audioVolume(gVolume);
  cfgSetInt("vol", gVolume);
  Serial.printf("[UI] volume -> %d/21\n", gVolume);
}

// 任意页面从屏幕上半部往下长拉 -> 设置页；退出设置靠手势（左右/下/上滑都回原页）。
// 只认「起点靠上 + 下拉够长 + 明显竖向」的手势，26px 的普通下滑仍是翻页。
#define PULL_START_Y  125      // 起点 y 上限（圆屏上半部）
#define PULL_MIN_DY   55       // 下拉距离下限
static void enterSettings() {
  if (curPage != P_SETTINGS) {
    gSettingsFrom = curPage;
    gMusicList = false;        // 曲库浮层不跟着带进设置页
  }
  gCityList = false;
  curPage = P_SETTINGS;
  render();
}
static void exitSettings() {
  gCityList = false;
  curPage = gSettingsFrom;
  Serial.printf("[UI] settings exit -> page %d (gesture)\n", (int)curPage);
  render();
}

// 设置页点击：进 Wi-Fi / 亮度 -+ / 音量 -+ / 城市（返回靠手势）
static bool settingsTap(int x, int y) {
  if (curPage != P_SETTINGS) return false;
  if (gCityList) return cityListTap(x, y);
  if (y >= ST_R1 && y <= ST_R1 + ST_RH) { enterWifiList(); render(); return true; }
  if (y >= ST_R2 && y <= ST_R2 + ST_RH) {
    if (hit(x, y, ST_BTN_MINUS_CX, ST_R2 + 21, ST_BTN_R + 6))      brightStep(-BRIGHT_STEP);
    else if (hit(x, y, ST_BTN_PLUS_CX, ST_R2 + 21, ST_BTN_R + 6))  brightStep(+BRIGHT_STEP);
    render(); return true;
  }
  if (y >= ST_R3 && y <= ST_R3 + ST_RH) {
    if (hit(x, y, ST_BTN_MINUS_CX, ST_R3 + 21, ST_BTN_R + 6))      volumeStep(-3);
    else if (hit(x, y, ST_BTN_PLUS_CX, ST_R3 + 21, ST_BTN_R + 6))  volumeStep(+3);
    render(); return true;
  }
  if (y >= ST_R4 && y <= ST_R4 + ST_RH) {
    if (x < 180) {                 // 左半：主界面表盘 模拟 <-> 数字（home 只显示选中的那一块）
      toggleClockFace();
    } else {                       // 右半：数字表盘语言 中文 <-> English
      toggleLang();
    }
    return true;
  }
  if (y >= ST_R5 && y <= ST_R5 + ST_RH) {
    gCityList = true;
    gCityScroll = gCityIdx / CITY_ROWS;
    render(); return true;
  }
  return true;   // 设置页吞掉其余点击，避免误触翻页
}

// ================================================================ 应用中心（Apple Watch 蜂窝）
// 除主界面（模拟/数字两块表盘）外，所有入口都收在这里。布局是错位蜂窝（正三角网格）：
//   行 y= 72 : x=180                          （1 格 · 设置 坐顶点）
//   行 y=130 : x=146, 214                     （2 格）
//   行 y=188 : x=112, 180, 248                （3 格）
//   行 y=246 : x= 78, 146, 214, 282           （4 格）
// 列距 68 / 行距 58（= 68*√3/2 ≈ 58.9），对角线相邻圆心距 67.2，图标 r=32 -> 间隙 3~4px。
// v2.2：改成**正三角形 1+2+3+4 = 10 格**，10 个入口刚好排满一个三角形（原来是 2/3/4/3 的
// 圆饼形，底行两侧留两个预留位）。设置挪到顶点最上面 —— 它是"全局"入口，站最高一格；
// 其余 9 个功能按 2/3/4 往下一层层铺开。顶点 (180,72) 外缘 140、最外格 (282,246) 外缘 153.5，
// 都在 r158 安全区内；底缘 278，下面留 home 指示条。再加第 11 个入口时得往外扩一行（1/2/3/4/3）。
// 每格 = 功能图形（上半，26×26）+ 两字名称（下半，FONT16）—— 图形表达功能，文字兜底可读性。
// 格位与图形解耦（APP_GLYPH[]），`APP_GLYPH[i] < 0` 的预留位画法仍然保留，留作以后插空。
#define APP_N        10
#define APP_TILE_R   32
#define APP_HIT_R    36

static const int APP_X[APP_N] = {180, 146, 214, 112, 180, 248,  78, 146, 214, 282};
static const int APP_Y[APP_N] = { 72, 130, 130, 188, 188, 188, 246, 246, 246, 246};
static const char *const APP_LABEL[APP_N] = {"设置",
                                             "音乐", "雷达", "番茄", "小智", "单词",
                                             "备忘", "宠物", "录音", "待办"};
static const Page APP_PAGE[APP_N] = {P_SETTINGS,
                                     P_MUSIC, P_RADAR, P_POMO, P_CHAT, P_WORDS,
                                     P_MEMO,  P_PET,   P_RECORDER, P_TODO};
// 图形编号（drawAppGlyph 里的 case），与格位解耦：增删入口不用重排图形
static const int APP_GLYPH[APP_N] = {11, 1, 2, 3, 4, 5, 6, 7, 8, 9};
// 每格一个强调色，图形和描边同色 —— 不靠文字也能一眼分辨
static const uint16_t APP_COL[APP_N] = {
  COL(188, 198, 212),   // 设置 银
  COL(246, 168,  86),   // 音乐 橙
  COL(104, 214, 150),   // 雷达 绿
  COL(238, 104,  92),   // 番茄 红
  COL(250, 206,  90),   // 小智 黄
  COL(150, 196, 246),   // 单词 蓝
  COL(120, 222, 186),   // 备忘 青
  COL(240, 176,  96),   // 宠物 驼
  COL(232, 146, 108),   // 录音 砖红
  COL(140, 214, 158)    // 待办 草绿
};

// 格内功能图形（26×26，中心 cx,cy）。bg 用来"挖空"细节（眼睛、指针、齿轮轴）。
static void drawAppGlyph(int id, int cx, int cy, uint16_t c) {
  const uint16_t bg = COL(30, 38, 48);
  switch (id) {
    case 0:   // 时钟：表盘 + 时针分针（v2.1 起主界面自带两种表盘，此图形暂未上墙，留作扩充）
      circle(cx, cy, 10, c, 2);
      line(cx, cy, cx, cy - 6, c, 2);
      line(cx, cy, cx + 6, cy + 3, c, 2);
      fillCircle(cx, cy, 2, c);
      break;
    case 1:   // 音乐：八分音符
      fillCircle(cx - 4, cy + 6, 4, c);
      line(cx - 1, cy + 6, cx - 1, cy - 8, c, 2);
      line(cx - 1, cy - 8, cx + 7, cy - 6, c, 2);
      line(cx + 7, cy - 6, cx + 7, cy + 1, c, 2);
      break;
    case 2:   // 雷达：同心圈 + 扇形扫描 + 光点
      circle(cx, cy, 10, c, 2);
      circle(cx, cy, 5, mix(c, bg, 80), 1);
      fillTri(cx, cy, cx + 9, cy - 9, cx + 11, cy - 2, mix(c, bg, 60));
      line(cx, cy, cx + 9, cy - 9, c, 2);
      fillCircle(cx + 9, cy - 9, 2, c);
      break;
    case 3:   // 番茄钟：番茄 + 梗叶 + 指针
      fillCircle(cx, cy + 2, 9, c);
      line(cx, cy - 8, cx, cy - 12, c, 2);
      line(cx - 5, cy - 9, cx + 5, cy - 9, c, 2);
      line(cx, cy + 2, cx, cy - 4, bg, 2);
      line(cx, cy + 2, cx + 4, cy + 4, bg, 2);
      break;
    case 4:   // 小智：对话气泡 + 三个点
      fillRoundRect(cx - 12, cy - 10, 24, 16, 7, c);
      fillTri(cx - 6, cy + 4, cx - 1, cy + 4, cx - 6, cy + 11, c);
      fillCircle(cx - 6, cy - 2, 2, bg);
      fillCircle(cx, cy - 2, 2, bg);
      fillCircle(cx + 6, cy - 2, 2, bg);
      break;
    case 5:   // 单词：单词卡（亮卡 + 深色 A）—— 26px 下画"摊开的书"只会看成两块色块
      fillRoundRect(cx - 9, cy - 11, 18, 22, 3, c);
      drawText(cx, cy - 6, "A", bg, 12, 1, 1);
      break;
    case 6:   // 备忘：麦克风
      fillRoundRect(cx - 4, cy - 12, 9, 13, 4, c);
      arcRing(cx, cy - 2, 9, 100, 260, 2, c);
      line(cx, cy + 7, cx, cy + 10, c, 2);
      line(cx - 6, cy + 10, cx + 6, cy + 10, c, 2);
      break;
    case 7:   // 宠物：圆头 + 两只圆耳 + 眼睛 + 嘴（和宠物页那只同气质，但线描更好认）
      fillCircle(cx - 9, cy - 8, 4, c);
      fillCircle(cx + 9, cy - 8, 4, c);
      fillRoundRect(cx - 11, cy - 9, 23, 20, 8, c);
      fillCircle(cx - 4, cy - 1, 2, bg);
      fillCircle(cx + 4, cy - 1, 2, bg);
      fillRect(cx - 3, cy + 5, 6, 2, bg);
      break;
    case 8: { // 录音：波形竖条
      static const int BH[5] = {9, 15, 21, 15, 9};
      for (int i = 0; i < 5; i++)
        fillRoundRect(cx - 11 + i * 5, cy - BH[i] / 2, 3, BH[i], 1, c);
      break;
    }
    case 9:   // 待办：清单（第一条打勾）
      for (int i = 0; i < 3; i++) {
        int yy = cy - 9 + i * 8;
        rect(cx - 12, yy, 6, 6, c, 1);
        line(cx - 3, yy + 3, cx + 12, yy + 3, c, 2);
        if (i == 0) {
          line(cx - 11, yy + 3, cx - 9, yy + 5, c, 1);
          line(cx - 9, yy + 5, cx - 7, yy + 1, c, 1);
        }
      }
      break;
    case 10:  // 配网：Wi-Fi 扇形（已收进设置页首行，此图形暂未上墙，留作扩充）
      arcRing(cx, cy + 8, 12, 305, 415, 3, c);
      arcRing(cx, cy + 8, 7, 315, 405, 3, c);
      fillCircle(cx, cy + 8, 2, c);
      break;
    case 11:  // 设置：齿轮（8 齿 + 中心孔）
      circle(cx, cy, 8, c, 2);
      for (int k = 0; k < 8; k++) {
        float a = k * 45.0f * PI / 180.0f;
        line(cx + (int)(8.0f * cosf(a)),  cy + (int)(8.0f * sinf(a)),
             cx + (int)(12.0f * cosf(a)), cy + (int)(12.0f * sinf(a)), c, 3);
      }
      fillCircle(cx, cy, 3, bg);
      break;
    default: break;
  }
}

static void appTile(int i) {
  int cx = APP_X[i], cy = APP_Y[i];
  if (APP_GLYPH[i] < 0) {
    // 预留位：不画底座，只留一圈虚线 + 一颗淡十字 —— 一眼看出"这里以后会有功能"，
    // 又不会亮到抢走真图标的注意力（点击只弹一句提示，不跳页）。
    for (int k = 0; k < 8; k++)
      arcRing(cx, cy, APP_TILE_R - 4, k * 45, k * 45 + 22, 2, COL(56, 66, 82));
    fillRect(cx - 1, cy - 7, 3, 15, COL(52, 62, 78));
    fillRect(cx - 7, cy - 1, 15, 3, COL(52, 62, 78));
    return;
  }
  uint16_t c = APP_COL[i];
  // 立体底座：下投影 + 深色圆 + 强调色描边 + 内圈弱光（和 uiBtn 同一套立体语言）
  fillCircle(cx, cy + 2, APP_TILE_R, COL(8, 12, 16));
  fillCircle(cx, cy, APP_TILE_R, COL(30, 38, 48));
  circle(cx, cy, APP_TILE_R, c, 2);
  circle(cx, cy, APP_TILE_R - 4, mix(COL(30, 38, 48), c, 64), 1);
  drawAppGlyph(APP_GLYPH[i], cx, cy - 8, c);
  drawBtnText(cx, cy + 7, 16, APP_LABEL[i], COL(226, 234, 244), 16, 1, 1);
}

static void renderApps() {
  // 蜂窝图标墙：没有标题栏、没有返回键 —— Apple Watch 观感，
  // 进出全靠手势（左右滑进、上滑回表盘），底部一条手机式 home 指示条提示可上滑。
  clear(C_DARK);
  fillCircle(180, 180, 172, COL(14, 19, 26));
  circle(180, 180, 168, COL(46, 58, 72), 2);
  for (int i = 0; i < APP_N; i++) appTile(i);
  fillRoundRect(158, 316, 44, 5, 2, COL(96, 106, 122));
}

// ================================================================ v1.0 功能页公共件
// v2.8：页内返回键整体撤掉（用户口径：手势左右默认返回即可）。原来那颗
// (106,62) r16 的 "<" 在九个功能页上都占着标题栏左侧的黄金位置，且和
// 「左右滑 = 回上一层」重复。现在返回只有一条路：左右/下滑回上一层、上滑回表盘，
// 子页（期限/选词书/城市/录音列表/小智绑定/曲库）滑一下只关自己那层。

// UTF-8 安全的居中自动换行：按整字符累加像素宽，超宽断到下一行。
// x = 行中心；y = 第一行字形顶部；返回实际画了几行。
static int drawWrapText(const char *s, int x, int y, int maxW, int lineH,
                        int maxLines, uint16_t col, int font) {
  if (!s || !*s) return 0;
  int lines = 0;
  const char *p = s;
  char buf[96];
  while (*p && lines < maxLines) {
    int n = 0; buf[0] = 0;
    while (*p) {
      unsigned char c = (unsigned char)*p;
      int clen = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      if (*(p + clen - 1) == 0) clen = strlen(p);   // 尾部残串保护
      if (clen == 1 && *p == '\n') { p++; break; }
      char tmp[8]; memcpy(tmp, p, clen); tmp[clen] = 0;
      if (n > 0 && textWidth(buf, font, 1) + textWidth(tmp, font, 1) > maxW) break;
      memcpy(buf + n, p, clen); n += clen; buf[n] = 0;
      p += clen;
    }
    if (n == 0 && !*p) break;
    drawText(x, y + lines * lineH, buf, col, font, 1, 1);
    lines++;
  }
  return lines;
}

// 三个语音页共用的大圆键：IDLE「说话/录音」→ RECORDING「停止」→ PROCESSING「识别中」
static void drawVoiceButton(int cx, int cy, int r, const char *idleLabel, AiVoiceMode mode) {
  bool rec = gAiState == AVS_RECORDING && !gAiRecToFile && gAiMode == mode;
  bool proc = gAiState == AVS_PROCESSING && gAiMode == mode;
  const char *lab = rec ? "停止" : proc ? (gAiCancel.load() ? "取消中" : "取消") : idleLabel;
  uiBtn(cx, cy, r, lab, rec ? UI_D : proc ? UI_N : UI_A, 24);
  if (rec) {
    // 电平条画在圆键**内部下方**（不放圆外）：录音机键已在 y=288，
    // 若按老写法 cy+r+22 会到 y=350，四角距圆心 188 远超安全区 158。
    int bw = (int)(r * 1.6);
    int by = cy + 10;
    fillRoundRect(cx - bw / 2, by, bw, 6, 3, COL(70, 30, 30));
    int lv = constrain(gAiRms / 8, 0, 100);
    if (lv > 0) fillRoundRect(cx - bw / 2, by, bw * lv / 100, 6, 3, C_GREEN);
  }
}

// ---------------- 小智问答：语音提问 → Qwen 答复 ----------------
// 结果是否属于当前页：问答 / 备忘 / 会议各看各的，切页不串台。
// 串口 QWEN 的纯文字结果落 gLastAnswer，只属于问答页。
static bool aiResFor(AiVoiceMode m) {
  return gAiResultOwner == m && (gAiTranscript.length() || gAiResult.length());
}
static bool gXzPageOpen = false;

static void renderXzBinding() {
  uiPage("小智绑定", C_ORANGE_D);
  XzBinding state = gXzState.load();
  switch (state) {
    case XzBinding::Fetching:
      drawText(180, 122, "正在查询设备", C_CREAM, 16, 1, 1);
      break;
    case XzBinding::Waiting: {
      drawText(180, 92, "xiaozhi.me", C_CREAM, 16, 1, 1);
      char code[7];
      snprintf(code, sizeof(code), "%06d", constrain(gXzCode.load(), 0, 999999));
      drawText(180, 125, code, C_WHITE, 24, 2, 1);
      drawText(180, 196, "在平台输入绑定码", C_GREY, 16, 1, 1);
      drawText(180, 220, "等待确认", C_GREY, 16, 1, 1);
      break;
    }
    case XzBinding::Bound:
      drawText(180, 122, "设备已绑定", C_GREEN, 16, 2, 1);
      break;
    case XzBinding::Offline:
      drawText(180, 122, "请连接 Wi-Fi", C_CREAM, 16, 1, 1);
      break;
    case XzBinding::Failed: {
      drawText(180, 122, "查询绑定失败", C_CREAM, 16, 1, 1);
      char status[20];
      snprintf(status, sizeof(status), "XZ %d", gXzStatus.load());
      drawText(180, 156, status, C_GREY, 16, 1, 1);
      break;
    }
    case XzBinding::Expired:
      drawText(180, 122, "绑定码已过期", C_CREAM, 16, 1, 1);
      break;
    case XzBinding::Idle:
      break;
  }
  if (state != XzBinding::Waiting && state != XzBinding::Fetching)
    uiBtn(180, 260, 34, "查询", UI_A);
}

// 显示前把首尾的句读符号剪掉。drawWrapText 按像素断行，最后一个"？"经常
// 挤到下一行变成孤零零一个标点；纯标点的识别结果（比如只听到"。"）也直接丢。
static String crTrimPunct(String s) {
  static const char *PUNCT[] = {"，", "。", "、", "？", "！", "；", "：", "…", "·",
                                ",", ".", "?", "!", ";", ":", " "};
  for (int pass = 0; pass < 8 && s.length(); ++pass) {
    s.trim();
    bool cut = false;
    for (const char *p : PUNCT) {
      const size_t n = strlen(p);
      if (s.endsWith(p)) { s.remove(s.length() - n); cut = true; break; }
    }
    if (!cut) break;
  }
  return s;
}

static void renderChat() {
  if (gXzPageOpen) { renderXzBinding(); return; }
  uiPage("小智", C_ORANGE_D);
  const XzChat st = gXzChat.load();
  const uint32_t now = millis();
  const bool active = xzChatActive();

  // ---- 状态行（y=74）：一句话 + 三个点（连接/听/想的时候转起来） ----
  const char *label = "点下面说话";
  uint16_t lcol = C_GREY;
  switch (st) {
    case XC_CONNECTING: label = "连接小智"; lcol = C_YELLOW; break;
    case XC_READY:      label = "准备好了"; lcol = C_GREEN;  break;
    case XC_LISTENING:  label = "听你说";  lcol = COL(120, 210, 255); break;
    case XC_THINKING:   label = "想一想";  lcol = C_ORANGE; break;
    case XC_SPEAKING:   label = "小智在说"; lcol = C_GREEN;  break;
    case XC_ERROR:      label = "出错了";  lcol = COL(240, 120, 110); break;
    default: break;
  }
  drawText(180, 74, label, lcol, 16, 1, 1);
  if (st == XC_CONNECTING || st == XC_LISTENING || st == XC_THINKING) {
    const int lit = (int)((now / 260) % 4);
    for (int i = 0; i < 3; i++) {
      const int dx = 148 + i * 16;
      fillCircle(dx, 74 + 8, 3, i == lit ? lcol : COL(58, 68, 80));
    }
  }

  // ---- 你说（灰色小字，来自服务器 stt） ----
  String heard = crTrimPunct(xzGetText(gXzHeard));
  String reply = crTrimPunct(xzGetText(gXzReply));
  String err = xzGetText(gXzErrText);
  if (heard.length()) {
    drawText(180, 100, "你", COL(120, 132, 148), 16, 1, 1);
    drawWrapText(heard.c_str(), 180, 118, 258, 18, 2, C_CREAM, 16);
  }

  // ---- 小智说（TTS 逐句文本，最多 4 行） ----
  if (reply.length()) {
    drawWrapText(reply.c_str(), 180, 152, 258, 19, 4, C_WHITE, 16);
  } else if (!heard.length() && !active) {
    drawText(180, 150, "连续对话", COL(120, 132, 148), 16, 1, 1);
    drawText(180, 176, "说完不用再点", COL(96, 108, 124), 16, 1, 1);
  }

  if (st == XC_ERROR && err.length()) {
    drawWrapText(err.c_str(), 180, 196, 250, 18, 2, COL(240, 150, 140), 16);
  }

  // ---- 底部主键：说话 / 说完 / 打断 ----
  const char *btn = "说话";
  uint16_t btnCol = C_ORANGE;
  if (st == XC_LISTENING) { btn = "说完"; btnCol = COL(120, 210, 255); }
  else if (st == XC_SPEAKING) { btn = "打断"; btnCol = C_GREEN; }
  else if (st == XC_THINKING || st == XC_CONNECTING) { btn = "取消"; btnCol = COL(120, 132, 148); }
  uiBtn(180, 288, 36, btn, btnCol);
  // 收音电平：说话时给一条小弧，让人知道"它在听"
  if (st == XC_LISTENING) {
    const int lv = constrain(gXzLevel.load(), 0, 100);
    if (lv > 2) fillRoundRect(180 - 40, 242, 80 * lv / 100, 4, 2, COL(120, 210, 255));
  }
  // 会话进行中不画"绑定"：那一块要留给多行答复，按钮会压到最后一行上
  if (!active) uiBtn(266, 250, 22, "绑定", UI_G);
}

// 离开小智页就收线：麦克风和网络都不该在别的页面继续跑
static void xzPageGuard() {
  static Page last = P_ANALOG;
  if (last == P_CHAT && curPage != P_CHAT) xzChatStop();
  last = curPage;
}

static void renderWords() { if (wordPicking) renderWordPicker(); else wordRender(); }
// ---------------- 语音备忘：语音 → 提取待办 → Todo ----------------
static void renderMemo() {
  uiPage("语音备忘", COL(90, 160, 130));
  bool mine = aiResFor(AVM_MEMO);
  if (mine && gAiTranscript.length()) {
    drawText(180, 72, "记下", C_GREY, 16, 1, 1);
    drawWrapText(gAiTranscript.c_str(), 180, 90, 264, 17, 2, C_CREAM, 16);
  }
  if (mine && gAiResult.length())
    drawWrapText(gAiResult.c_str(), 180, 124, 252, 18, 2, COL(140, 230, 180), 16);
  // 刚加进来的条目预览：让用户当场看到成果，而不只是一个数字
  if (mine && gAiState != AVS_ERROR && gMemoLastAdded > 0 && gTodoNewFrom >= 0) {
    int n = 0;
    for (int i = gTodoNewFrom; i < gTodoN && n < 3; i++, n++) {
      char line[40];
      fitText(line, sizeof(line), gTodos[i].text, 16, 236);
      drawText(180, 148 + n * 19, line, COL(170, 235, 200), 16, 1, 1);
    }
    if (gMemoLastAdded > 3) {
      char more[24]; snprintf(more, sizeof(more), "还有 %d 条", gMemoLastAdded - 3);
      drawText(180, 146 + 3 * 19, more, C_GREY, 16, 1, 1);
    }
  }
  if (!mine) {
    drawText(180, 118, "说出要做的事", C_GREY, 16, 1, 1);
    drawText(180, 144, "自动记进待办清单", COL(100, 130, 115), 16, 1, 1);
  }
  drawVoiceButton(138, 250, 44, "录音", AVM_MEMO);
  // 这颗是"跳到待办清单"的功能键，不是返回键 —— 文案统一成"清单"，两种入口都对
  uiBtn(242, 250, 36, "清单", UI_G);
}

static void renderPet()   { petRender(); }

// ---------------- 录音机：长录音 → AI 会议纪要 ----------------
// ---------------- 录音机（v2.5）：录音 → TF 卡 → 回放；纪要可选 ----------------
// 用户口径：录音机首先是录音机（录下来、能回放），文字纪要是**额外可选**的一步。
// 所以停止录音不再自动跑 ASR，页面上三颗键：回放 / 录音 / 纪要。
#define REC_ROWS 4
static void renderRecList() {
  clear(C_DARK2);
  fillCircle(180, 180, 172, UI_PAGE_BG);
  circle(180, 180, 168, COL(120, 96, 74), 2);
  drawText(180, 34, "录音文件", C_CREAM, 16, 1, 1);
  line(132, 60, 240, 60, UI_DIV);
  char cnt[12]; snprintf(cnt, sizeof(cnt), "%d", gRecN);   // 只画数字：FONT16 中英混排会撞
  drawText(180, 66, cnt, C_GREY, 16, 1, 1);
  int pages = (gRecN + REC_ROWS - 1) / REC_ROWS; if (pages < 1) pages = 1;
  if (gRecListPage >= pages) gRecListPage = pages - 1;
  if (gRecN == 0) {
    drawText(180, 150, "NO REC", C_GREY, 16, 1, 1);
    drawText(180, 176, "先录一段", COL(120, 100, 86), 16, 1, 1);
  }
  for (int r = 0; r < REC_ROWS; r++) {
    int idx = gRecListPage * REC_ROWS + r;
    if (idx >= gRecN) break;
    int y = 90 + r * 38;
    bool sel = (idx == gRecSel);
    fillRoundRect(66, y, 228, 32, 6, sel ? COL(70, 46, 34) : COL(28, 34, 42));
    drawBtnText(78, y, 32, gRec[idx].name, sel ? C_YELLOW : C_WHITE, 16, 1, 0);
    char s[12]; snprintf(s, sizeof(s), "%dS", (int)gRec[idx].secs);
    drawBtnText(278, y, 32, s, C_GREY, 16, 1, 2);
  }
  char pg[12]; snprintf(pg, sizeof(pg), "%d/%d", gRecListPage + 1, pages);
  drawText(180, 248, pg, C_GREY, 16, 1, 1);
  // v2.8：返回键撤掉（滑一下关浮层），只留翻页；只有一页时连这颗也不画
  if (pages > 1) uiBtn(180, 288, 28, "翻页", UI_N);
}

static bool recListTap(int x, int y);

static void renderRecorder() {
  // 第一次进这个页面先把卡里的录音列出来（进页面就走，不在 250ms 渲染里反复扫卡）
  if (!gRecScanned) { recScan(); gRecScanned = (gRecN > 0) || tfEnsure(); }
  if (gRecListOpen) { renderRecList(); return; }
  uiPage("录音机", COL(160, 110, 80));
  bool rec = gAiState == AVS_RECORDING && gAiRecToFile;
  bool busy = gAiState == AVS_PROCESSING && gAiMode == AVM_MEETING;
  bool play = recIsPlaying();

  // ---- 状态行（y=74）：红点/三角 + 纯数字，中文另外一段画 ----
  if (rec) {
    uint32_t sec = (millis() - gAiRecStartMs) / 1000;
    fillCircle(126, 84, 6, COL(238, 92, 80));
    char t[12]; snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    drawText(142, 74, t, COL(252, 158, 146), 16, 1, 0);
    drawText(210, 74, "录音", COL(200, 120, 108), 16, 1, 0);
  } else if (busy) {
    drawText(180, 74, "纪要生成中", COL(250, 206, 120), 16, 1, 1);
  } else if (play) {
    fillTri(124, 74, 124, 92, 140, 83, COL(150, 210, 250));
    // 分母用文件真实秒数：库里的 getAudioFileDuration 是截断值（10s 报 9s），
    // 用它会跟左边"共 N 段"里的时长对不上，看着像掉了一秒
    uint32_t dur = (gRecN > 0) ? gRec[gRecSel].secs : gAudDuration;
    char t[24]; snprintf(t, sizeof(t), "%d/%d", (int)gAudElapsed, (int)dur);
    drawText(150, 74, t, COL(180, 224, 252), 16, 1, 0);
  } else {
    drawText(180, 74, "就绪", COL(150, 132, 112), 16, 1, 1);
  }

  // ---- 电平条 / 进度条（y=106 高 10，x 116..244）----
  if (rec) {
    fillRoundRect(116, 106, 128, 10, 5, COL(40, 34, 30));
    int lv = constrain(gAiRms / 8, 0, 100);
    if (lv > 0) fillRoundRect(116, 106, 128 * lv / 100, 10, 5, COL(214, 96, 76));
  } else if (play && gAudDuration > 0) {
    fillRoundRect(116, 106, 128, 10, 5, COL(26, 34, 44));
    int lv = constrain((int)gAudElapsed * 100 / (int)gAudDuration, 0, 100);
    fillRoundRect(116, 106, 128 * lv / 100, 10, 5, COL(96, 160, 210));
  }   // 空闲态不画空槽：一条深色横杠只会看成"没画完的进度条"

  // ---- 当前选中这一段 ----
  if (gRecN > 0) {
    char nm[64];
    fitText(nm, sizeof(nm), gRec[gRecSel].name, 16, 258);
    drawText(180, 132, nm, C_WHITE, 16, 1, 1);
  } else {
    drawText(180, 132, "还没有录音", COL(150, 132, 112), 16, 1, 1);
  }
  // 段数 / 存储（数字与中文分段：FONT16 下中文 16px、ASCII 只有 10px，混排必撞）
  if (!gRecWrite && gRecMsg[0] && (int32_t)(millis() - gRecMsgUntil) >= 0) gRecMsg[0] = 0;   // 提示 3 秒后让位
  if (gRecMsg[0]) {
    drawText(180, 158, gRecMsg, COL(200, 176, 140), 16, 1, 1);
  } else if (gRecN > 0) {
    char n1[8]; snprintf(n1, sizeof(n1), "%d", gRecN);
    int w1 = textWidth(n1, 16, 1);
    int total = 16 + 6 + w1 + 6 + 16;          // 共 + N + 段
    int x0 = 180 - total / 2;
    drawText(x0, 158, "共", C_GREY, 16, 1, 0);
    drawText(x0 + 22, 158, n1, C_CREAM, 16, 1, 0);
    drawText(x0 + 22 + w1 + 6, 158, "段", C_GREY, 16, 1, 0);
    drawText(180, 182, "点文件名打开列表", COL(120, 104, 90), 16, 1, 1);
  } else {
    drawText(180, 158, "点圆键开始录音", COL(120, 104, 90), 16, 1, 1);
    drawText(180, 182, "存在 TF 卡 REC 目录", COL(96, 88, 80), 16, 1, 1);
  }

  // ---- 纪要（可选，点"纪要"键才生成） ----
  if (gAiResult.length() && gAiResultOwner == AVM_MEETING) {
    drawText(180, 204, gAiState == AVS_ERROR ? "识别状态" : "纪要", COL(230, 170, 120), 16, 1, 1);
    drawWrapText(gAiResult.c_str(), 180, 222, 264, 18, 2, C_WHITE, 16);
  }

  // ---- 三颗键：回放 / 录音 / 纪要 ----
  uiBtn(112, 292, 24, play ? "停止" : "回放", play ? UI_D : UI_G);
  if (rec) uiBtn(180, 286, 34, "停止", UI_D, 24);
  else uiBtn(180, 286, 34, "录音", busy ? UI_N : UI_A, 24);
  uiBtn(248, 292, 24, busy ? "取消" : "纪要", UI_S);
}

static bool recListTap(int x, int y) {
  int pages = (gRecN + REC_ROWS - 1) / REC_ROWS; if (pages < 1) pages = 1;
  if (pages > 1 && hit(x, y, 180, 288, 34)) { gRecListPage = (gRecListPage + 1) % pages; render(); return true; }
  for (int r = 0; r < REC_ROWS; r++) {
    int ry = 90 + r * 38;
    if (x >= 66 && x <= 294 && y >= ry && y <= ry + 32) {
      int idx = gRecListPage * REC_ROWS + r;
      if (idx < gRecN) {
        gRecSel = idx;
        gAiTranscript = ""; gAiResult = "";     // 换了一段，旧纪要作废
        gRecListOpen = false;
        recPlaySel();
        render();
      }
      return true;
    }
  }
  return true;
}

// ---------------- 待办清单：清单是唯一数据源，勾选 / 删除 / 翻页 / 清空 / 语音加 ----------------
#define TODO_PER_PAGE 5
static void renderTodo() {
  uiPage("待办清单", COL(90, 150, 110));
  int pages = (gTodoN + TODO_PER_PAGE - 1) / TODO_PER_PAGE;
  if (pages < 1) pages = 1;
  if (gTodoPage >= pages) gTodoPage = pages - 1;
  // 中文 12px 会缺字（FONT12 无中文），数字与中文分段画
  if (gTodoN == 0) {
    drawText(180, 58, "空", C_GREY, 16, 1, 1);
  } else {
    char c1[12], c2[16];
    snprintf(c1, sizeof(c1), "%d", gTodoN);
    drawText(120, 58, c1, C_GREY, 16, 1, 0);
    drawText(138, 58, "条", C_GREY, 16, 1, 0);
    if (pages > 1) {
      snprintf(c2, sizeof(c2), "%d/%d", gTodoPage + 1, pages);
      drawText(166, 58, "第", C_GREY, 16, 1, 0);
      drawText(184, 58, c2, C_YELLOW, 16, 1, 0);
      drawText(228, 58, "页", C_GREY, 16, 1, 0);
    }
  }
  line(110, 80, 250, 80, COL(50, 70, 58));
  if (gTodoN == 0) {
    drawText(180, 158, "还没有待办", C_GREY, 16, 1, 1);
    drawText(180, 186, "点语音加说一条", COL(90, 120, 104), 16, 1, 1);
  } else {
    for (int k = 0; k < TODO_PER_PAGE; k++) {
      int i = gTodoPage * TODO_PER_PAGE + k;
      if (i >= gTodoN) break;
      int ry = 92 + k * 31;
      bool isNew = (gTodoNewFrom >= 0 && i >= gTodoNewFrom && gMemoLastAdded > 0);
      uint16_t col = gTodos[i].done ? C_GREY : (isNew ? COL(170, 235, 200) : C_WHITE);
      // 勾选框
      rect(48, ry, 18, 18, gTodos[i].done ? C_GREEN : COL(90, 110, 100), 2);
      if (gTodos[i].done) {
        line(52, ry + 9, 57, ry + 14, C_GREEN, 2);
        line(57, ry + 14, 63, ry + 4, C_GREEN, 2);
      }
      char txt[28];
      fitText(txt, sizeof(txt), gTodos[i].text, 16, 128);
      drawText(74, ry + 1, txt, col, 16, 1, 0);
      // 期限短标签：今天/明天/N天后/逾期N天；点它可改期限
      char dlLab[20]; uint16_t dlCol = C_GREY;
      todoDeadlineLabel(gTodos[i].deadline, dlLab, sizeof(dlLab), dlCol);
      drawText(210, ry + 2, dlLab, dlCol, 16, 1, 0);   // FONT12 无中文，必须 16
      // 右侧删除圆键 ×
      fillCircle(302, ry + 9, 12, COL(74, 40, 40));
      circle(302, ry + 9, 12, COL(150, 80, 80), 1);
      line(298, ry + 5, 306, ry + 13, COL(240, 170, 170), 2);
      line(306, ry + 5, 298, ry + 13, COL(240, 170, 170), 2);
    }
  }
  // 底键：语音加 / 翻页（多页时）/ 清空——统一 UI 组件。
  // r30@(108/252,292) 外缘 163 越出安全区 158 → 上移到 y=285（外缘 157），
  // 同时热区从 r+4 收到 r+2，保证手指落点仍在圆屏内。
  uiBtn(108, 285, 30, "语音加", UI_G);
  if (pages > 1) uiBtn(180, 285, 30, "下一页", UI_N);
  bool armed = millis() < gTodoClearArmUntil;
  uiBtn(252, 285, 30, armed ? "再点" : "清空", UI_D);
}

// ---------------- 期限快捷选择（清单子页） ----------------
static bool todoSetQuickDeadline(int days) {
  if (gTodoDeadlineFor < 0 || gTodoDeadlineFor >= gTodoN) return false;
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  t.tm_hour = 23; t.tm_min = 59; t.tm_sec = 0;
  gTodos[gTodoDeadlineFor].deadline =
    days < 0 ? 0 : (uint32_t)(mktime(&t) + (time_t)days * 86400L);
  return todoCommit();
}
static void renderTodoDeadline() {
  uiPage("设置期限", COL(90, 150, 110));
  if (gTodoDeadlineFor < 0 || gTodoDeadlineFor >= gTodoN) return;
  char preview[28];
  fitText(preview, sizeof(preview), gTodos[gTodoDeadlineFor].text, 16, 232);
  drawText(180, 76, preview, C_WHITE, 16, 1, 1);
  char cur[20]; uint16_t cc = C_GREY;
  todoDeadlineLabel(gTodos[gTodoDeadlineFor].deadline, cur, sizeof(cur), cc);
  drawText(112, 104, "当前", cc, 16, 1, 0);
  drawText(152, 104, cur, cc, 16, 1, 0);

  // 五颗圆键：今天 / 明天 / 下周 / 下月 / 无期限
  static const int DX[5] = {104, 180, 256, 142, 218};
  static const int DY[5] = {158, 158, 158, 236, 236};
  static const char *const DL[5] = {"今天", "明天", "下周", "下月", "无期限"};
  static const int DD[5] = {0, 1, 7, 30, -1};
  for (int i = 0; i < 5; i++) {
    int st = (i == 4) ? UI_D : UI_N;   // 无期限=危险色；其余普通（当前项在下面另标）
    uiBtn(DX[i], DY[i], 28, DL[i], st);
  }
  drawText(180, 290, "点心仪选项即可", C_GREY, 16, 1, 1);
}

// ================================================================ 渲染调度
static void render() {
  if (gLocked) return;           // 息屏中：不画任何东西（背光已关，画了也看不见，白耗 CPU）
  // v2.8：进入背单词页自动读一遍当前词（开关开时）。只在页面切换那一帧触发，
  // 之后 250ms 的常规重绘不会再发音。
  static Page gLastRenderPage = P_ANALOG;
  if (curPage == P_WORDS && gLastRenderPage != P_WORDS) wordPronounce();
  gLastRenderPage = curPage;
  switch (curPage) {
    case P_ANALOG:   if (gFace) renderDigital(); else renderAnalog(); break;   // 主界面两块表盘
    case P_DIGITAL:  renderDigital(); break;   // 串口 PAGE 1 保留（UI 上已并入主界面）
    case P_MUSIC:    renderMusic(); break;
    case P_RADAR:    renderRadar(); break;
    case P_POMO:     renderPomo(); break;
    case P_WIFI:     renderWifi(); break;
    case P_SETTINGS: gCityList ? renderCityList() : renderSettings(); break;
    case P_APPS:     renderApps(); break;
    case P_CHAT:     renderChat(); break;
    case P_WORDS:    renderWords(); break;
    case P_MEMO:     renderMemo(); break;
    case P_PET:      renderPet(); break;
    case P_RECORDER: renderRecorder(); break;
    case P_TODO:     if (gTodoDeadlineFor >= 0) renderTodoDeadline(); else renderTodo(); break;
    default: break;
  }
  // v0.15：模拟表盘上的齿轮 / 锁键两个虚拟按钮全部去掉，表盘保持干净。
  //   - 设置：表盘上半部往下拉呼出，设置页任意方向滑动退出
  //   - 锁屏：只按右上实体 BOOT 键（GPIO0）切换；息屏后同样只由该键唤醒，
  //           触摸一律忽略（v0.17 防误触，不再"碰一下就亮"）
  // 图标函数仍留在下面备用，但不再绘制、也不再注册任何命中热区。
  drawToast();
  flushScreen();
}

// ================================================================ 串口调试命令
// PAGE <0..4> 强制切页；SHOT 把 360x360 RGB565 帧缓冲原样回传，PC 端可转 PNG 验收
bool cfgExtra(const String &raw) {
  String up = raw; up.toUpperCase();
  if (up == "WBOOK") {                 // 查看已记住密码的网络（只报长度，不报明文）
    wifiBookLoad();
    wifiBookDump();
    return true;
  }
  if (up == "WBOOKCLEAR") {
    gWifiBook = "";
    cfgSet("wbook", gWifiBook);
    Serial.println("[WIFI] password book cleared");
    return true;
  }
  if (up.startsWith("PAGE ")) {
    int n = raw.substring(5).toInt();
    if (n < 0) n = 0;
    curPage = (Page)(n % PAGE_COUNT);
    detailIdx = -1;
    render();
    Serial.printf("[UI] page -> %d\n", (int)curPage);
    return true;
  }
  // v2.1 验收：主界面两块表盘互换。FACE 切一下；FACE 0 / FACE 1 直接指定。
  if (up == "FACE" || up.startsWith("FACE ")) {
    if (up.startsWith("FACE ")) {
      gFace = raw.substring(5).toInt() ? 1 : 0;
      cfgSetInt("face", gFace);
      curPage = P_ANALOG;
      render();
    } else {
      toggleClockFace();
    }
    Serial.printf("[CFG] face -> %s (page %d)\n", gFace ? "DIGITAL" : "ANALOG", (int)curPage);
    return true;
  }
  // v2.2 验收：表盘语言。LANG 切一下；LANG 0 / LANG 1 直接指定（0=中文 1=English）。
  if (up == "LANG" || up.startsWith("LANG ")) {
    if (up.startsWith("LANG ")) {
      gLang = raw.substring(5).toInt() ? 1 : 0;
      cfgSetInt("lang", gLang);
      curPage = P_ANALOG;
      render();
    } else {
      toggleLang();
    }
    Serial.printf("[CFG] lang -> %s (page %d)\n", gLang ? "EN" : "CN", (int)curPage);
    return true;
  }
  // 验收/调试：强制星期（0=周日..6=周六），用于检查星期窗两种配色
  if (up.startsWith("WDAY ")) {
    gWday = constrain(raw.substring(5).toInt(), 0, 6);
    render();
    Serial.printf("[UI] wday -> %d (%s)\n", gWday, WDAY_EN[gWday]);
    return true;
  }
  if (up == "SHOT") {
    Serial.flush();
    // 抓帧期间暂停网络任务：netTask 的 [WX]/[ADSB] 日志会插进二进制流，
    // PC 端解码就会整体错位（看着像"表盘被切成两半"）。
    netBusy = true;
    delay(60);                  // 让已经在跑的 HTTP 打印收尾
    Serial.print("===FB_BEGIN===\n");
    // 一次写 259200B 会长时间占住 loop()（Core1），并发 TLS 时容易触发
    // Task WDT 复位。改成 8KB 分块 + yield，让 idle task 能喂狗。
    const uint8_t *p = (const uint8_t *)FB;
    size_t left = (size_t)SCREEN_W * SCREEN_H * 2;
    while (left) {
      size_t chunk = (left > 8192) ? 8192 : left;
      Serial.write(p, chunk);
      Serial.flush();
      yield();
      p += chunk; left -= chunk;
    }
    Serial.print("\n===FB_END===\n");
    netBusy = false;              // 抓完立刻放开网络任务
    return true;
  }
  if (up == "AP") {
    Serial.println("[CFG] opening provisioning AP ...");
    provisionStart();
    return true;
  }
  if (up == "WIFI") {
    enterWifiList(); render();
    Serial.println("[CFG] opened on-device Wi-Fi setup (async scan started)");
    return true;
  }
  if (up == "WIFIK") {                 // 直接进入键盘态（验收用，模拟已选网络）
    curPage = P_WIFI; gWifiState = W_KEY; gWifiSSID = "DEMO-NET";
    gWifiSecured = true; gWifiPass = ""; gWifiSym = false; gWifiCaps = false;
    render();
    Serial.println("[CFG] wifi keyboard (demo) shown");
    return true;
  }
  if (up == "TONE" || up.startsWith("TONE ")) {
    int f = 440, ms = 1500;
    if (up.startsWith("TONE ")) {
      String rest = raw.substring(5); rest.trim();
      int sp = rest.indexOf(' ');
      if (sp < 0) { f = rest.toInt(); }
      else { f = rest.substring(0, sp).toInt(); ms = rest.substring(sp + 1).toInt(); }
      if (f <= 0) f = 440;
      if (ms <= 0) ms = 1500;
    }
    audioToneRequest(f, ms);   // 由 audioTask 异步生成 WAV 并播放，不阻塞 UI
    Serial.printf("[CFG] TONE %dHz %dms queued\n", f, ms);
    return true;
  }
  // 方向翻页（与滑动等价，方便无触摸时串口调试）
  if (up == "UP")    { if (navigate('U')) render(); Serial.println("[CFG] nav UP");    return true; }
  if (up == "DOWN")  { if (navigate('D')) render(); Serial.println("[CFG] nav DOWN");  return true; }
  if (up == "LEFT")  { if (navigate('L')) render(); Serial.println("[CFG] nav LEFT");  return true; }
  if (up == "RIGHT") { if (navigate('R')) render(); Serial.println("[CFG] nav RIGHT"); return true; }
  if (up == "LOCK")  { toggleLockScreen();
                       Serial.printf("[CFG] lock -> %s\n", gLocked ? "SCREEN OFF" : "SCREEN ON");
                       return true; }
  // 模拟按一下实体 BOOT 键：走和真按键完全相同的处理函数，无手指也能验收
  if (up == "BTN")   { Serial.printf("[CFG] inject BOOT press (locked=%d)\n", (int)gLocked);
                       toggleLockScreen(); return true; }
  // 注入一次点击（走和真手指完全相同的命中判定，方便无触摸调试锁键/按钮）
  if (up.startsWith("TAP ")) {
    int sp = raw.indexOf(' ', 4);
    int tx = raw.substring(4, sp).toInt();
    int ty = raw.substring(sp + 1).toInt();
    Serial.printf("[CFG] inject tap %d,%d (locked=%d)\n", tx, ty, (int)gLocked);
    // v0.17：息屏后触摸不再唤醒，TAP 与真手指一致——锁屏时直接忽略。
    if (!gLocked) handleTap(tx, ty);
    else Serial.println("[CFG] screen locked: tap ignored (use BTN / LOCK to wake)");
    return true;
  }
  // 注入一次真实滑动手势（进触摸队列，走和手指完全相同的手势判定），
  // 用来无手指验收「任意页面下拉呼出设置」「设置页上推返回」。
  // 用法：SWIPE <x1> <y1> <x2> <y2>
  if (up.startsWith("SWIPE ")) {
    String rest = raw.substring(6); rest.trim();
    int a = rest.indexOf(' '), b = a < 0 ? -1 : rest.indexOf(' ', a + 1);
    int c = b < 0 ? -1 : rest.indexOf(' ', b + 1);
    if (a > 0 && b > a && c > b) {
      int x1 = rest.substring(0, a).toInt(), y1 = rest.substring(a + 1, b).toInt();
      int x2 = rest.substring(b + 1, c).toInt(), y2 = rest.substring(c + 1).toInt();
      Serial.printf("[CFG] inject swipe %d,%d -> %d,%d\n", x1, y1, x2, y2);
      injectSwipe(x1, y1, x2, y2);
    } else Serial.println("[CFG] usage: SWIPE <x1> <y1> <x2> <y2>");
    return true;
  }
  if (up == "SETTINGS") { enterSettings(); Serial.println("[CFG] settings page"); return true; }
  // v0.17：番茄钟专注时长档位（15 / 25 / 45 分钟），无手指也能验收
  if (up == "POMO" || up.startsWith("POMO ")) {
    int m = up.startsWith("POMO ") ? raw.substring(5).toInt() : -1;
    if (m == 15 || m == 25 || m == 45) {
      gPomoFocusMin = m;
      cfgSetInt("pomofocus", m);
      if (pomo.phase == PH_FOCUS && !pomo.running) pomo.remainMs = pomoPhaseMs();
      Serial.printf("[POMO] focus preset -> %d min\n", m);
      render();
    } else {
      Serial.printf("[POMO] current %d min (presets: 15 / 25 / 45), usage: POMO <15|25|45>\n",
                    gPomoFocusMin);
    }
    return true;
  }
  // v0.16：天气城市（同时决定 ADS-B 雷达的圆心）。支持序号与中文名。
  if (up == "CITY" || up.startsWith("CITY ")) {
    String q = up.startsWith("CITY ") ? raw.substring(5) : "";
    q.trim();
    if (!q.length()) {
      Serial.printf("[CITY] %d cities, current = %d %s\n", CITY_COUNT, gCityIdx + 1, CITIES[gCityIdx].name);
      for (int i = 0; i < CITY_COUNT; i++)
        Serial.printf("  %2d %-10s %.4f %.4f%s\n", i + 1, CITIES[i].name,
                      CITIES[i].lat, CITIES[i].lon, i == gCityIdx ? "  <-" : "");
    } else {
      int idx = -1;
      if (q.length() <= 2 && q[0] >= '0' && q[0] <= '9') idx = q.toInt() - 1;
      else for (int i = 0; i < CITY_COUNT; i++)
        if (q == String(CITIES[i].name)) { idx = i; break; }
      if (idx >= 0 && idx < CITY_COUNT) { citySelect(idx); render(); }
      else Serial.println("[CITY] usage: CITY | CITY <1..N> | CITY <中文名>");
    }
    return true;
  }
  if (up == "BRIGHT" || up.startsWith("BRIGHT ")) {
    gBright = (up == "BRIGHT") ? gBright + BRIGHT_STEP : raw.substring(7).toInt();
    if (gBright < BRIGHT_MIN) gBright = BRIGHT_MIN;
    if (gBright > BRIGHT_MAX) gBright = BRIGHT_MAX;
    Set_Backlight((uint8_t)gBright);
    cfgSetInt("bright", gBright);
    Serial.printf("[CFG] bright -> %d%%\n", gBright);
    if (curPage == P_SETTINGS) render();
    return true;
  }
  if (up == "VOL" || up.startsWith("VOL ")) {
    gVolume = (up == "VOL") ? gVolume + 3 : raw.substring(4).toInt();
    if (gVolume < 0) gVolume = 0;
    if (gVolume > VOL_MAX) gVolume = VOL_MAX;
    audioVolume(gVolume);
    cfgSetInt("vol", gVolume);
    Serial.printf("[CFG] volume -> %d/21\n", gVolume);
    if (curPage == P_SETTINGS) render();
    return true;
  }
  // ---- 音频三音源（电台 / TF 卡 / 播客）----
  if (up == "PLAY")  { audioToggle(); Serial.println("[CFG] audio play/pause requested"); return true; }
  if (up == "STOP")  { audioStop();  Serial.println("[CFG] audio stop requested");        return true; }
  if (up == "AUD" || up == "POD")  { audioStatusDump(); return true; }
  if (up == "SRC" || up.startsWith("SRC ")) {
    int s = (up == "SRC") ? ((int)audioSource() + 1) % 3 : raw.substring(4).toInt();
    s = constrain(s, 0, 2);
    audioSetSource(s);
    curPage = P_MUSIC; gMusicList = false; gMusicPage = 0; render();
    Serial.printf("[CFG] source -> %d (%s)\n", s, AUD_SRC_NAME[s]);
    return true;
  }
  if (up.startsWith("TRACK ")) {
    int i = raw.substring(6).toInt() - 1;
    audioSelect(i);
    Serial.printf("[CFG] select track %d\n", i + 1);
    return true;
  }
  if (up == "PET" || up.startsWith("PETSET ")) { petCmd(raw); return true; }
  if (up == "WORD" || up == "WORDRESET" || up == "WORDDUE" || up.startsWith("WORDSET ") ||
      up == "PRON" || up.startsWith("PRON ")) { wordCmd(raw); return true; }
  if (up == "MIC" || up == "MICPIN" || up.startsWith("MICTEST")) { micCmd(raw); return true; }
  // 录音机验收：REC / RECLIST / RECREC [秒] / RECPLAY [n] / RECSTOP / RECMIN [n] / RECDEL [n]
  if (up == "REC" || up == "RECLIST" || up.startsWith("RECREC") || up.startsWith("RECPLAY")
      || up == "RECSTOP" || up.startsWith("RECMIN") || up.startsWith("RECDEL")) {
    recCmd(raw); return true;
  }
  if (up == "XZCHAT" || up == "XZCLOSE" || up == "XZINFO" || up == "XZTEST" ||
      up.startsWith("XZAUTO")) {
    xzChatCmd(raw); return true;
  }
  if (up.startsWith("BAILIAN") || up.startsWith("XZTOKEN") || up == "XZBIND" || up == "XZSTATUS"
      || up.startsWith("TODO")
      || up == "AI" || up == "AICANCEL" || up.startsWith("QWEN ") || up == "ASRTODO"
      || up.startsWith("NET") || up == "MEM" || up.startsWith("MODEL")) { cloudCmd(raw); return true; }
  if (up == "TFSCAN") { audioRescanTF(); Serial.println("[CFG] TF rescan queued"); return true; }
  if (up == "TFTEST") {
    audioTfTest();
    Serial.println("[CFG] TF test tone queued (writes /music/test-tone.wav, then plays it)");
    return true;
  }
  if (up == "TFTESTCN") {
    audioTfTestCN();
    Serial.println("[CFG] TF Chinese-name test queued (writes /music/<中文歌名测试>.wav, then plays it)");
    return true;
  }
  if (up == "PODFEED" || up.startsWith("PODFEED ")) {
    String u = up.startsWith("PODFEED ") ? raw.substring(8) : "";
    u.trim();
    if (u.length() == 1 && u[0] >= '1' && u[0] <= '0' + POD_FEED_COUNT) {
      int i = u[0] - '1';
      audioPodFeedSelect(i);
      Serial.printf("[CFG] podcast feed -> %d/%d %s\n", i + 1, POD_FEED_COUNT, POD_FEEDS[i].name);
    } else {
      audioPodFeed(u.length() ? u.c_str() : nullptr);   // 无参 = 刷新当前（失败自动换源）
      Serial.printf("[CFG] podcast feed queued: %s\n", u.length() ? u.c_str() : "(current)");
    }
    return true;
  }
  if (up.startsWith("PODADD ")) {
    String rest = raw.substring(7); rest.trim();
    int sp = rest.indexOf(' ');
    if (sp > 0) {
      String nm = rest.substring(0, sp), url = rest.substring(sp + 1); url.trim();
      audioPodAdd(nm.c_str(), url.c_str());
      Serial.printf("[CFG] podcast episode added: %s\n", nm.c_str());
    } else Serial.println("[CFG] usage: PODADD <name> <url>");
    return true;
  }
  if (up == "PODCLEAR") { audioPodClear(); Serial.println("[CFG] podcast list cleared"); return true; }
  if (up == "BATF" || up.startsWith("BATF ")) {   // 验收用：强制电量显示，BATF -1 恢复真实读数
    int a = -1, b = 0;
    sscanf(up.c_str() + 4, "%d %d", &a, &b);
    if (a < 0) {
      gBatFakeSoc = -1; gBatFakeChg = -1;
      Serial.println("[BAT] fake cleared -> real reading");
    } else {
      gBatFakeSoc = constrain(a, 0, 100);
      gBatFakeChg = (b > 0) ? 1 : 0;
      Serial.printf("[BAT] fake soc=%d%% charging=%d (UI only; BATF -1 to clear)\n",
                    gBatFakeSoc, gBatFakeChg);
    }
    render();
    return true;
  }
  if (up == "BAT") {
    batteryPoll(true);
    Serial.printf("[BAT] BQ27220 @0x55 valid=%d soc=%d%% mv=%d ma=%d charging=%d "
                  "status=0x%04X rm=%dmAh fcc=%dmAh\n",
                  (int)gBat.valid, gBat.soc, gBat.mv, gBat.ma, (int)gBat.charging,
                  (unsigned)gBat.status, gBat.rm, gBat.fcc);
    if (!gBat.valid) Serial.println("[BAT] no ACK or battery voltage out of range -> UI shows NO BAT / USB");
    return true;
  }
  // v0.19：板载 PCF85063 状态 / 手动设时（没有网络也能把时间调准并写进 RTC）
  if (up == "RTC") {
    uint8_t b[7];
    if (!rtcReadBlock(b, 7)) {
      Serial.println("[RTC] PCF85063 @0x51 no ACK (not fitted / no I2C contact)");
      return true;
    }
    int s, mi, h, d, mo, y;
    bool ok = rtcDecode(b, s, mi, h, d, mo, y);
    Serial.printf("[RTC] raw 0x04..0x0A: sec=%02X min=%02X hour=%02X day=%02X wday=%02X mon=%02X year=%02X\n",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
    if (ok) Serial.printf("[RTC] %04d-%02d-%02d %02d:%02d:%02d  (usable)\n", y, mo, d, h, mi, s);
    else    Serial.println("[RTC] content not usable (out of range) -> needs a set (SETTIME / SNTP)");
    Serial.printf("[RTC] chip=%s valid=%s  soft=%04d-%02d-%02d %02d:%02d:%02d src=%s\n",
                  gRtcFound ? "FOUND" : "MISSING", gRtcValid ? "YES" : "NO",
                  gYear, gMon, gDay, gHour, gMin, gSec, gTimeSrc);
    return true;
  }
  if (up.startsWith("RTCD")) {              // RTCD <reg> [len] —— 原始寄存器 dump（十六进制）
    unsigned reg = 0, n = 7;
    int got = sscanf(raw.c_str() + 4, "%x %u", &reg, &n);
    if (got < 1) { Serial.println("[RTC] usage: RTCD <reg-hex> [len]"); return true; }
    if (n == 0 || n > 16) n = 16;
    uint8_t b[16];
    if (!rtcReadRegs((uint8_t)reg, b, (uint8_t)n)) { Serial.println("[RTC] read failed"); return true; }
    Serial.printf("[RTC] reg 0x%02X..0x%02X:", reg, reg + n - 1);
    for (unsigned i = 0; i < n; i++) Serial.printf(" %02X", b[i]);
    Serial.println();
    return true;
  }
  if (up.startsWith("RTCW")) {              // RTCW <reg> <b0> [b1..] —— 原始写寄存器（十六进制）
    unsigned reg = 0, bb[8], nb = 0;
    const char *p = raw.c_str() + 4;
    int used = 0;
    if (sscanf(p, "%x%n", &reg, &used) != 1) { Serial.println("[RTC] usage: RTCW <reg-hex> <b0> [b1..]"); return true; }
    p += used;
    while (nb < 8) {
      int u2 = 0;
      unsigned v;
      if (sscanf(p, "%x%n", &v, &u2) != 1) break;
      bb[nb++] = v & 0xFF; p += u2;
    }
    if (!nb) { Serial.println("[RTC] usage: RTCW <reg-hex> <b0> [b1..]"); return true; }
    uint8_t tmp[8];
    for (unsigned i = 0; i < nb; i++) tmp[i] = (uint8_t)bb[i];
    bool okw = rtcWriteBlock((uint8_t)reg, tmp, (uint8_t)nb);
    uint8_t chk[8];
    bool okr = rtcReadRegs((uint8_t)reg, chk, (uint8_t)nb);
    Serial.printf("[RTC] wrote 0x%02X..: %s | read back:", reg, okw ? "ok" : "FAIL");
    if (okr) for (unsigned i = 0; i < nb; i++) Serial.printf(" %02X", chk[i]);
    Serial.println();
    return true;
  }
  if (up.startsWith("SETTIME ")) {          // SETTIME 2026-11-15 23:30[:00]
    int Y, M, D, h, m, s = 0;
    int n = sscanf(raw.c_str() + 8, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s);
    bool ok = (n >= 5) && Y >= 2024 && Y <= 2099 && M >= 1 && M <= 12 &&
              D >= 1 && D <= daysInMonth(Y, M) && h >= 0 && h <= 23 && m >= 0 && m <= 59 &&
              s >= 0 && s <= 59;
    if (!ok) { Serial.println("[RTC] usage: SETTIME 2026-11-15 23:30[:00]"); return true; }
    gYear = Y; gMon = M; gDay = D; gHour = h; gMin = m; gSec = s;
    gWday = weekdayOf(Y, M, D);
    clockSetSystem();
    gClockSynced = true;
    gTimeSrc = "MANUAL";
    gRtcDirty = true;                       // 主循环写回 RTC
    Serial.printf("[RTC] manual set -> %04d-%02d-%02d %02d:%02d:%02d (write to RTC queued)\n",
                  gYear, gMon, gDay, gHour, gMin, gSec);
    render();
    return true;
  }
  if (up == "WX")   { lastWx = 0;   Serial.println("[CFG] weather refresh queued"); return true; }
  if (up == "ADSB") { lastAdsb = 0; Serial.println("[CFG] adsb refresh queued");    return true; }
  if (up == "WPASS") {
    // v0.21：只报「长度 + 是否含不可打印字符 + 是否有空格」，不打印明文。
    // 排障用：204 HANDSHAKE_TIMEOUT 且 SSID 能扫到时，先确认 NVS 里的密码没被
    // 屏上键盘输坏（全角字符/尾部空格/被截断）。
    int bad = 0, sp = 0, hi = 0;
    for (size_t i = 0; i < gCfg.pass.length(); i++) {
      uint8_t c = (uint8_t)gCfg.pass[i];
      if (c == ' ') sp++;
      else if (c < 0x21 || c > 0x7E) { bad++; if (c > 0x7E) hi++; }
    }
    Serial.printf("[WPASS] ssid='%s' len=%d | pass len=%d spaces=%d nonprint=%d nonascii=%d\n",
                  gCfg.ssid.c_str(), (int)gCfg.ssid.length(), (int)gCfg.pass.length(),
                  sp, bad, hi);
    Serial.printf("[WPASS] 密码 8~63 位才算合法 WPA2；若为 64 位则是 hex PSK\n");
    return true;
  }
  if (up == "SCAN") {
    // v0.21：先收掉残留的异步扫描。之前 P_WIFI 页会起 scanNetworks(true) 异步扫描，
    // 它在后台一直处于 RUNNING，这里直接同步扫只会拿到 -2(WIFI_SCAN_RUNNING)，
    // 结果就是「[SCAN] -2 networks」——排障时完全看不到周围到底有没有目标 SSID。
    // 另外 netTask 每 30s 会强连一次，连接中扫描同样必然失败，所以先按住它。
    gNetHold = true;
    // 关键：ESP-IDF 的 esp_wifi_scan_start() 在 STA 处于 connecting/connected 时
    // 直接返回 ESP_ERR_WIFI_STATE（Arduino 层翻译成 -2 = WIFI_SCAN_RUNNING），
    // 所以只要后台在重连，扫描就永远拿不到结果。必须先关射频再重开，等回 IDLE。
    WiFi.disconnect(true, false);
    delay(400);
    WiFi.mode(WIFI_STA);
    delay(300);
    for (int i = 0; i < 40 && WiFi.status() != WL_IDLE_STATUS; i++) delay(100);
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
      Serial.println("[SCAN] waiting for pending async scan ...");
      for (int i = 0; i < 60 && WiFi.scanComplete() == WIFI_SCAN_RUNNING; i++) delay(100);
    }
    WiFi.scanDelete();
    delay(50);
    int n = WiFi.scanNetworks(false, true);
    Serial.printf("[SCAN] %d networks (status=%d lastReason=%u %s)\n", n, (int)WiFi.status(),
                  (unsigned)gWifiLastReason,
                  WiFi.disconnectReasonName((wifi_err_reason_t)gWifiLastReason));
    int bestRssi = -127, bestCh = 0, bestEnc = -1;
    for (int i = 0; i < n && i < 24; i++) {
      const String &ss = WiFi.SSID(i);
      bool hit = (ss == gCfg.ssid);
      if (hit) { bestRssi = (int)WiFi.RSSI(i); bestCh = (int)WiFi.channel(i); bestEnc = (int)WiFi.encryptionType(i); }
      Serial.printf("  %2d  %-32s %4d dBm  ch%2d  %s%s\n", i, ss.c_str(),
                    (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN   " : "SECURED",
                    hit ? "  <== SAVED SSID" : "");
    }
    if (bestEnc >= 0)
      Serial.printf("[SCAN] saved '%s' found: %d dBm ch%d enc=%d "
                    "(0=OPEN 2=WEP 3=WPA_PSK 4=WPA2_PSK 5=WPA_WPA2 6=ENT 7=WPA3 8=WPA2_WPA3)\n",
                    gCfg.ssid.c_str(), bestRssi, bestCh, bestEnc);
    else if (gCfg.ssid.length())
      Serial.printf("[SCAN] saved SSID '%s' NOT in scan result -> 2.4G 没广播/频段被关/名字改了\n",
                    gCfg.ssid.c_str());
    WiFi.scanDelete();
    gNetHold = false;
    if (WiFi.status() != WL_CONNECTED) WiFi.begin(gCfg.ssid.c_str(), gCfg.pass.c_str());
    return true;
  }
  if (up == "PLANES") {
    Serial.printf("[PLANES] src=api.adsb.lol city=%s (%.4f,%.4f) radius=%dnm "
                  "total=%d LIVE=%d STALE=%d demo=0 last=%lus ago\n",
                  CITIES[gCityIdx].name, gHomeLat, gHomeLon, radiusNm,
                  planeCount, liveCount, planeCount - liveCount,
                  lastAdsbOkMs ? (unsigned long)((millis() - lastAdsbOkMs) / 1000UL) : 0);
    for (int i = 0; i < planeCount; i++) {
      Plane &p = planes[i];
      Serial.printf("  %2d %-9s %-5s brg=%3.0f r=%.2f dist=%4dkm FL%4d %4dkm/h %s\n",
                    i, p.call, p.type, p.bearing, p.r,
                    (int)(p.r * radiusNm * 1.852f), (int)(p.alt_m * 3.28084f / 100),
                    p.gs, p.stale ? "STALE" : "LIVE");
    }
    return true;
  }
  if (up == "STATUS") {
    Serial.println("---- runtime status ----");
    Serial.printf("  WiFi      : %s  ip=%s  rssi=%d dBm\n",
                  wifiOK ? "CONNECTED" : (gProvActive ? "AP-PROVISION" : "OFFLINE"),
                  wifiOK ? WiFi.localIP().toString().c_str() : "-",
                  wifiOK ? WiFi.RSSI() : 0);
    // v0.21：离线排障三件套 —— 状态码 / 断开原因 / 当前信道。
    // WiFi.status(): 0=IDLE 1=NO_SSID_AVAIL 4=CONNECT_FAILED 5=CONNECTION_LOST 6=DISCONNECTED
    // disconnectReason(): 201=NO_AP_FOUND 15/205=握手/超时 204=AP_NOT_FOUND(旧) 200=BEACON_TIMEOUT
    {
      int st = (int)WiFi.status();
      int rs = (int)gWifiLastReason;
      Serial.printf("  WiFi diag : status=%d lastReason=%d(%s) drops=%lu ch=%d mode=%d\n",
                    st, rs, WiFi.disconnectReasonName((wifi_err_reason_t)rs),
                    (unsigned long)gWifiDiscCount, (int)WiFi.channel(), (int)WiFi.getMode());
      Serial.printf("  WiFi ssid : '%s' saved, %d chars pass\n",
                    gCfg.ssid.c_str(), (int)gCfg.pass.length());
    }
    Serial.printf("  Clock     : %04d-%02d-%02d %02d:%02d:%02d  src=%s  boot=%s  NTP=%s\n",
                  gYear, gMon, gDay, gHour, gMin, gSec, gTimeSrc, gBootTimeSrc,
                  (strcmp(gTimeSrc, "NTP") == 0) ? "SYNCED" : "NOT-YET");
    Serial.printf("  RTC       : PCF85063 @0x51 %s  time=%s  lastWrite=%lus ago\n",
                  gRtcFound ? "FOUND" : "MISSING",
                  gRtcValid ? "VALID" : "INVALID",
                  gRtcLastWrite ? (unsigned long)((millis() - gRtcLastWrite) / 1000UL) : 0);
    Serial.printf("  Weather   : src=%s live=%d  %dC %s  %d/%dC\n",
                  wxSrc, (int)wx.live, wx.temp, wx.desc, wx.lo, wx.hi);
    Serial.printf("  ADSB      : src=adsb.lol city=%s LIVE=%d STALE=%d total=%d radius=%dnm (%d km) last=%lus ago\n",
                  CITIES[gCityIdx].name, liveCount, planeCount - liveCount, planeCount,
                  radiusNm, (int)(radiusNm * 1.852f),
                  lastAdsbOkMs ? (unsigned long)((millis() - lastAdsbOkMs) / 1000UL) : 0);
    Serial.printf("  City      : %d/%d %s (%.4f, %.4f)\n",
                  gCityIdx + 1, CITY_COUNT, CITIES[gCityIdx].name, gHomeLat, gHomeLon);
    const char *as = (gAudioState == A_IDLE) ? "IDLE" :
                     (gAudioState == A_CONNECTING) ? "CONNECTING" :
                     (gAudioState == A_PLAYING) ? "PLAYING" :
                     (gAudioState == A_PAUSED) ? "PAUSED" : "ERROR";
    // loops / buf 是客观健康指标：loops 持续增长 = 喂流任务没被饿死，
    // buf > 0 = InBuffer 有数据。旧版只看 isRunning()，静音时也会显示 YES。
    Serial.printf("  Audio     : codec=%s state=%s src=%s %d/%d running=%s loops=%u buf=%u pos=%us/%us\n",
                  gAudioReady ? "OK" : "NO", as, AUD_SRC_NAME[audioSource()],
                  audioIndex() + 1, audioCount((int)audioSource()),
                  (gAudioReady && gAudio->isRunning()) ? "YES" : "NO",
                  (unsigned)gAudLoops, (unsigned)gAudBuf,
                  (unsigned)audioElapsed(), (unsigned)audioDuration());
    Serial.printf("  Library   : radio=%d tf=%d pod=%d vol=%d/21\n",
                  STREAM_COUNT, audioCount(AS_TF), audioCount(AS_POD), gVolume);
    if (gBat.valid) {
      Serial.printf("  Battery   : %d%%  %dmV  %dmA  %s (BQ27220)\n",
                    gBat.soc, gBat.mv, gBat.ma, batteryStateText());
    } else {
      Serial.println("  Battery   : NO BAT / USB (BQ27220 not responding)");
    }
    Serial.printf("  Lock      : %s\n", gLocked ? "LOCKED" : "unlocked");
    Serial.printf("  Button    : GPIO%d=%d (BOOT, low=pressed)\n", BTN_PIN, digitalRead(BTN_PIN));
    Serial.printf("  Display   : bright=%d%%  volume=%d/21\n", gBright, gVolume);
    Serial.printf("  Page      : %d\n", (int)curPage);
    Serial.printf("  Face      : %s (page %d)\n", gFace ? "DIGITAL" : "ANALOG", (int)curPage);
    Serial.printf("  Lang      : %s\n", gLang ? "EN" : "CN");
    Serial.printf("  Provision : %s\n", gProvActive ? "AP 192.168.4.1" : "off");
    Serial.printf("  Uptime    : %lu s   reset=%d\n",
                  (unsigned long)(millis() / 1000), (int)esp_reset_reason());
    Serial.println("------------------------");
    return true;
  }
  return false;
}

// ================================================================ 触摸
static void handleTap(int x, int y) {
  // v0.15：屏幕虚拟锁键 / 齿轮热区已全部移除，避免「看不见却点得到」的隐形按钮。
  // 息屏改用实体 BOOT 键（pollButton），设置改用任意页下拉手势。
  // 设置页：返回 / Wi-Fi / 亮度 / 音量
  if (curPage == P_SETTINGS) { settingsTap(x, y); return; }
  switch (curPage) {
    case P_ANALOG:
      // v2.1：轴心 = **切换表盘**（模拟 / 数字）。进应用中心靠左右滑（和数字表盘同一套手势），
      // 轴心不再承担"进应用"，两个动作互不干扰：点=换脸，滑=换层。
      if (hit(x, y, 180, 180, 34)) { toggleClockFace(); return; }
      if (gFace == 0) {
        // 点 6 点天气副盘 = 立刻重拉真实天气
        if (hit(x, y, 180, 248, 46)) {
          lastWx = 0; showToast("WEATHER REFRESH");
          Serial.println("[UI] weather refresh requested by tap");
        }
      } else {
        // 数字表盘：没有副盘，点任意处即重拉天气（旧 P_DIGITAL 的行为原样保留）
        lastWx = 0; showToast("WEATHER REFRESH");
        Serial.println("[UI] weather refresh requested by tap");
      }
      break;
    case P_DIGITAL:
      // 串口 PAGE 1 进来的数字表盘：同样点轴心换回模拟
      if (hit(x, y, 180, 180, 34)) { toggleClockFace(); return; }
      lastWx = 0;
      showToast("WEATHER REFRESH");
      Serial.println("[UI] weather refresh requested by tap");
      break;
    case P_MUSIC:
      if (gMusicList) {                         // ---- 曲库浮层 ----
        if (x >= 141 && x <= 219 && y >= 288 && y <= 310) {
          int n = audioCount((int)audioSource());
          int pages = (n + MUSIC_LIST_ROWS - 1) / MUSIC_LIST_ROWS; if (pages < 1) pages = 1;
          gMusicPage = (gMusicPage + 1) % pages; render(); return;
        }
        for (int r = 0; r < MUSIC_LIST_ROWS; r++) {
          int ry = MUSIC_LIST_Y0 + r * MUSIC_LIST_ROWH;
          if (x >= 60 && x <= 300 && y >= ry && y <= ry + 32) {
            int idx = gMusicPage * MUSIC_LIST_ROWS + r;
            if (idx < audioCount((int)audioSource())) {
              audioSelect(idx); trackProgress = 0; gMusicList = false;
            }
            render(); return;
          }
        }
        break;
      }
      // 顶部三颗音源圆（v0.27）：中心 (125,56)/(180,44)/(235,56)，图形 r=23、热区 r=30
      if (y <= 86) {
        static const int TABXY[3][2] = {{125, 56}, {180, 44}, {235, 56}};
        for (int i = 0; i < 3; i++) {
          if (hit(x, y, TABXY[i][0], TABXY[i][1], 30)) {
            audioSetSource(i);
            gMusicPage = 0; trackProgress = 0; render(); return;
          }
        }
      }
      if (hit(x, y, 180, 180, 58)) {            // 超大播放/暂停键（图形 r52，热区 r58）
        audioToggle();
      } else if (hit(x, y, 72, 180, 37)) {      // 上一曲 / 上一台（图形 r33，热区 r37）
        audioNext(-1); trackProgress = 0;
      } else if (hit(x, y, 288, 180, 37)) {     // 下一曲 / 下一台
        audioNext(1);  trackProgress = 0;
      } else if (y >= 86 && y <= 112 && x >= 60 && x <= 300) {  // 点曲名开曲库
        gMusicList = true; gMusicPage = 0; render(); return;
      } else if (hit(x, y, 118, 292, 32)) {     // 曲库（图形 r25，热区 r32）
        gMusicList = true; gMusicPage = 0; render(); return;
      } else if (hit(x, y, 242, 292, 32)) {     // 重扫 / 更新 / 切台
        int s = (int)audioSource();
        if      (s == AS_TF)  { audioRescanTF();        showToast("TF RESCAN..."); }
        else if (s == AS_POD) {
          audioPodFeedNext();          // 循环切换下一条小宇宙中文播客（失败自动换源）
          showToast("NEXT PODCAST FEED");
        }
        else                  { audioNext(1); trackProgress = 0; }
        render(); return;
      }
      break;
    case P_RADAR:
      // 半径档位胶囊 v0.26 = (62,70,64,26)，中心 (94,83)；命中半径盖住整块再外扩 4px
      if (hit(x, y, 94, 83, 38)) {
        int n = sizeof(RANGE_OPTIONS) / sizeof(RANGE_OPTIONS[0]);
        int oldRadius = radiusNm;
        rangeIdx = (rangeIdx + 1) % n;
        radiusNm = RANGE_OPTIONS[rangeIdx];
        detailIdx = -1;
        // 只按半径比例重投影现有真实目标，不清空、不塞演示飞机
        for (int i = 0; i < planeCount; i++) {
          planes[i].r = planes[i].r * oldRadius / radiusNm;
          if (planes[i].r > 1.0f) planes[i].r = 0.98f;
        }
        lastAdsb = 0;        // 立刻排队一次真实拉取，1 秒内换成新半径的实时数据
      } else {
        int best = -1; long bestD = 24L * 24L;
        for (int i = 0; i < planeCount; i++) {
          int px, py; polar(180, 180, (int)(planes[i].r * 158), planes[i].bearing, px, py);
          long d = (long)(x - px) * (x - px) + (long)(y - py) * (y - py);
          if (d < bestD) { bestD = d; best = i; }
        }
        detailIdx = (best >= 0) ? (detailIdx == best ? -1 : best) : -1;
      }
      break;
    case P_POMO:
      // v0.27：三档改圆按钮，拱形排布（图形 r=19，中心 (131,58)/(180,48)/(229,58)）。
      // 热区 r=24：左中圆心距 50 > 48，三块热区互不重叠。
      if (y <= 84) {
        static const int PX[3] = {131, 180, 229};
        static const int PY[3] = {58, 48, 58};
        for (int i = 0; i < POMO_PRESET_COUNT && i < 3; i++) {
          if (hit(x, y, PX[i], PY[i], 24)) {
            gPomoFocusMin = POMO_PRESETS[i];
            cfgSetInt("pomofocus", gPomoFocusMin);
            if (pomo.phase == PH_FOCUS && !pomo.running) pomo.remainMs = pomoPhaseMs();
            Serial.printf("[POMO] focus preset -> %d min\n", gPomoFocusMin);
            render(); return;
          }
        }
      }
      if (hit(x, y, 180, 264, 36)) { pomo.running = !pomo.running; pomo.lastMs = millis(); }
      else if (hit(x, y, 102, 264, 28)) {
        pomo.remainMs = pomoPhaseMs(); pomo.running = false; pomo.alarm = false;
      } else if (hit(x, y, 258, 264, 28)) {
        pomoNext(); pomo.alarm = false;
      }
      break;
    case P_APPS:
      for (int i = 0; i < APP_N; i++) {
        if (!hit(x, y, APP_X[i], APP_Y[i], APP_HIT_R)) continue;
        if (APP_GLYPH[i] < 0) {                 // 预留位：只提示，不跳页
          showToast("预留位 / 待扩充"); render(); return;
        }
        curPage = APP_PAGE[i];
        if (curPage == P_SETTINGS) gSettingsFrom = P_APPS;
        render(); return;
      }
      break;
    case P_CHAT:
      if (gXzPageOpen) {
        auto state = gXzState.load();
        if (hit(x, y, 180, 260, 36) && state != XzBinding::Waiting) xzRequestBinding();
        render(); return;
      }
      if (hit(x, y, 266, 258, 29)) {
        gXzPageOpen = true;
        auto state = gXzState.load();
        if (state != XzBinding::Waiting && state != XzBinding::Bound) xzRequestBinding();
        render(); return;
      }
      if (hit(x, y, 180, 288, 44)) { xzChatToggle(); render(); }
      break;
    case P_MEMO:
      if (hit(x, y, 138, 250, 52)) { aiVoiceToggle(AVM_MEMO); render(); }
      else if (hit(x, y, 242, 250, 42)) {
        gMemoBackToTodo = false; curPage = P_TODO; gTodoPage = 0; render();
      }
      break;
    case P_RECORDER:
      if (gRecListOpen) { recListTap(x, y); return; }
      if (gAiState == AVS_PROCESSING) {
        if (gAiMode == AVM_MEETING && hit(x, y, 248, 292, 32)) gAiCancel.store(true);
        else showToast("语音任务进行中");
        break;
      }
      if (hit(x, y, 180, 286, 42)) { recToggleRecord(); return; }
      if (hit(x, y, 112, 292, 32)) { recTogglePlay(); render(); return; }
      if (hit(x, y, 248, 292, 32)) { recMakeMinutes(); render(); return; }
      // 点文件名那一行 = 打开录音列表（挑别的段来回放）
      if (y >= 126 && y <= 176) { gRecListPage = gRecSel / REC_ROWS; gRecListOpen = true; render(); return; }
      break;
    case P_TODO: {
      // 期限子页：滑动关闭（pollTouch），五颗圆键写期限
      if (gTodoDeadlineFor >= 0) {
        static const int DX[5] = {104, 180, 256, 142, 218};
        static const int DY[5] = {158, 158, 158, 236, 236};
        static const int DD[5] = {0, 1, 7, 30, -1};
        for (int i = 0; i < 5; i++) {
          if (hit(x, y, DX[i], DY[i], 34)) {
            if (todoSetQuickDeadline(DD[i])) showToast(DD[i] < 0 ? "已清除期限" : "期限已设置");
            gTodoDeadlineFor = -1;
            render(); return;
          }
        }
        break;
      }
      if (hit(x, y, 108, 285, 38)) {                   // 语音加：进备忘页，置闭环标志
        if (gAiState == AVS_RECORDING || gAiState == AVS_PROCESSING) {
          showToast("语音任务进行中"); render(); return;
        }
        gMemoBackToTodo = true; gMemoLastAdded = 0; gTodoNewFrom = -1;
        gAiTranscript = ""; gAiResult = ""; gAiState = AVS_IDLE;
        curPage = P_MEMO; render();
      } else if (hit(x, y, 252, 285, 38)) {            // 清空：3s 内二次确认
        if (gTodoN > 0 && millis() < gTodoClearArmUntil) {
          gTodoN = 0;
          if (todoCommit()) showToast("已清空");
          gTodoPage = 0; gTodoClearArmUntil = 0;
          gMemoLastAdded = 0; gTodoNewFrom = -1;
        } else if (gTodoN > 0) {
          gTodoClearArmUntil = millis() + 3000; showToast("再点一次确认清空");
        }
        render();
      } else if (gTodoN > 0) {
        int pages = (gTodoN + TODO_PER_PAGE - 1) / TODO_PER_PAGE;
        if (pages > 1 && hit(x, y, 180, 285, 38)) {    // 翻页
          gTodoPage = (gTodoPage + 1) % pages; render();
        } else {                                       // 条目行：左侧勾选 / 右侧删除
          for (int k = 0; k < TODO_PER_PAGE; k++) {
            int i = gTodoPage * TODO_PER_PAGE + k;
            if (i >= gTodoN) break;
            int ry = 92 + k * 31;
            if (y >= ry - 2 && y <= ry + 20 && x >= 44 && x <= 320) {
              if (x >= 288) { todoDel(i); }            // 右侧 × 区
              else if (x >= 212) {                      // 期限标签区 → 进快捷选择
                gTodoDeadlineFor = i; render(); return;
              } else { todoToggle(i); }                // 左侧勾选区
              gMemoLastAdded = 0; gTodoNewFrom = -1;   // 一操作就清高亮
              if (gTodoPage >= (gTodoN + TODO_PER_PAGE - 1) / TODO_PER_PAGE && gTodoPage > 0)
                gTodoPage--;                           // 末页删空则退回上一页
              render(); return;
            }
          }
        }
      }
      break;
    }
    case P_WORDS:
      wordTap(x, y);
      break;
    case P_PET:
      petTap(x, y);
      break;
    case P_WIFI: {
      if (gWifiState == W_LIST) {
        // RESCAN 按钮（回设置页靠左右滑，见 pollTouch）
        if (x >= 141 && x <= 219 && y >= 284 && y <= 314) {
          wifiScanStart(); render(); return;
        }
        // 网络行点选
        if (!gWifiScanning && gWifiN > 0) {
          int rowH = 34, top = 64, visible = 6;
          for (int i = 0; i < visible; i++) {
            int idx = i + gWifiScroll; if (idx >= gWifiN) break;
            int ry = top + i * rowH;
            if (x >= 26 && x <= 334 && y >= ry && y <= ry + rowH - 4) {
              gWifiSel = idx; gWifiSSID = String(gNetSSID[idx]);
              gWifiSecured = (gNetEnc[idx] != WIFI_AUTH_OPEN);
              gWifiSym = false; gWifiCaps = false; gWifiPass = "";
              // 密码簿里有的网络：自动把密码填进去，用户只需点 JOIN（也可改了重输）
              String bkPass;
              gWifiPassFromBook = gWifiSecured && wifiBookFind(gWifiSSID, bkPass);
              if (gWifiPassFromBook) gWifiPass = bkPass;
              Serial.printf("[WIFI] pick '%s' secured=%d fromBook=%d\n",
                            gWifiSSID.c_str(), (int)gWifiSecured, (int)gWifiPassFromBook);
              gWifiState = W_KEY; render(); return;
            }
          }
        }
      } else {
        // 键盘态：命中最接近的按键
        String act = wifiKeyHit(x, y);
        if (act.length()) {
          if      (act == "BKSP")   { if (gWifiPass.length()) gWifiPass.remove(gWifiPass.length() - 1); gWifiPassFromBook = false; }
          else if (act == "CAPS")   { gWifiCaps = !gWifiCaps; }
          else if (act == "SYM")    { gWifiSym = !gWifiSym; gWifiCaps = false; }
          else if (act == "CANCEL") { gWifiState = W_LIST; gWifiPassFromBook = false; }
          else if (act == "SPACE")  { if (gWifiPass.length() < 32) gWifiPass += ' '; gWifiPassFromBook = false; }
          else if (act == "JOIN")   { wifiConnect(); return; }
          else {
            if (gWifiPass.length() < 32) {
              char ch = act.charAt(0);
              if (!gWifiSym && gWifiCaps && ch >= 'a' && ch <= 'z') ch -= 32;
              gWifiPass += ch;
              gWifiPassFromBook = false;     // 手动改过密码，就不再标"已保存"
            }
          }
          render(); return;
        }
      }
      break;
    }
    default: break;
  }
}

// ================================================================ 触摸：自己做手势判定
// 芯片内置手势引擎对轻扫不敏感，且原来每个中断只读一帧、采样率还跟着 UI 重绘走
// （表盘 4fps），一次快速滑动可能整帧被漏掉 —— 这就是"滑动不大灵"。
// 现在改成独立任务 100Hz 读原始坐标，抬手时按位移自己判定方向，阈值也更宽松。
enum TouchEv : uint8_t { TE_NONE = 0, TE_TAP, TE_UP, TE_DOWN, TE_LEFT, TE_RIGHT };
struct TouchEvent { uint8_t type; int16_t x, y; int16_t dx, dy; uint16_t dt; };
static QueueHandle_t gTouchQ = nullptr;

#define TOUCH_SWIPE_PX 26      // 主方向位移 ≥26px 即算滑动（原芯片引擎阈值高得多）
#define TOUCH_TAP_PX   18      // 总位移 ≤18px 算点击
#define TOUCH_SWIPE_MS 1400    // 慢慢拖也算滑动
#define TOUCH_TAP_MS   600

static void touchTask(void *) {
  bool down = false;
  int sx = 0, sy = 0, lx = 0, ly = 0;
  uint32_t tDown = 0;
  for (;;) {
    // 电量计与触摸共用 Wire；放在同一任务里串行读，避免跨任务 I2C 事务互相打断
    batteryPoll();
    uint8_t pts = 0; uint16_t rx = 0, ry = 0;
    if (!Touch_PollRaw(&pts, &rx, &ry)) {   // 读失败：跳过本帧，别把总线抖动当成抬手
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    uint32_t now = millis();
    if (pts > 0) {
      if (!down) { down = true; sx = rx; sy = ry; tDown = now; }
      lx = rx; ly = ry;
    } else if (down) {
      down = false;
      int dx = lx - sx, dy = ly - sy;
      int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
      int m = adx > ady ? adx : ady;
      uint32_t dt = now - tDown;
      TouchEvent ev = {TE_NONE, (int16_t)sx, (int16_t)sy, (int16_t)dx, (int16_t)dy, (uint16_t)dt};
      if (m >= TOUCH_SWIPE_PX && dt <= TOUCH_SWIPE_MS) {
        if (adx > ady) ev.type = (dx > 0) ? TE_RIGHT : TE_LEFT;
        else           ev.type = (dy > 0) ? TE_DOWN  : TE_UP;
      } else if (m <= TOUCH_TAP_PX && dt <= TOUCH_TAP_MS) {
        ev.type = TE_TAP;
      }
      if (ev.type != TE_NONE && gTouchQ) xQueueSend(gTouchQ, &ev, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(10));      // 100Hz 采样：快速轻扫也不会漏
  }
}

static bool isSettingsPull(const TouchEvent &ev) {
  int adx = ev.dx < 0 ? -ev.dx : ev.dx;
  return ev.type == TE_DOWN && ev.y <= PULL_START_Y &&
         ev.dy >= PULL_MIN_DY && (int)ev.dy * 2 > adx * 3;
}
static void injectSwipe(int x1, int y1, int x2, int y2) {
  if (!gTouchQ) return;
  int dx = x2 - x1, dy = y2 - y1;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  TouchEvent ev = {TE_NONE, (int16_t)x1, (int16_t)y1, (int16_t)dx, (int16_t)dy, 300};
  if (adx > ady) ev.type = (dx > 0) ? TE_RIGHT : TE_LEFT;
  else           ev.type = (dy > 0) ? TE_DOWN  : TE_UP;
  xQueueSend(gTouchQ, &ev, 0);
}

static void pollTouch() {
  if (!gTouchQ) return;
  TouchEvent ev;
  while (xQueueReceive(gTouchQ, &ev, 0) == pdTRUE) {
    // v0.17：息屏中忽略所有触摸，只允许实体 BOOT 键唤醒（防误触）。
    // 以前任意触摸都会亮屏，放口袋里蹭一下就唤醒；现在触摸直接丢弃事件。
    if (gLocked) {
      continue;
    }
    // v2.0 导航：除表盘外所有入口都在应用中心里。
    // 下拉呼出设置只在表盘上生效（应用里下拉 = 返回应用中心，别抢手势）。
    if (isSettingsPull(ev) && curPage == P_ANALOG) {
      Serial.printf("[UI] pulldown -> settings (from page %d)\n", (int)curPage);
      enterSettings();
      continue;
    }
    // 子页/浮层优先关掉，再交给导航（一次滑动只做一件事，不会"跳过一层"）
    if (ev.type != TE_TAP) {
      if (curPage == P_TODO && gTodoDeadlineFor >= 0) {
        gTodoDeadlineFor = -1; Serial.println("[UI] subpage close -> todo"); render(); continue;
      }
      if (curPage == P_SETTINGS && gCityList) {
        gCityList = false; Serial.println("[UI] subpage close -> settings"); render(); continue;
      }
      if (curPage == P_RECORDER && gRecListOpen) {
        gRecListOpen = false; Serial.println("[UI] subpage close -> recorder"); render(); continue;
      }
      if (curPage == P_WORDS && wordPicking) {
        wordPicking = false; Serial.println("[UI] subpage close -> words"); render(); continue;
      }
      // 小智绑定子页：和其它子页同一约定，滑一下只关子页，不跳层
      if (curPage == P_CHAT && gXzPageOpen) {
        gXzPageOpen = false; Serial.println("[UI] subpage close -> chat"); render(); continue;
      }
    }
    // v2.8：设置页 / 配网页不在"应用中心"那一层，滑动直接回上一层。
    // 不拦的话下面的 navigate 会把用户甩到应用中心（设置来自表盘、配网来自设置）。
    if (ev.type != TE_TAP && curPage == P_SETTINGS) { exitSettings(); continue; }
    if (ev.type != TE_TAP && curPage == P_WIFI) {
      if (gWifiState == W_KEY) {                        // 键盘态：任意滑动 = 取消输入回列表
        gWifiState = W_LIST; gWifiPassFromBook = false;
        Serial.println("[UI] wifi keypad canceled (gesture)");
        render(); continue;
      }
      if (ev.type == TE_LEFT || ev.type == TE_RIGHT) {  // 列表态：上下滑留给列表滚动
        curPage = P_SETTINGS; gWifiScroll = 0; gWifiSel = -1;
        Serial.println("[UI] wifi -> settings (gesture)");
        render(); continue;
      }
    }
    // 曲库浮层：左右滑 = 退出（返回键已撤），上下滑 = 翻页
    if (curPage == P_MUSIC && gMusicList && ev.type != TE_TAP) {
      if (ev.type == TE_LEFT || ev.type == TE_RIGHT) {
        gMusicList = false; Serial.println("[UI] library closed (gesture)"); render(); continue;
      }
      int n = audioCount((int)audioSource());
      int pages = (n + MUSIC_LIST_ROWS - 1) / MUSIC_LIST_ROWS; if (pages < 1) pages = 1;
      bool fwd = (ev.type == TE_UP);
      gMusicPage = (gMusicPage + (fwd ? 1 : pages - 1)) % pages;
      Serial.printf("[UI] library page -> %d/%d\n", gMusicPage + 1, pages);
      render();
      continue;
    }
    switch (ev.type) {
      case TE_TAP:
        Serial.printf("[UI] tap %d,%d d=%d,%d dt=%d page=%d\n",
                      ev.x, ev.y, ev.dx, ev.dy, (int)ev.dt, curPage);
        handleTap(ev.x, ev.y);
        break;
      case TE_UP:
        Serial.printf("[UI] swipe UP   d=%d,%d dt=%d page=%d\n", ev.dx, ev.dy, (int)ev.dt, curPage);
        // Wi-Fi 列表里上滑 = 列表滚动（唯一的例外，手势让位给列表）
        if (curPage == P_WIFI && gWifiState == W_LIST) { wifiScrollBy(1); render(); }
        else if (curPage == P_ANALOG) toggleClockFace();   // 表盘上滑 = 换表盘（上滑在本页无 home 可回）
        else if (navigate('U')) render();       // 其余页面：上滑 = 回表盘（home）
        break;
      case TE_DOWN:
        Serial.printf("[UI] swipe DOWN d=%d,%d dt=%d page=%d\n", ev.dx, ev.dy, (int)ev.dt, curPage);
        if (curPage == P_WIFI && gWifiState == W_LIST) { wifiScrollBy(-1); render(); }
        else if (navigate('D')) render();
        break;
      case TE_LEFT:
        Serial.printf("[UI] swipe LEFT d=%d,%d dt=%d page=%d\n", ev.dx, ev.dy, (int)ev.dt, curPage);
        if (navigate('L')) render();
        break;
      case TE_RIGHT:
        Serial.printf("[UI] swipe RIGHT d=%d,%d dt=%d page=%d\n", ev.dx, ev.dy, (int)ev.dt, curPage);
        if (navigate('R')) render();
        break;
      default: break;
    }
  }
}

// ================================================================ setup / loop
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n==== RoundClock ESP32-S3 1.85B ====");
  static const char *RR[] = {"UNKNOWN", "POWERON", "EXT", "SW", "PANIC",
                             "INT_WDT", "TASK_WDT", "WDT", "DEEPSLEEP",
                             "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE",
                             "PWR_GLITCH", "CPU_LOCKUP"};
  int rr = (int)esp_reset_reason();
  Serial.printf("[BOOT] reset reason = %d (%s)\n", rr,
                (rr >= 0 && rr < 16) ? RR[rr] : "?");
  Serial.printf("Flash %u MB  PSRAM %u bytes  free heap %u\n",
                ESP.getFlashChipSize() / 1048576, ESP.getPsramSize(), ESP.getFreeHeap());
  cfgLoad();                  // NVS 覆盖 config.h 默认值
  cloudLoadCreds();           // 百炼凭据（必须在这里读，全局构造时 NVS 还没 init）
  petInit();                  // v1.0 电子宠物：从 NVS 读回状态
  wordInit();                 // v1.0 背单词：读回词书与进度
  todoLoad();                 // v1.0 云端 AI：读回 Todo 清单
  // v0.16：天气/雷达城市（NVS city），默认上海；开机即生效，不用等网络
  gCityIdx = constrain(cfgGetInt("city", 0), 0, CITY_COUNT - 1);
  gHomeLat = CITIES[gCityIdx].lat;
  gHomeLon = CITIES[gCityIdx].lon;
  initSoftClockFromBuild();   // 离线时的起点 = 固件编译时刻，不再是写死的旧时间
  wifiBookLoad();             // 记住的 Wi-Fi 密码（输过一次就不用再输）
  // 已经配在 config.h / 串口里的网络也进密码簿：换回它时列表直接带绿勾，不用重输
  if (gCfg.ssid.length() && gCfg.pass.length() >= 8) wifiBookPut(gCfg.ssid, gCfg.pass);
  cfgShow();

  I2C_Init();
  batteryPoll(true);         // v0.16：开机探测 BQ27220 并打印一次真实读数
  Serial.printf("[BOOT] BQ27220 @0x55: %s soc=%d%% %dmV %dmA %s\n",
                gBat.valid ? "OK" : "NOT FOUND / NO BAT", gBat.soc, gBat.mv, gBat.ma,
                batteryStateText());
  // v0.19：板载 PCF85063 RTC —— 有电池时断网开机时间也是准的，盖掉"编译时刻"兜底。
  // RTC 读不出可信时间（没装备用电池/从没设过）时，退回 NVS 里最后一次对好的时间。
  setenv("TZ", "CST-8", 1);
  tzset();
  if (!rtcBegin()) restoreClockFromNvs();
  clockSetSystem();
  sntp_set_time_sync_notification_cb(onNtpSync);
  Serial.printf("[TIME] boot time source = %s\n", gBootTimeSrc);
  Backlight_Init();
  // 亮度 / 音量从 NVS 读回（设置页里改过的话掉电保留）
  gBright = cfgGetInt("bright", BACKLIGHT_LEVEL);
  gVolume = cfgGetInt("vol", 16);
  gFace   = cfgGetInt("face", 0) ? 1 : 0;     // 主界面表盘：0 模拟 / 1 数字（NVS 记住上次选择）
  gLang   = cfgGetInt("lang", 0) ? 1 : 0;     // 表盘语言：0 中文 / 1 English
  Serial.printf("[UI] clock face = %s\n", gFace ? "DIGITAL" : "ANALOG");
  if (gBright < BRIGHT_MIN) gBright = BRIGHT_MIN;
  if (gBright > BRIGHT_MAX) gBright = BRIGHT_MAX;
  if (gVolume < 0) gVolume = 0;
  if (gVolume > VOL_MAX) gVolume = VOL_MAX;
  Set_Backlight((uint8_t)gBright);
  LCD_Init();                 // ST77916 QSPI + CST816S
  gfxInit();
  Serial.println("[BOOT] display + touch ready");
  audioInit();                // ES8311 + I2S（与触摸共用 I²C 总线）
  audioVolume(gVolume);       // 应用保存的音量（异步，由 audioTask 执行）
  // 右上实体 BOOT 键：板上已有上拉，按下为低。开机时按住会进下载模式（芯片自带行为）
  pinMode(BTN_PIN, INPUT_PULLUP);
  Serial.printf("[BOOT] BOOT button on GPIO%d ready (level=%d)\n", BTN_PIN, digitalRead(BTN_PIN));
  // 触摸采样任务：100Hz 读原始坐标，自己做手势判定
  gTouchQ = xQueueCreate(8, sizeof(TouchEvent));
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 3, nullptr, 1);
  Serial.println("[BOOT] type HELP for serial configuration");

  // v0.16：开机不再预填演示飞机；首帧雷达为空 / NO SIGNAL，
  // 联网拿到 adsb.lol 真实响应后才画目标（通常开机 3~8 秒内）。
  // v0.17：番茄钟专注时长档位从 NVS 读回（15/25/45），非法值回落默认 25
  gPomoFocusMin = cfgGetInt("pomofocus", POMO_FOCUS_MIN);
  if (gPomoFocusMin != 15 && gPomoFocusMin != 25 && gPomoFocusMin != 45)
    gPomoFocusMin = POMO_FOCUS_MIN;
  pomo.remainMs = (uint32_t)gPomoFocusMin * 60000UL;
  pomo.lastMs = millis();

  // 首帧立刻出画，不等 Wi-Fi
  render();
  // 未配网时提示用屏幕左上角 Wi-Fi 图标本机配网（脱离手机/电脑）
  if (gCfg.ssid.length() == 0) showToast("PULL DOWN -> WIFI SETUP");
  else                          showToast("PULL DOWN = SETTINGS");

  // v0.21：先把断开原因事件挂上，后面任何一次断线都能留下 reason 快照
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  if (gCfg.ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(gCfg.ssid.c_str(), gCfg.pass.c_str());
    Serial.printf("[NET] connecting to '%s' ...\n", gCfg.ssid.c_str());
  } else {
    // 未预置 Wi-Fi：不再自动开手机配网 AP，避免依赖手机/电脑。
    // 设备离线跑演示数据，用户随时点屏幕左上角 Wi-Fi 图标本机扫描并输入密码联网。
    // 如需回到手机配网，可串口发 AP 命令。
    Serial.println("[NET] no Wi-Fi saved -> offline demo; use on-device WIFI icon to setup");
  }
  // v0.14：网络任务必须在 Core1。ESP32-audioI2S 的解码任务(优先级2)和我们
  // 的喂流任务(优先级3)都钉在 Core0；原来 netTask 也挤在 Core0，天气/航班的
  // HTTPS 长请求抢 lwIP/TLS 共享锁，直播流 read() 被卡 -> InBuffer 抽空即静音。
  gVoiceWorkerReady = xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 1) == pdPASS;
  if (!gVoiceWorkerReady) Serial.println("[NET] task creation failed");
}

static uint32_t lastDraw = 0, lastFrame = 0;
void loop() {
  uint32_t now = millis();
  uint32_t dt = now - lastFrame;
  lastFrame = now;

  clockTick();
  clockApplyNtp();
  rtcService();
  netPollConnection();
  pomoTick();
  pollButton();               // 右上实体 BOOT 键：按一下 息屏 / 亮屏
  pollTouch();
  cloudApplyJobs();           // 语音结果、待办存盘都由主循环处理
  recPollSave();              // 每轮写一块，触摸和渲染可以继续
  aiVoicePoll();              // 录音中：刷新电平条 + 20s 上限自动停
  xzPageGuard();              // 离开小智页就关语音会话（别让麦克风在别的页面偷听）
  // 小智 OTA：绑定的第一屏要它轮询激活码；联网后还要静默拉一次 websocket 配置，
  // 否则 NVS 里没有 xz.token，语音通道连不上（表现为 token=0 chars）。
  {
    static uint8_t tries = 0;
    static uint32_t nextAt = 0;
    if (!xzWsToken().length() && WiFi.status() == WL_CONNECTED && tries < 5 &&
        (int32_t)(millis() - nextAt) >= 0) {
      tries++;
      nextAt = millis() + 20000;      // 失败就 20s 后再来，最多 5 次
      xzRequestBinding();
    }
  }
  xzPollBinding();
  // 闭环：从待办页"语音加"进备忘，识别写入完成后自动回清单（录完即见成果）
  if (gMemoBackToTodo && curPage == P_MEMO && gAiResultOwner == AVM_MEMO
      && (gAiState == AVS_DONE || gAiState == AVS_ERROR)) {
    gMemoBackToTodo = false;
    curPage = P_TODO; gTodoPage = 0;
    if (gAiState == AVS_DONE && gMemoLastAdded > 0) {
      char tip[32]; snprintf(tip, sizeof(tip), "已加 %d 条待办", gMemoLastAdded);
      showToast(tip);
    } else if (gAiState == AVS_ERROR) {
      showToast(gAiResult.length() ? gAiResult.c_str() : "识别失败");
    }
    render();
  }
  cfgPoll();                  // 串口配置命令
  // 离开配网页后把 STA 自动重连还回去（扫描时为避开干扰临时掐掉过）
  if (gWifiStoppedReconnect && curPage != P_WIFI) {
    gWifiStoppedReconnect = false;
    WiFi.setAutoReconnect(true);
    if (gCfg.ssid.length()) WiFi.begin(gCfg.ssid.c_str(), gCfg.pass.c_str());
    Serial.println("[NET] reconnect resumed");
  }
  provisionLoop();            // 配网 AP 的 DNS + WebServer
  mPlaying = (gAudioState == A_PLAYING);   // 黑胶转动跟随音频任务真实播放状态

  // 动画：锁屏时不跑，省电；解锁后才转
  if (!gLocked) {
    if (mPlaying) {
      vinylAngle += dt * 0.018f;
      if (vinylAngle >= 360) vinylAngle -= 360;
      trackProgress += dt * 0.000004f;
      if (trackProgress > 1) trackProgress = 0;
    }
    // v0.16：真实航班不插值，扫描线动画仍在 renderRadar() 里按 millis() 走
  }

  if (gLocked) { delay(gRecWrite ? 1 : 15); return; }
  static bool lastSettingsWifi = false;
  const bool staticPage = curPage == P_APPS || curPage == P_SETTINGS;
  if (staticPage) {
    const bool expired = gToast[0] && (int32_t)(now - gToastUntil) >= 0;
    if (expired) gToast[0] = 0;
    const bool wifiChanged = curPage == P_SETTINGS && lastSettingsWifi != wifiOK.load();
    lastSettingsWifi = wifiOK.load();
    if (expired || wifiChanged) render();
    delay(1);
    return;
  }

  uint32_t interval = 250;                                   // 表盘 4 fps 足够
  if (curPage == P_WIFI) interval = 120;                    // 配网页：扫描/键盘响应
  // 录音机：录音计时/回放进度要跟上（不录音不播放时也是 250ms 默认值，无需另设）
  else if (curPage == P_RECORDER && gAiState == AVS_RECORDING) interval = 200;
  // 音乐页降到 ~8fps：黑胶转动仍可见，但 QSPI 全屏 DMA 次数减半，
  // 与 Core0 上的 I2S 音频通道互不干扰（旧 16fps 是 DMA 竞态的诱因之一）。
  else if (curPage == P_MUSIC) interval = 120;
  else if (curPage == P_RADAR) interval = 70;               // 扫描线 ~14 fps
  else if (curPage == P_POMO && pomo.running) interval = 200;
  else if (curPage == P_CHAT && xzChatActive()) interval = 180;  // 状态点/电平条要跟手
  else if (gAiState == AVS_RECORDING) interval = 250;        // 录音：电平条 4fps
  if (now - lastDraw >= interval) { lastDraw = now; render(); }
  else delay(1);
}

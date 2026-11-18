#pragma once
// ============================================================================
//  运行时配置（NVS / Preferences）
//  刷机后不用重新编译，串口发命令即可设置 Wi-Fi 与彩云天气 token。
//     HELP                 查看命令
//     SHOW                 查看当前配置
//     WIFI <ssid> <pass>   设置 Wi-Fi（ssid 不支持空格）
//     TOKEN <彩云token>    设置彩云天气 v2.6 token
//     CLEAR                清空 NVS，回落到 config.h 里的编译期默认值
//     REBOOT               重启生效
//  注意：串口是 USB CDC，命令以回车结尾；波特率任意（USB CDC 不依赖波特率）。
// ============================================================================
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

struct RuntimeCfg {
  String ssid;
  String pass;
  String token;
};
static RuntimeCfg gCfg;
static Preferences gPrefs;
static String gCmdBuf;

// 由 main.cpp 实现的扩展命令（PAGE / SHOT），返回 true 表示已处理
bool cfgExtra(const String &line);

static void cfgLoad() {
  gPrefs.begin("rcfg", false);
  gCfg.ssid  = gPrefs.getString("ssid",  WIFI_SSID);
  gCfg.pass  = gPrefs.getString("pass",  WIFI_PASS);
  gCfg.token = gPrefs.getString("token", CAIYUN_TOKEN);
  gPrefs.end();
  Serial.printf("[CFG] wifi='%s'  caiyun=%s\n",
                gCfg.ssid.c_str(), gCfg.token.length() ? "SET" : "EMPTY(演示天气)");
}

static void cfgSet(const char *key, const String &val) {
  gPrefs.begin("rcfg", false);
  gPrefs.putString(key, val);
  gPrefs.end();
}

static String cfgGetStr(const char *key, const String &def = "") {
  gPrefs.begin("rcfg", false);
  String v = gPrefs.getString(key, def);
  gPrefs.end();
  return v;
}

// 整型配置（亮度 / 音量等），设置页里改完立即落盘
static int cfgGetInt(const char *key, int def) {
  gPrefs.begin("rcfg", false);
  int v = gPrefs.getInt(key, def);
  gPrefs.end();
  return v;
}
static void cfgSetInt(const char *key, int v) {
  gPrefs.begin("rcfg", false);
  gPrefs.putInt(key, v);
  gPrefs.end();
}

static void cfgShow() {
  Serial.println("---- runtime config ----");
  Serial.printf("  WIFI_SSID   : %s\n", gCfg.ssid.c_str());
  Serial.printf("  WIFI_PASS   : %s\n", gCfg.pass.length() ? "********" : "(empty)");
  Serial.printf("  CAIYUN_TOKEN: %s\n", gCfg.token.length() ? gCfg.token.c_str() : "(empty)");
  Serial.printf("  BRIGHT      : %d %%   VOLUME: %d/21\n",
                cfgGetInt("bright", BACKLIGHT_LEVEL), cfgGetInt("vol", 16));
  Serial.printf("  CITY        : %s  (%.5f, %.5f)   radar %d nm\n",
                CITIES[gCityIdx].name, gHomeLat, gHomeLon, RADAR_RADIUS_NM);
  Serial.printf("  POMODORO    : focus %d min (presets 15 / 25 / 45)\n",
                cfgGetInt("pomofocus", POMO_FOCUS_MIN));
  Serial.printf("  CLOCK FACE  : %s (0=analog 1=digital)\n",
                cfgGetInt("face", 0) ? "DIGITAL" : "ANALOG");
  Serial.printf("  refresh     : weather %lu min / adsb %lu s\n",
                (unsigned long)(WX_REFRESH_MS / 60000UL), (unsigned long)(ADSB_REFRESH_MS / 1000UL));
  Serial.println("------------------------");
}

static void cfgHandle(const String &raw) {
  String line = raw;
  line.trim();
  if (line.length() == 0) return;
  String up = line; up.toUpperCase();

  if (up == "HELP") {
    Serial.println("Commands: SHOW | STATUS | WIFI <ssid> <pass> | TOKEN <token> | CLEAR | REBOOT");
    Serial.println("          AP (captive portal) | WIFI (on-device setup page) | WIFIK (kb demo)");
    Serial.println("          SETTINGS (settings page) | BRIGHT <10..100> | VOL <0..21>");
    Serial.println("          TONE [freq] [ms]  (offline WAV test: I2S -> ES8311 -> PA -> speaker)");
    Serial.println("Audio:    PLAY | STOP | AUD (health dump) | SRC <0=radio 1=music 2=podcast> | TRACK <1..n>");
    Serial.println("          TFSCAN | TFTEST (write+play 2s WAV) | TFTESTCN (Chinese filename, verifies UTF-8)");
    Serial.println("          POD / PODFEED [1..6 | rss-url] | PODADD <name> <url> | PODCLEAR");
    Serial.println("          Sources: RADIO=12 named in Chinese, MUSIC=SD_MMC UTF-8 names, PODCAST=6 Xiaoyuzhou feeds");
    Serial.println("          PODFEED 1=无聊斋 2=谐星聊天会 3=李诞 4=忽左忽右 5=半拿铁 6=半拿铁周刊");
    Serial.println("Location: CITY (list) | CITY <1..N> | CITY <中文名>  -> weather + ADS-B re-fetch");
    Serial.println("          PLANES (dump radar contacts: LIVE / STALE, no demo targets)");
    Serial.println("Power:    BAT (BQ27220 fuel gauge raw dump: SOC / voltage / current / charging)");
    Serial.println("          BATF <0..100> [0|1] (force battery cell display for visual check; BATF -1 = real)");
    Serial.println("Time:     RTC (PCF85063 @0x51 raw 0x04..0x0A + usable?) | SETTIME 2026-11-15 23:30[:00]");
    Serial.println("          RTCD <reg> [len] / RTCW <reg> <b0> [b1..] (raw register peek/poke, hex)");
    Serial.println("          NTP is only used when online (~every 3 h); offline boot uses RTC, else last NVS time");
    Serial.println("          PAGE <0..13> | SHOT (dump 360x360 RGB565 framebuffer) | WDAY <0..6> (force weekday)");
    Serial.println("          FACE [0|1] (home clock: 0 analog / 1 digital; also settings FACE card)");
    Serial.println("          LANG [0|1] (digital face text: 0 中文 / 1 English; also settings LANG card)");
    Serial.println("          LOCK (screen off / on) | BTN (inject a BOOT-button press: only way to WAKE)");
    Serial.println("          Pomodoro: POMO <15|25|45> (focus preset; touch ignored while locked)");
    Serial.println("          TAP <x> <y> (inject a tap) | SWIPE <x1> <y1> <x2> <y2> (inject a swipe)");
    Serial.println("          MIC (state) | MICTEST [1..20] (record N s: samples/peak/rms) | MICPIN (pin edge diag)");
    Serial.println("Cloud:    BAILIAN [key] | QWEN <text> | AI | ASRTODO | XZBIND | XZSTATUS");
    Serial.println("Gesture:  swipe LEFT/RIGHT on home -> app grid; swipe UP on any app -> home");
    Serial.println("          home shows ONLY the face picked in Settings (FACE card), not both");
    Serial.println("          pull DOWN from upper half -> settings on ANY page; push UP in settings -> back");
    Serial.println("No Wi-Fi saved? Settings page (app grid) -> first row WIFI, or serial WIFI. AP command still works.");
    Serial.println("Config changes take effect after REBOOT (or power cycle).");
  } else if (up == "SHOW") {
    cfgShow();
  } else if (up == "REBOOT") {
    Serial.println("[CFG] rebooting...");
    delay(200);
    ESP.restart();
  } else if (up == "CLEAR") {
    gPrefs.begin("rcfg", false); gPrefs.clear(); gPrefs.end();
    // 内存态也要回落，否则 SHOW 会显示已经清掉的旧值
    gCfg.ssid  = WIFI_SSID;
    gCfg.pass  = WIFI_PASS;
    gCfg.token = CAIYUN_TOKEN;
    gCityIdx = 0; gHomeLat = HOME_LAT; gHomeLon = HOME_LON;
    Serial.println("[CFG] cleared, reboot to use config.h defaults");
  } else if (up.startsWith("TOKEN ")) {
    String v = line.substring(6); v.trim();
    cfgSet("token", v);
    gCfg.token = v;
    Serial.printf("[CFG] token saved (%d chars), REBOOT to apply\n", v.length());
  } else if (up.startsWith("WIFI ")) {
    String rest = line.substring(5); rest.trim();
    int sp = rest.indexOf(' ');
    String s = (sp < 0) ? rest : rest.substring(0, sp);
    String p = (sp < 0) ? ""    : rest.substring(sp + 1);
    p.trim();
    cfgSet("ssid", s); cfgSet("pass", p);
    gCfg.ssid = s; gCfg.pass = p;
    Serial.printf("[CFG] wifi '%s' saved, REBOOT to apply\n", s.c_str());
  } else if (!cfgExtra(line)) {
    Serial.printf("[CFG] unknown: %s (try HELP)\n", line.c_str());
  }
}

static void cfgPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (gCmdBuf.length()) { cfgHandle(gCmdBuf); gCmdBuf = ""; }
    } else if (gCmdBuf.length() < 220) {
      gCmdBuf += c;
    }
  }
}

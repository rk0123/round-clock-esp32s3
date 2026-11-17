#pragma once
// ============================================================
//  编译期默认值。刷机后可用串口命令覆盖（存 NVS，无需重编译）：
//    WIFI <ssid> <pass> / TOKEN <彩云token> / SHOW / CLEAR / REBOOT
//  运行时值优先于本文件；NVS 清空后回落到这里。
// ============================================================

// --- Wi-Fi ---
// 留空 = 自动开 AP "RoundClock-Setup" 配网（手机连上后打开 http://192.168.4.1/）。
// 也可以用串口 WIFI <ssid> <pass> 配置，值存 NVS。
#define WIFI_SSID                ""
#define WIFI_PASS                ""

// --- 彩云天气 v2.6（可选）---
// 格式: https://api.caiyunapp.com/v2.6/{TOKEN}/{经度},{纬度}/weather?dailysteps=1&hourlysteps=1
// 留空 / token 失效 -> 自动改用 Open-Meteo（免密钥，同样返回真实天气）。
// 只有两个源都失败才回落内置演示值。
#define CAIYUN_TOKEN             ""

// --- 位置（天气与雷达共用；默认上海）---
// v0.16：HOME_LAT/HOME_LON 只是「默认城市」的编译期坐标。运行时坐标由
// gHomeLat/gHomeLon 提供，用户在 设置 -> CITY 里选城市后写 NVS(city)，掉电保留。
#define HOME_LAT                 31.23
#define HOME_LON                 121.47

// v0.16 天气城市表：中文名用 16px 全量字库渲染，坐标同时供天气与 ADS-B 使用。
struct City { const char *name; float lat; float lon; };
static const City CITIES[] = {
  {"上海",     31.23f, 121.47f}, {"北京",     39.90f, 116.41f},
  {"广州",     23.13f, 113.26f}, {"深圳",     22.54f, 114.06f},
  {"杭州",     30.27f, 120.16f}, {"南京",     32.06f, 118.80f},
  {"成都",     30.57f, 104.07f}, {"武汉",     30.59f, 114.31f},
  {"西安",     34.34f, 108.94f}, {"重庆",     29.56f, 106.55f},
  {"天津",     39.13f, 117.20f}, {"苏州",     31.30f, 120.62f},
  {"长沙",     28.23f, 112.94f}, {"青岛",     36.07f, 120.38f},
  {"沈阳",     41.80f, 123.43f}, {"厦门",     24.48f, 118.09f},
  {"哈尔滨",   45.80f, 126.53f}, {"昆明",     25.04f, 102.71f},
  {"拉萨",     29.65f,  91.13f}, {"乌鲁木齐", 43.83f,  87.62f},
};
#define CITY_COUNT ((int)(sizeof(CITIES) / sizeof(CITIES[0])))

// 可变状态在 main.cpp 定义（单一副本）；runtime_config.h 的 SHOW 也要读
extern int   gCityIdx;
extern float gHomeLat, gHomeLon;

// --- 刷新策略（按需求下调，省流量 / 省配额 / 省电）---
#define WX_REFRESH_MS            (30UL * 60UL * 1000UL)   // 天气 30 min
#define ADSB_REFRESH_MS          (60UL * 1000UL)          // 航班 60 s
#define ADSB_HTTP_TIMEOUT_MS     15000   // ESP32 TLS + 响应头实测偶发 8s 不够，HTTP -11

// --- 雷达：v0.16 起只画 api.adsb.lol 的真实目标，不再补演示飞机 ---
#define RADAR_RADIUS_NM          250      // 大半径：实测国内近场稀疏，250 nm 才有稳定目标
#define RADAR_RANGES_NM          { 100, 150, 250 }
#define RADAR_MAX_PLANES         10       // 同屏最多（圆屏标签不打架）

// --- 番茄钟 ---
#define POMO_FOCUS_MIN           25
#define POMO_SHORT_BREAK_MIN     5
#define POMO_LONG_BREAK_MIN      15
#define POMO_LONG_BREAK_EVERY    4

// --- 显示 ---
#define BACKLIGHT_LEVEL          70       // 0..100
#define SCREEN_W                 360
#define SCREEN_H                 360
#define SAFE_R                   158      // 外壳遮挡后的安全半径

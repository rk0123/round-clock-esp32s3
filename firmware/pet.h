#pragma once
// ============================================================
//  WorkBuddy Buddy v2.0  ·  圆屏桌面伙伴
//  参考 anthropics/claude-desktop-buddy 的桌面宠物设计，平台改为 WorkBuddy：
//    · 七种状态 sleep / idle / busy / attention / celebrate / dizzy / heart
//    · 五种 ASCII 精灵，状态动画用逐帧姿态 + 粒子实现
//    · 待授权时在设备上直接 OK / NO，回写 permission ack
//    · token 累计升级（每 5 万一级），授权响应时长进入心情评估
//    · 数据来源：USB 串口逐行 JSON 快照（桌面桥接脚本），另有 DEMO 自演示
//  旧宠物（v2.9 拓麻歌子）已封存为 pet_v29_sealed.h。
// ============================================================
#include <ArduinoJson.h>

// main.cpp 前向声明（本头在颜色宏 + ui_kit.h 之后 include）
static void showToast(const char *s);
static bool hit(int x, int y, int cx, int cy, int r);
static void render();

#define WB_BTN_L   108
#define WB_BTN_C   180
#define WB_BTN_R   252
#define WB_BTN_Y   285
#define WB_BTN_RAD 30

enum WbState : uint8_t {
  WB_SLEEP = 0, WB_IDLE, WB_BUSY, WB_ATTENTION,
  WB_CELEBRATE, WB_DIZZY, WB_HEART, WB_STATE_COUNT
};
static const char *const WB_STATE_NAME[WB_STATE_COUNT] = {
  "SLEEP", "IDLE", "BUSY", "ATTENTION", "CELEBRATE", "DIZZY", "HEART"
};
// 屏幕上一律中文；英文状态名只留串口诊断用
static const char *const WB_STATE_CN[WB_STATE_COUNT] = {
  "睡眠", "空闲", "忙碌", "待授权", "完成", "眩晕", "开心"
};
// 忙碌态的第二行标签：进行中 / 全部完成
#define WB_STATE_DONE_LABEL "完成"

// ------------------------------------------------------------ ASCII 精灵
// 每行严格 12 字符、5 行，FONT12 × scale2 = 192×120 像素，整幅落在安全圆内。
static const char *const ART_CAT_SLEEP[5] = {
  "            ", "   .-..-.   ", "  ( -.- )   ", "  `------`~ ", "            "
};
static const char *const ART_CAT_IDLE[5] = {
  "            ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )  ", "  (\")_(\")   "
};
static const char *const ART_CAT_BUSY[5] = {
  "    .       ", "   /\\_/\\    ", "  ( O   O ) ", "  (  w   )_ ", "  (\")_(\")   "
};
static const char *const ART_CAT_ATTENTION[5] = {
  "     !      ", "   /^_^\\    ", "  ( O   O ) ", "  (  v   )  ", "  (\")_(\")   "
};
static const char *const ART_CAT_CELEBRATE[5] = {
  "  \\^   ^/   ", "    /\\_/\\   ", "  ( ^   ^ ) ", "  (  W   )  ", "  (\")_(\")~  "
};
static const char *const ART_CAT_DIZZY[5] = {
  "            ", "   /\\_/\\    ", "  ( @   @ ) ", "  (  ~   )  ", "  (\")_(\")   "
};
static const char *const ART_CAT_HEART[5] = {
  "     <3     ", "   /\\_/\\    ", "  ( <3 <3 ) ", "  (  u   )  ", "  (\")_(\")~  "
};

static const char *const ART_DUCK_SLEEP[5] = {
  "            ", "    __      ", "   ( -_- )  ", "    \\___)   ", "      z     "
};
static const char *const ART_DUCK_IDLE[5] = {
  "            ", "    __      ", "   (o>_\\    ", "    \\___)   ", "      ~     "
};
static const char *const ART_DUCK_BUSY[5] = {
  "     .      ", "    __      ", "   (O>_\\    ", "    \\___)_  ", "      ~     "
};
static const char *const ART_DUCK_ATTENTION[5] = {
  "     !      ", "    __      ", "   (O>_\\    ", "    \\___)   ", "      ~     "
};
static const char *const ART_DUCK_CELEBRATE[5] = {
  "    \\o/     ", "    __      ", "   (^>^\\    ", "    \\___)~  ", "      *     "
};
static const char *const ART_DUCK_DIZZY[5] = {
  "     *      ", "    __      ", "   (@>@\\    ", "    \\___)   ", "     ~      "
};
static const char *const ART_DUCK_HEART[5] = {
  "    <3      ", "    __      ", "   (u>u\\    ", "    \\___)~  ", "     <      "
};

static const char *const ART_PENGUIN_SLEEP[5] = {
  "   .---.    ", "  ( -.- )   ", "  (  ~  )   ", "  (_____)   ", "      z     "
};
static const char *const ART_PENGUIN_IDLE[5] = {
  "   .---.    ", "  ( o o )   ", "  (  >  )   ", "  (_____)   ", "   /   \\    "
};
static const char *const ART_PENGUIN_BUSY[5] = {
  "    .       ", "  ( O O )   ", "  (  >  )_  ", "  (_____)   ", "   /   \\    "
};
static const char *const ART_PENGUIN_ATTENTION[5] = {
  "     !      ", "  ( O O )   ", "  (  >  )   ", "  (_____)   ", "   /   \\    "
};
static const char *const ART_PENGUIN_CELEBRATE[5] = {
  "    \\o/     ", "   .---.    ", "  ( ^ ^ )   ", "  (  W  )~  ", "  (_____)   "
};
static const char *const ART_PENGUIN_DIZZY[5] = {
  "     *      ", "   .---.    ", "  ( @ @ )   ", "  (  ~  )   ", "  (_____)   "
};
static const char *const ART_PENGUIN_HEART[5] = {
  "    <3      ", "   .---.    ", "  ( <3 > )  ", "  (  u  )   ", "  (_____)~  "
};

static const char *const ART_ROBOT_SLEEP[5] = {
  "  [======]  ", "  | -  - |  ", "  |  zz  |  ", "  [======]  ", "   |    |   "
};
static const char *const ART_ROBOT_IDLE[5] = {
  "  [======]  ", "  | o  o |  ", "  |  --  |  ", "  [======]  ", "   |    |   "
};
static const char *const ART_ROBOT_BUSY[5] = {
  "  [======]  ", "  | O  O |  ", "  |  ##  |  ", "  [======]  ", "   |    |   "
};
static const char *const ART_ROBOT_ATTENTION[5] = {
  "  [======]  ", "  | !  ! |  ", "  |  ??  |  ", "  [======]  ", "   |    |   "
};
static const char *const ART_ROBOT_CELEBRATE[5] = {
  "  [======]  ", "  | ^  ^ |  ", "  |  **  |  ", "  [======]  ", "  \\|    |/  "
};
static const char *const ART_ROBOT_DIZZY[5] = {
  "  [======]  ", "  | @  @ |  ", "  |  ~~  |  ", "  [======]  ", "   |    |   "
};
static const char *const ART_ROBOT_HEART[5] = {
  "  [======]  ", "  |<3 <3 |  ", "  |  uu  |  ", "  [======]  ", "   |    |   "
};

static const char *const ART_SHROOM_SLEEP[5] = {
  "   .----.   ", "  / -  - \\  ", " |   ~~   | ", "  \\      /  ", "   '----'   "
};
static const char *const ART_SHROOM_IDLE[5] = {
  "   .----.   ", "  / o  o \\  ", " |   w    | ", "  \\      /  ", "   '----'   "
};
static const char *const ART_SHROOM_BUSY[5] = {
  "    .       ", "  / O  O \\  ", " |   w    | ", "  \\      /_ ", "   '----'   "
};
static const char *const ART_SHROOM_ATTENTION[5] = {
  "     !      ", "  / O  O \\  ", " |   v    | ", "  \\      /  ", "   '----'   "
};
static const char *const ART_SHROOM_CELEBRATE[5] = {
  "    \\o/     ", "  / ^  ^ \\  ", " |   W    | ", "  \\      /~ ", "   '----'   "
};
static const char *const ART_SHROOM_DIZZY[5] = {
  "     *      ", "  / @  @ \\  ", " |   ~    | ", "  \\      /  ", "   '----'   "
};
static const char *const ART_SHROOM_HEART[5] = {
  "    <3      ", "  / ^  ^ \\  ", " |   u    | ", "  \\      /~ ", "   '----'   "
};

struct WbSpecies {
  const char *name;
  uint16_t color;
  const char *const *art[WB_STATE_COUNT];
};
static const WbSpecies WB_SPECIES[5] = {
  {"CAT",     COL(226, 178, 104), {ART_CAT_SLEEP,     ART_CAT_IDLE,     ART_CAT_BUSY,     ART_CAT_ATTENTION,     ART_CAT_CELEBRATE,     ART_CAT_DIZZY,     ART_CAT_HEART}},
  {"DUCK",    COL(248, 212,  84), {ART_DUCK_SLEEP,    ART_DUCK_IDLE,    ART_DUCK_BUSY,    ART_DUCK_ATTENTION,    ART_DUCK_CELEBRATE,    ART_DUCK_DIZZY,    ART_DUCK_HEART}},
  {"PENGUIN", COL(126, 196, 232), {ART_PENGUIN_SLEEP, ART_PENGUIN_IDLE, ART_PENGUIN_BUSY, ART_PENGUIN_ATTENTION, ART_PENGUIN_CELEBRATE, ART_PENGUIN_DIZZY, ART_PENGUIN_HEART}},
  {"ROBOT",   COL(150, 206, 186), {ART_ROBOT_SLEEP,   ART_ROBOT_IDLE,   ART_ROBOT_BUSY,   ART_ROBOT_ATTENTION,   ART_ROBOT_CELEBRATE,   ART_ROBOT_DIZZY,   ART_ROBOT_HEART}},
  {"SHROOM",  COL(232, 140, 158), {ART_SHROOM_SLEEP,  ART_SHROOM_IDLE,  ART_SHROOM_BUSY,  ART_SHROOM_ATTENTION,  ART_SHROOM_CELEBRATE,  ART_SHROOM_DIZZY,  ART_SHROOM_HEART}},
};
#define WB_N_SPECIES 5

static const char *const WB_SPECIES_CN[WB_N_SPECIES] = {"猫猫", "鸭鸭", "企鹅", "机器人", "蘑菇"};

// FONT16 中英混排修复：ASCII 步进 10px、画布 16px，数字紧挨汉字时墨迹右缘会
// 咬进汉字 6px（这是当初改用中文数字的原因）。正解是按「连续 ASCII / 连续 CJK」
// 切段分别绘制，段间按真实墨迹宽度补 6px —— 这样统计数字可以用阿拉伯数字。
// 已含空格段：空格无墨迹，补位不影响观感。font 固定 16 / scale 1。
static void wbDrawMix(int cx, int y, const char *s, uint16_t col) {
  char segs[12][48]; int ns = 0, total = 0;
  const char *p = s;
  while (*p && ns < 12) {
    bool ascii = ((unsigned char)*p < 0x80);
    char *d = segs[ns]; int n = 0;
    while (*p && n < 46) {
      d[n++] = *p;
      if (ascii) { p++; }
      else { p++; while (*p && ((unsigned char)*p & 0xC0) == 0x80) d[n++] = *p++; }
      if (ascii != ((*p && ((unsigned char)*p < 0x80)) || !*p)) break;
    }
    d[n] = 0;
    int w = ascii ? n * 10 : (n / 3) * 16;   // FONT16: ASCII 10 / 汉字 16
    total += ascii ? w + 6 : w;               // ASCII 段末字画布多 6px
    ns++;
  }
  int x = cx - total / 2;
  for (int i = 0; i < ns; i++) {
    drawText(x, y, segs[i], col, 16, 1, 0);
    int n = (int)strlen(segs[i]);
    x += ((unsigned char)segs[i][0] < 0x80) ? n * 10 + 6 : (n / 3) * 16;
  }
}
static const char *wbToolCn(const char *tool) {
  if (!tool || !tool[0]) return "操作";
  if (!strcmp(tool, "Bash"))     return "命令";
  if (!strcmp(tool, "Read"))     return "读文件";
  if (!strcmp(tool, "Write"))    return "写文件";
  if (!strcmp(tool, "Edit"))     return "编辑";
  if (!strcmp(tool, "Glob"))     return "查找文件";
  if (!strcmp(tool, "Grep"))     return "搜索";
  if (!strcmp(tool, "WebFetch")) return "抓网页";
  return tool;                    // 未知工具名是数据，原样显示
}

// wbDrawMix 的度量版（同一套分段规则）：ASCII 段 = 字数×10 + 段末 6px，汉字 16/字。
// 用来判断一行到底装不装得下，装不下就走马灯。
static int wbMixWidth(const char *s) {
  int total = 0;
  const char *p = s;
  while (*p) {
    bool ascii = ((unsigned char)*p < 0x80);
    int n = 0;
    while (*p) {
      n++;
      if (ascii) p++;
      else { p++; while (*p && ((unsigned char)*p & 0xC0) == 0x80) p++; }
      if (!*p || ((*p < 0x80) != ascii)) break;
    }
    total += ascii ? n * 10 + 6 : n * 16;
  }
  return total;
}

// 放不下时的横向走马灯：起点停 1.2s → 匀速滚完一圈 → 尾部留 24px 空隙后循环。
// 用 drawTextClip 把文字裁在 [x0,x1] 里，否则圆屏上会直接溢出到圈外。
static bool gPetScroll = false;     // 本帧有滚动 → main.cpp 提高该页刷新率
static void wbMarquee(int y, const char *s, uint16_t col, int x0, int x1, int font) {
  if (!s || !s[0]) return;
  int w = textWidth(s, font, 1), avail = x1 - x0 + 1;
  if (w <= avail) {                                  // 装得下就说明，别动
    drawText((x0 + x1 + 1) / 2, y, s, col, font, 1, 1);
    return;
  }
  gPetScroll = true;
  const int gap = 24;
  int span = w + gap;
  const uint32_t speed = (font == 12) ? 34UL : 44UL;  // ms/px
  uint32_t moveMs = (uint32_t)span * speed;
  uint32_t t = millis() % (moveMs + 1200UL);
  int off = (t < 1200UL) ? 0 : (int)((t - 1200UL) / speed);
  drawTextClip(x0 - off, y, s, col, font, 1, x0, x1);
  drawTextClip(x0 - off + span, y, s, col, font, 1, x0, x1);
}

// ------------------------------------------------------------ 持久化统计
#define WB_TOKENS_PER_LEVEL 50000UL
struct WbStats {
  uint16_t appr, deny;
  uint32_t tokens;
  uint8_t level, species;
  uint16_t vel[8];            // 授权响应秒数环形缓冲
  uint8_t velIdx, velCnt;
};
static WbStats wbSt;
static bool   wbDemo = true;
static uint32_t wbDemoNext = 0;
static uint8_t  wbDemoIdx = 0;

static void wbStatsLoad() {
  wbSt.appr    = cfgGetInt("wb.appr", 0);
  wbSt.deny    = cfgGetInt("wb.deny", 0);
  wbSt.tokens  = cfgGetInt("wb.tok", 0);
  wbSt.level   = cfgGetInt("wb.lvl", 0);
  wbSt.species = cfgGetInt("wb.spec", 0);
  wbSt.velIdx  = cfgGetInt("wb.vidx", 0);
  wbSt.velCnt  = cfgGetInt("wb.vcnt", 0);
  static const char *const VK[8] = {"wb.v0","wb.v1","wb.v2","wb.v3","wb.v4","wb.v5","wb.v6","wb.v7"};
  for (int i = 0; i < 8; i++) wbSt.vel[i] = cfgGetInt(VK[i], 0);
  if (wbSt.species >= WB_N_SPECIES) wbSt.species = 0;
  if (wbSt.velIdx >= 8) wbSt.velIdx = 0;
  if (wbSt.velCnt > 8) wbSt.velCnt = 8;
}
static void wbStatsSave() {
  cfgSetInt("wb.appr", wbSt.appr);
  cfgSetInt("wb.deny", wbSt.deny);
  cfgSetInt("wb.tok",  (int)wbSt.tokens);
  cfgSetInt("wb.lvl", wbSt.level);
  cfgSetInt("wb.spec", wbSt.species);
  cfgSetInt("wb.vidx", wbSt.velIdx);
  cfgSetInt("wb.vcnt", wbSt.velCnt);
  static const char *const VK[8] = {"wb.v0","wb.v1","wb.v2","wb.v3","wb.v4","wb.v5","wb.v6","wb.v7"};
  for (int i = 0; i < 8; i++) cfgSetInt(VK[i], wbSt.vel[i]);
}
static uint16_t wbMedianVel() {
  if (!wbSt.velCnt) return 0;
  uint16_t a[8]; int n = wbSt.velCnt;
  for (int i = 0; i < n; i++) a[i] = wbSt.vel[i];
  for (int i = 1; i < n; i++) { uint16_t k = a[i]; int j = i - 1; while (j >= 0 && a[j] > k) { a[j+1] = a[j]; j--; } a[j+1] = k; }
  return a[n/2];
}
static int wbEnergyTier() {
  int e = 5 - (int)((millis() / 3600000UL) % 6);
  return e < 1 ? 1 : e;
}

// ------------------------------------------------------------ 快照 / 状态
struct WbSnapshot {
  uint8_t total, running, waiting;    // 任务总数 / 进行中 / 待授权
  uint8_t done;                       // 已完成任务数
  bool completed;
  uint32_t tokensToday;
  uint32_t credits, streak, energy;   // 当前 WorkBuddy 账号只读数据（0 = 未绑定）
  char msg[40];
  char promptId[40], promptTool[20], promptHint[56];
  bool connected;
  uint32_t lastMs;
};
static WbSnapshot wbSnap;
static WbState  wbOverride = WB_IDLE;
static uint32_t wbOverrideUntil = 0;
static uint32_t wbCompleteUntil = 0;
static uint32_t wbPromptAt = 0;
static uint8_t  wbPrevRunning = 0;      // 上一帧进行中任务数，用于检测「全部完成」

static void wbTrigger(WbState s, uint32_t ms) {
  wbOverride = s;
  wbOverrideUntil = millis() + ms;
}

static WbState wbState() {
  // 待授权永远优先：升级庆祝 / 摇晃等短时覆盖不能把权限提示盖住
  if (wbSnap.promptId[0]) return WB_ATTENTION;
  if (wbOverrideUntil && (int32_t)(millis() - wbOverrideUntil) < 0) return wbOverride;
  if (!wbSnap.connected) return WB_SLEEP;
  if (wbCompleteUntil && (int32_t)(millis() - wbCompleteUntil) < 0) return WB_CELEBRATE;
  if (wbSnap.running) return WB_BUSY;
  if (wbSnap.total == 0) return WB_SLEEP;
  return WB_IDLE;
}

// DEMO：离线 / 空闲 / 忙碌 / 待授权 / 完成，8 秒一轮
static const struct { const char *n; uint8_t t, r, w; bool comp; } WB_DEMO[] = {
  {"OFFLINE",   0, 0, 0, false},
  {"IDLE",      1, 0, 0, false},
  {"BUSY",      4, 3, 0, false},
  {"ATTENTION", 2, 1, 1, false},
  {"COMPLETED", 1, 0, 0, true}
};
static void wbDemoApply() {
  const auto &d = WB_DEMO[wbDemoIdx];
  wbSnap.connected   = (wbDemoIdx != 0);
  wbSnap.total       = d.t;
  wbSnap.running     = d.r;
  wbSnap.waiting     = d.w;
  wbSnap.completed   = d.comp;
  wbSnap.tokensToday = 12000UL + (uint32_t)wbDemoIdx * 33000UL;
  wbSnap.credits = wbSnap.streak = wbSnap.energy = 0;   // 演示态不顶着真实账号数据
  snprintf(wbSnap.msg, sizeof(wbSnap.msg), "demo:%s", d.n);
  if (d.w) {
    strncpy(wbSnap.promptId,   "req_demo",        sizeof(wbSnap.promptId)-1);
    strncpy(wbSnap.promptTool, "Bash",            sizeof(wbSnap.promptTool)-1);
    strncpy(wbSnap.promptHint, "rm -rf /tmp/foo", sizeof(wbSnap.promptHint)-1);
    wbSnap.promptId[sizeof(wbSnap.promptId)-1] = 0;
    wbSnap.promptTool[sizeof(wbSnap.promptTool)-1] = 0;
    wbSnap.promptHint[sizeof(wbSnap.promptHint)-1] = 0;
    wbPromptAt = millis();
  } else {
    wbSnap.promptId[0] = wbSnap.promptTool[0] = wbSnap.promptHint[0] = 0;
    wbPromptAt = 0;
  }
  wbSnap.lastMs = millis();
  if (d.comp) wbTrigger(WB_CELEBRATE, 3000);
}

static void petTick() {
  uint32_t now = millis();
  if (wbDemo && (int32_t)(now - wbDemoNext) >= 0) {
    wbDemoIdx = (wbDemoIdx + 1) % (sizeof(WB_DEMO)/sizeof(WB_DEMO[0]));
    wbDemoNext = now + 8000;
    wbDemoApply();
  }
  if (wbSnap.connected && (now - wbSnap.lastMs) > 30000UL) wbSnap.connected = false;
  if (wbCompleteUntil && (int32_t)(now - wbCompleteUntil) >= 0) {
    wbCompleteUntil = 0; wbSnap.completed = false;
  }
}

// ------------------------------------------------------------ 桌面桥接数据
// 收到 WorkBuddy 桥接器逐行 JSON 快照；wire 协议与参考项目一致。
static void petFeedJson(const char *line) {
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, line)) {
    Serial.println("[WB] bad json");
    return;
  }
  // 桌面下发的命令：回 ack（桥接器要求每条 cmd 都有应答）
  const char *cmd = doc["cmd"];
  if (cmd) {
    char out[256];
    if (strcmp(cmd, "status") == 0) {
      snprintf(out, sizeof(out),
        "{\"ack\":\"status\",\"ok\":true,\"data\":{\"name\":\"WorkBuddy Buddy\",\"sec\":false,"
        "\"bat\":{\"pct\":0},\"sys\":{\"up\":%lu,\"heap\":%u},"
        "\"stats\":{\"appr\":%u,\"deny\":%u,\"lvl\":%u},"
        "\"acc\":{\"credits\":%lu,\"streak\":%lu,\"energy\":%lu}}}",
        millis()/1000UL, ESP.getFreeHeap(), wbSt.appr, wbSt.deny, wbSt.level,
        (unsigned long)wbSnap.credits, (unsigned long)wbSnap.streak,
        (unsigned long)wbSnap.energy);
    } else if (strcmp(cmd, "unpair") == 0) {
      snprintf(out, sizeof(out), "{\"ack\":\"unpair\",\"ok\":true}");
    } else if (strcmp(cmd, "name") == 0 || strcmp(cmd, "owner") == 0) {
      snprintf(out, sizeof(out), "{\"ack\":\"%s\",\"ok\":true}", cmd);
    } else {
      snprintf(out, sizeof(out), "{\"ack\":\"%s\",\"ok\":false,\"error\":\"unsupported\"}", cmd);
    }
    Serial.println(out);
    wbSnap.lastMs = millis();
    return;
  }
  // 真数据到来即退出演示模式，避免两套状态打架
  if (wbDemo) { wbDemo = false; cfgSetInt("wb.demo", 0); }

  wbSnap.total       = doc["total"]     | wbSnap.total;
  wbSnap.running     = doc["running"]   | wbSnap.running;
  wbSnap.waiting     = doc["waiting"]   | wbSnap.waiting;
  wbSnap.done        = doc["done"]      | wbSnap.done;
  wbSnap.tokensToday = doc["tokens_today"] | wbSnap.tokensToday;
  wbSnap.credits     = doc["credits"]     | wbSnap.credits;
  wbSnap.streak      = doc["streak"]      | wbSnap.streak;
  wbSnap.energy      = doc["energy"]      | wbSnap.energy;
  const char *m = doc["msg"];
  if (m) { strncpy(wbSnap.msg, m, sizeof(wbSnap.msg)-1); wbSnap.msg[sizeof(wbSnap.msg)-1] = 0; }

  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char *id = pr["id"], *tool = pr["tool"], *hint = pr["hint"];
    strncpy(wbSnap.promptId,   id   ? id   : "", sizeof(wbSnap.promptId)-1);
    strncpy(wbSnap.promptTool, tool ? tool : "", sizeof(wbSnap.promptTool)-1);
    strncpy(wbSnap.promptHint, hint ? hint : "", sizeof(wbSnap.promptHint)-1);
    wbSnap.promptId[sizeof(wbSnap.promptId)-1] = 0;
    wbSnap.promptTool[sizeof(wbSnap.promptTool)-1] = 0;
    wbSnap.promptHint[sizeof(wbSnap.promptHint)-1] = 0;
    if (wbSnap.promptId[0] && !wbPromptAt) wbPromptAt = millis();
  } else {
    wbSnap.promptId[0] = wbSnap.promptTool[0] = wbSnap.promptHint[0] = 0;
    wbPromptAt = 0;
  }

  if (doc["completed"] | false) { wbCompleteUntil = millis() + 3000; wbTrigger(WB_CELEBRATE, 3000); }
  // 任务全部完成：上一帧还有进行中任务，这一帧归零 → 庆祝（只触发一次）
  if (wbPrevRunning > 0 && wbSnap.running == 0 && !wbSnap.promptId[0]) {
    wbCompleteUntil = millis() + 4000;
    wbTrigger(WB_CELEBRATE, 4000);
  }
  wbPrevRunning = wbSnap.running;
  uint32_t tok = doc["tokens"] | 0;
  if (tok > wbSt.tokens) {
    uint8_t before = (uint8_t)(wbSt.tokens / WB_TOKENS_PER_LEVEL);
    wbSt.tokens = tok;
    uint8_t after = (uint8_t)(wbSt.tokens / WB_TOKENS_PER_LEVEL);
    if (after > before) {
      wbSt.level = after;
      wbTrigger(WB_CELEBRATE, 4000);
      wbStatsSave();                       // 只在升级里程碑写 NVS
    }
  }
  wbSnap.connected = true;
  wbSnap.lastMs = millis();
}

// ------------------------------------------------------------ 授权决策
// 云端回传队列：触摸 OK/NO 只置位，真正 POST 留给 netTask 做（绝不在触摸里做 TLS）
static volatile bool  wbCloudDecisionPending = false;
static volatile bool  wbCloudDecisionApprove = false;
static char wbCloudDecisionId[40], wbCloudDecisionTool[20], wbCloudDecisionHint[56];

static void wbDecide(bool approve) {
  // 演示模式下可能没有真实 prompt，但串口/触摸仍要能验收到完整 ack
  char id[40];
  if (wbSnap.promptId[0]) {
    strncpy(id, wbSnap.promptId, sizeof(id) - 1); id[sizeof(id) - 1] = 0;
  } else {
    snprintf(id, sizeof(id), "req_demo_%lu", millis() / 1000UL);
  }
  char ack[160];
  snprintf(ack, sizeof(ack), "{\"ack\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
           id, approve ? "once" : "deny");
  Serial.println(ack);                     // 桌面桥接器读这行回写决策
  // 云端模式：排队交给 netTask 回传，串口桥接与云端可以同时工作
  strncpy(wbCloudDecisionId,   id,                 sizeof(wbCloudDecisionId) - 1);
  strncpy(wbCloudDecisionTool, wbSnap.promptTool,  sizeof(wbCloudDecisionTool) - 1);
  strncpy(wbCloudDecisionHint, wbSnap.promptHint,  sizeof(wbCloudDecisionHint) - 1);
  wbCloudDecisionId[sizeof(wbCloudDecisionId)-1] = 0;
  wbCloudDecisionTool[sizeof(wbCloudDecisionTool)-1] = 0;
  wbCloudDecisionHint[sizeof(wbCloudDecisionHint)-1] = 0;
  wbCloudDecisionApprove = approve;
  wbCloudDecisionPending = true;
  wbOverrideUntil = 0;                     // 先撤掉强制状态，让 heart / dizzy 立刻生效
  uint32_t secs = wbPromptAt ? (millis() - wbPromptAt) / 1000UL : 0;
  if (approve) {
    wbSt.appr++;
    wbSt.vel[wbSt.velIdx] = (uint16_t)min(secs, 65535UL);
    wbSt.velIdx = (wbSt.velIdx + 1) % 8;
    if (wbSt.velCnt < 8) wbSt.velCnt++;
    wbTrigger(WB_HEART, 3500);
  } else {
    wbSt.deny++;
    wbTrigger(WB_DIZZY, 2500);
  }
  wbSnap.promptId[0] = wbSnap.promptTool[0] = wbSnap.promptHint[0] = 0;
  wbPromptAt = 0;
  wbSnap.waiting = 0;
  wbStatsSave();
}

// ------------------------------------------------------------ 绘制
#define WB_ART_X 84
#define WB_ART_Y 88
static void wbDrawArt(const WbSpecies &sp, WbState st, uint32_t t) {
  fillRect(72, 82, 216, 134, UI_PAGE_BG);   // 清掉上一帧精灵 + 粒子
  int xOff = 0, yOff = 0;
  switch (st) {
    case WB_SLEEP:     yOff = ((t / 4) & 1) ? 1 : 0; break;
    case WB_IDLE:      { int k = (t / 12) % 9; xOff = (k == 3) ? 1 : ((k == 4) ? -1 : 0); break; }
    case WB_BUSY:      xOff = (t & 1) ? 1 : -1; break;
    case WB_ATTENTION: yOff = (t & 1) ? -2 : 0; break;
    case WB_CELEBRATE: { int k = t & 3; yOff = (k == 0) ? -4 : ((k == 1) ? -2 : ((k == 2) ? 0 : -2)); break; }
    case WB_DIZZY:     xOff = (t & 1) ? 3 : -3; break;
    case WB_HEART:     yOff = ((t / 2) & 1) ? -1 : 0; break;
    default: break;
  }
  const char *const *art = sp.art[st];
  for (int i = 0; i < 5; i++)
    drawText(WB_ART_X + xOff * 2, WB_ART_Y + yOff + i * 24, art[i], sp.color, 12, 2, 0);
  // 状态粒子
  if (st == WB_SLEEP) {
    for (int i = 0; i < 3; i++) {
      int ph = (t + i * 4) % 12;
      drawText(250 + i * 9, 178 - ph * 7, i ? "z" : "Z", i ? C_GREY : C_CREAM, 12, 1, 0);
    }
  } else if (st == WB_BUSY) {
    static const char *const DOTS[] = {".  ", ".. ", "...", " ..", "  .", "   "};
    drawText(258, 142, DOTS[t % 6], C_WHITE, 12, 1, 0);
    drawText(96, 120, "~", COL(120, 190, 240), 12, 1, 0);
  } else if (st == WB_ATTENTION) {
    if (t & 1) drawText(180, 84, "!", C_YELLOW, 12, 2, 1);
  } else if (st == WB_CELEBRATE) {
    static const uint16_t CC[5] = {C_YELLOW, COL(240,120,140), COL(120,210,240), C_WHITE, C_GREEN};
    for (int i = 0; i < 7; i++) {
      int ph = (t * 2 + i * 5) % 20;
      drawText(88 + i * 17, 92 + ph * 5, (i & 1) ? "*" : ".", CC[i % 5], 12, 1, 0);
    }
  } else if (st == WB_DIZZY) {
    static const int8_t OX[8] = {0, 6, 8, 6, 0, -6, -8, -6};
    static const int8_t OY[8] = {-6, -4, 0, 4, 6, 4, 0, -4};
    drawText(180 + OX[t & 7], 138 + OY[t & 7], "*", COL(120,210,240), 12, 1, 1);
    drawText(180 + OX[(t+4)&7], 138 + OY[(t+4)&7], "*", C_YELLOW, 12, 1, 1);
  } else if (st == WB_HEART) {
    for (int i = 0; i < 4; i++) {
      int ph = (t + i * 3) % 12;
      drawText(140 + i * 22, 176 - ph * 7, "<3", COL(240,120,140), 12, 1, 1);
    }
  }
}

static void wbDrawInfo(WbState st) {
  char buf[96];
  if (st == WB_ATTENTION && wbSnap.promptId[0]) {
    fillRoundRect(48, 212, 264, 40, 10, COL(64, 36, 32));
    roundRect(48, 212, 264, 40, 10, COL(190, 96, 84));
    // 第一行「待授权 + 工具名」。已知工具都是 2~4 字，未知工具名（mcp__xxx__Yyy）
    // 可能是二三十个字符，原来会直接冲出胶囊 → 超出时只滚动工具名，前缀保持不动。
    const char *tool = wbToolCn(wbSnap.promptTool);
    snprintf(buf, sizeof(buf), "待授权  %s", tool);
    if (wbMixWidth(buf) <= 240) {
      wbDrawMix(180, 216, buf, COL(255, 214, 196));
    } else {
      drawText(62, 216, "待授权", COL(255, 214, 196), 16, 1, 0);
      wbMarquee(216, tool, COL(255, 214, 196), 116, 298, 16);
    }
    // 第二行是 WorkBuddy 传来的原始命令参数（数据，不是界面文案），ASCII 字库；
    // 推送源侧已做 ascii_safe，命令常常 40~56 字符，必须滚动才能看全。
    wbMarquee(236, wbSnap.promptHint, C_GREY, 62, 298, 12);
  } else {
    fillRoundRect(48, 212, 264, 40, 10, COL(24, 34, 44));
    roundRect(48, 212, 264, 40, 10, UI_DIV);
    if (wbSnap.credits) {
      // 已绑定当前 WorkBuddy 账号：显示真实只读数据（阿拉伯数字）
      snprintf(buf, sizeof(buf), "积分%u 连续%u天",
               wbSnap.credits, wbSnap.streak);
      wbDrawMix(180, 216, buf, C_CREAM);
      snprintf(buf, sizeof(buf), "同意%u 拒绝%u 能量%u",
               wbSt.appr, wbSt.deny, wbSnap.energy);
    } else {
      snprintf(buf, sizeof(buf), "%s 等级%d",
               WB_SPECIES_CN[wbSt.species], wbSt.level + 1);
      wbDrawMix(180, 216, buf, C_CREAM);
      snprintf(buf, sizeof(buf), "同意%u 拒绝%u 精神%d",
               wbSt.appr, wbSt.deny, wbEnergyTier());
    }
    wbDrawMix(180, 234, buf, C_GREY);
  }
}

static void petRender() {
  petTick();
  gPetScroll = false;              // 下面画东西时若有走马灯会置位
  uiPage("桌面伙伴", COL(72, 92, 112));
  WbState st = wbState();
  char head[72], tot[32];
  if (wbSnap.running) {
    snprintf(tot, sizeof(tot), "进行%u", wbSnap.running);
  } else if (st == WB_CELEBRATE && wbSnap.done) {
    snprintf(tot, sizeof(tot), "完成%u", wbSnap.done);
  } else {
    snprintf(tot, sizeof(tot), "任务%u", wbSnap.total);
  }
  snprintf(head, sizeof(head), "%s  %s  %s",
           wbDemo ? "演示" : "实时", WB_STATE_CN[st], tot);
  wbDrawMix(180, 66, head, st == WB_ATTENTION ? C_YELLOW : C_GREY);
  wbDrawArt(WB_SPECIES[wbSt.species], st, millis() / 200UL);
  wbDrawInfo(st);
  if (st == WB_ATTENTION) {
    uiBtn(WB_BTN_L, WB_BTN_Y, WB_BTN_RAD, "同意", UI_G);
    uiBtn(WB_BTN_C, WB_BTN_Y, WB_BTN_RAD, "拒绝", UI_D);
    uiBtn(WB_BTN_R, WB_BTN_Y, WB_BTN_RAD, "换宠", UI_N);
  } else {
    uiBtn(WB_BTN_L, WB_BTN_Y, WB_BTN_RAD, "换宠", UI_A);
    uiBtn(WB_BTN_C, WB_BTN_Y, WB_BTN_RAD, wbDemo ? "实时" : "演示", UI_N);
    uiBtn(WB_BTN_R, WB_BTN_Y, WB_BTN_RAD, "状态", UI_N);
  }
}

static void petTap(int x, int y) {
  petTick();
  WbState st = wbState();
  if (st == WB_ATTENTION) {
    if (hit(x, y, WB_BTN_L, WB_BTN_Y, WB_BTN_RAD + 2)) { wbDecide(true);  showToast("已同意"); render(); return; }
    if (hit(x, y, WB_BTN_C, WB_BTN_Y, WB_BTN_RAD + 2)) { wbDecide(false); showToast("已拒绝"); render(); return; }
    if (hit(x, y, WB_BTN_R, WB_BTN_Y, WB_BTN_RAD + 2)) {
      wbSt.species = (wbSt.species + 1) % WB_N_SPECIES; wbStatsSave(); render(); return;
    }
    return;
  }
  if (hit(x, y, WB_BTN_L, WB_BTN_Y, WB_BTN_RAD + 2)) {
    wbSt.species = (wbSt.species + 1) % WB_N_SPECIES; wbStatsSave(); render(); return;
  }
  if (hit(x, y, WB_BTN_C, WB_BTN_Y, WB_BTN_RAD + 2)) {
    wbDemo = !wbDemo;
    cfgSetInt("wb.demo", wbDemo ? 1 : 0);
    wbDemoNext = 0;
    showToast(wbDemo ? "演示模式" : "实时模式");
    render(); return;
  }
  if (hit(x, y, WB_BTN_R, WB_BTN_Y, WB_BTN_RAD + 2)) {
    uint8_t next = ((uint8_t)wbState() + 1) % WB_STATE_COUNT;
    wbTrigger((WbState)next, 4500);
    render(); return;
  }
}

// ------------------------------------------------------------ 串口诊断
// PET / PETDEMO / PETSTATE <0..6> / PETNEXT / PETAPPROVE / PETDENY
// PETDIZZY / PETHEART / PETJSON <json>
static void petCmd(const String &cmd) {
  if (cmd == "PET") {
    Serial.printf("[WB] %s state=%s species=%s demo=%d connected=%d "
                  "T%d R%d W%d tok=%lu lvl=%d appr=%d deny=%d medVel=%d\n",
                  wbSnap.msg, WB_STATE_NAME[wbState()], WB_SPECIES[wbSt.species].name,
                  wbDemo, wbSnap.connected, wbSnap.total, wbSnap.running, wbSnap.waiting,
                  (unsigned long)wbSt.tokens, wbSt.level, wbSt.appr, wbSt.deny, wbMedianVel());
  } else if (cmd == "PETDEMO") {
    wbDemo = !wbDemo; wbDemoNext = 0; cfgSetInt("wb.demo", wbDemo ? 1 : 0);
    Serial.printf("[WB] demo=%d\n", wbDemo); render();
  } else if (cmd.startsWith("PETSTATE ")) {
    int s = cmd.substring(9).toInt();
    if (s >= 0 && s < WB_STATE_COUNT) { wbTrigger((WbState)s, 10000); render(); Serial.printf("[WB] force %s\n", WB_STATE_NAME[s]); }
  } else if (cmd == "PETNEXT") {
    wbSt.species = (wbSt.species + 1) % WB_N_SPECIES; wbStatsSave(); render();
    Serial.printf("[WB] species=%s\n", WB_SPECIES[wbSt.species].name);
  } else if (cmd == "PETAPPROVE") {
    wbDecide(true); render(); Serial.println("[WB] approved");
  } else if (cmd == "PETDENY") {
    wbDecide(false); render(); Serial.println("[WB] denied");
  } else if (cmd == "PETDIZZY") {
    wbTrigger(WB_DIZZY, 4000); render();
  } else if (cmd == "PETHEART") {
    wbTrigger(WB_HEART, 4000); render();
  } else if (cmd.startsWith("PETJSON ")) {
    petFeedJson(cmd.substring(8).c_str()); render();
  }
}

static void petInit() {
  wbStatsLoad();
  wbDemo = cfgGetInt("wb.demo", 1) != 0;
  memset(&wbSnap, 0, sizeof(wbSnap));
  wbSnap.lastMs = millis();
  wbDemoIdx = 0; wbDemoNext = 0;
  if (wbDemo) wbDemoApply();
  Serial.printf("[WB] buddy ready: %s demo=%d species=%s lvl=%d\n",
                wbDemo ? "DEMO" : "LIVE", wbDemo, WB_SPECIES[wbSt.species].name, wbSt.level + 1);
}

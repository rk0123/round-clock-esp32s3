#pragma once
// ============================================================
//  电子宠物 v1.0  ·  拓麻歌子升级版
//  状态：饱食 / 心情 / 精力 / 清洁 / 成长 / 年龄 / 生病
//  四个动作：喂食 / 玩耍 / 睡觉 / 洗澡；久病不治会死，可复活。
//  所有状态存 NVS（rcfg 命名空间），掉电保留。
// ============================================================
// 本文件在 main.cpp 的颜色宏之后 include；下面三个是 main.cpp 里的前向声明。
static void showToast(const char *s);
static bool hit(int x, int y, int cx, int cy, int r);
static void render();
static void drawBtnText(int x, int boxY, int boxH, const char *s, uint16_t col,
                        int font, int scale, int align);

struct PetState {
  int  hunger, mood, energy, clean, exp, ageMin, sick, dead, sleeping;
  uint32_t lastMs;
};
static PetState pet;

static void petSave() {
  cfgSetInt("pet.hg", pet.hunger);
  cfgSetInt("pet.md", pet.mood);
  cfgSetInt("pet.en", pet.energy);
  cfgSetInt("pet.cl", pet.clean);
  cfgSetInt("pet.ex", pet.exp);
  cfgSetInt("pet.ag", pet.ageMin);
  cfgSetInt("pet.sk", pet.sick);
  cfgSetInt("pet.dt", pet.dead);
  cfgSetInt("pet.sl", pet.sleeping);
}

static void petInit() {
  pet.hunger   = cfgGetInt("pet.hg", 72);
  pet.mood     = cfgGetInt("pet.md", 78);
  pet.energy   = cfgGetInt("pet.en", 82);
  pet.clean    = cfgGetInt("pet.cl", 75);
  pet.exp      = cfgGetInt("pet.ex", 0);
  pet.ageMin   = cfgGetInt("pet.ag", 0);
  pet.sick     = cfgGetInt("pet.sk", 0);
  pet.dead     = cfgGetInt("pet.dt", 0);
  pet.sleeping = cfgGetInt("pet.sl", 0);
  pet.lastMs   = millis();
  // 首次进入：给一组健康的初值，别让新宠物一上来就饿死
  if (pet.exp == 0 && pet.ageMin == 0 && pet.hunger == 72 && pet.mood == 78) {
    pet.hunger = 72; pet.mood = 78; pet.energy = 82; pet.clean = 75;
    petSave();
  }
  Serial.printf("[PET] load hg=%d md=%d en=%d cl=%d exp=%d age=%d dead=%d\n",
                pet.hunger, pet.mood, pet.energy, pet.clean, pet.exp, pet.ageMin, pet.dead);
}

// 按真实经过时间衰减；一分钟一档，最多补 12 小时，避免改系统时间后瞬间暴毙
static void petTick() {
  uint32_t now = millis();
  uint32_t dt  = now - pet.lastMs;
  if (dt < 60000UL) return;
  int mins = (int)(dt / 60000UL);
  if (mins > 720) mins = 720;
  pet.lastMs = now - (dt % 60000UL);
  if (pet.dead) return;
  pet.ageMin += mins;
  pet.hunger -= mins;                 // 饱食 -1 / 分钟
  pet.clean  -= mins / 2;             // 清洁 -0.5 / 分钟
  if (pet.sleeping) pet.energy += mins * 2;
  else              pet.energy -= mins;
  pet.hunger = constrain(pet.hunger, 0, 100);
  pet.clean  = constrain(pet.clean, 0, 100);
  pet.energy = constrain(pet.energy, 0, 100);
  // 心情由三项最差值决定，饿/累/脏都会掉心情；照顾好了自然回满
  int worst = min(pet.hunger, min(pet.energy, pet.clean));
  pet.mood = constrain(pet.mood + (worst >= 60 ? mins : -mins), 0, 100);
  if (worst <= 15) pet.sick += mins;
  else             pet.sick = max(0, pet.sick - mins * 2);
  if (pet.sick > 30) { pet.dead = 1; pet.sleeping = 0; }
  if (pet.hunger >= 50 && pet.clean >= 50 && pet.energy >= 50 && pet.mood >= 70)
    pet.exp += mins / 3;
  petSave();
}

static const char *petStage() {
  if (pet.dead)  return "已逝";
  if (pet.exp < 80)   return "幼体";
  if (pet.exp < 300)  return "成体";
  return "长老";
}
static const char *petMoodText() {
  if (pet.dead)   return "请复活";
  if (pet.sick)   return "生病了";
  if (pet.sleeping) return "睡梦中";
  if (pet.hunger < 25) return "好饿";
  if (pet.energy < 25) return "好困";
  if (pet.clean  < 25) return "好脏";
  if (pet.mood > 75)   return "很开心";
  return "还不错";
}

// ============================================================
//  宠物机本体 = 整块圆屏就是彩色像素游戏画面（无外壳）
//  蓝天 + 草地 + 太阳 + 彩色像素宠物；顶部 4 条状态，底部 4 个动作键。
//  四个动作：食 / 玩 / 睡 / 洗。
// ============================================================

// 圆内横带：只填 y0..y1 之间、且落在圆内的部分（草地 / 地平线）
static void petBand(int cx, int cy, int r, int y0, int y1, uint16_t c) {
  for (int y = y0; y <= y1; y++) {
    int dy = y - cy;
    if (dy * dy > r * r) continue;
    int dx = (int)(r * sqrtf(1.0f - (float)(dy * dy) / (float)(r * r)));
    fillRect(cx - dx, y, dx * 2 + 1, 1, c);
  }
}

// 彩屏里的像素精灵：8x6 点阵，每点 7~9px（随成长阶段变大）。
// 字符调色：Y=身体(黄) O=角/脚(橙) K=眼/描边(深) R=腮红(红)；'.'=透明。
// 形状与旧版完全一致，只把「单色墨点」换成彩色，睡眠/死亡换眼型。
static void petDrawBody(int cx, int cy) {
  bool blink = (millis() % 3600UL < 180UL);
  int  stage = pet.exp < 80 ? 0 : (pet.exp < 300 ? 1 : 2);
  bool dead = pet.dead, sick = pet.sick, sleeping = pet.sleeping;
  uint16_t cY, cO, cK, cR;
  if (dead) {                       // 死亡：整体灰化
    cY = COL(150, 150, 146); cO = COL(120, 120, 116);
    cK = COL(76, 76, 74);    cR = COL(128, 128, 124);
  } else if (sick) {                // 生病：泛青绿
    cY = COL(188, 206, 118); cO = COL(150, 172, 96);
    cK = COL(58, 70, 52);    cR = COL(150, 170, 120);
  } else {                          // 健康：明亮黄身 + 橙角脚 + 红腮
    cY = COL(252, 210, 76);  cO = COL(242, 148, 58);
    cK = COL(52, 44, 38);    cR = COL(238, 112, 104);
  }
  int px = 10 + stage;                                        // 单点边长（铺满大屏后放大）
  int gw = 8 * px, gh = 6 * px;
  int x0 = cx - gw / 2, y0 = cy - gh / 2 + 2;

  static const char *SPR[6] = {
    ".OO..OO.",   // 两只小角
    "YYYYYYYY",   // 头
    "YYKYYKYY",   // 大眼
    "RYYYYYYR",   // 脸 + 两侧腮红
    ".YYYYYY.",   // 身
    "..O..O.."    // 两只小脚
  };
  char spr[6][9];
  for (int r = 0; r < 6; r++) strcpy(spr[r], SPR[r]);
  if (dead) {
    strcpy(spr[2], "YKYYKYYK");   // 交叉闭眼（近似）
  } else if (sleeping || blink) {
    strcpy(spr[2], "YYYYYYYY");   // 闭眼一条线
  }
  for (int r = 0; r < 6; r++)
    for (int c = 0; c < 8; c++) {
      char ch = spr[r][c];
      uint16_t col;
      if      (ch == 'Y') col = cY;
      else if (ch == 'O') col = cO;
      else if (ch == 'K') col = cK;
      else if (ch == 'R') col = cR;
      else continue;
      fillRect(x0 + c * px, y0 + r * px, px - 1, px - 1, col);
    }
  if (sick && !dead) {              // 生病挂 +
    fillRect(cx + gw / 2 - 2, y0 - 10, 3, 9, cK);
    fillRect(cx + gw / 2 - 6, y0 - 6, 11, 3, cK);
  }
  if (sleeping && !dead)
    drawText(cx + gw / 2 - 2, y0 - 14, "Z", C_WHITE, 12, 1, 0);
}

// 彩屏顶部横排 4 条状态条（食/心/力/净）。屏铺满整圆后横向空间足够，
// 不再需要 2x2 挤在小圆里。
static void petLcdBars() {
  uint16_t cols[4] = { C_ORANGE, COL(236, 120, 140), C_BLUE, C_GREEN };
  int vals[4] = { pet.hunger, pet.mood, pet.energy, pet.clean };
  const char *labs[4] = {"F", "M", "E", "C"};
  for (int i = 0; i < 4; i++) {
    int bx = 70 + i * 58;           // 四个起点：70 / 128 / 186 / 244
    int by = 96;                    // 让开左上角返回键 (106,62) 热区
    drawText(bx, by - 4, labs[i], C_WHITE, 12, 1, 0);
    fillRect(bx + 11, by, 40, 7, COL(240, 248, 255));
    int w = 40 * constrain(vals[i], 0, 100) / 100;
    if (w > 0) fillRect(bx + 11, by, w, 7, cols[i]);
  }
}

static void petRender() {
  petTick();
  clear(C_DARK);

  // 整块圆屏就是彩色像素游戏画面（不再有精灵球外壳）
  const int CX = 180, CY = 180, R = 168;
  fillCircle(CX, CY, R + 4, COL(12, 18, 24));          // 深色外沿
  fillCircle(CX, CY, R, COL(126, 214, 244));           // 天空
  petBand(CX, CY, R, 156, 360, COL(122, 196, 92));     // 草地（逐行裁到圆内）
  petBand(CX, CY, R, 155, 157, COL(92, 158, 70));      // 地平线
  // v2.8：不再画返回键，退出靠左右/上滑手势（见 main.cpp navigate）

  // 太阳（右侧中部，避开顶部状态条，圆 + 十字光芒全在屏内）
  fillCircle(288, 132, 12, COL(255, 224, 84));
  fillCircle(288, 132, 8, COL(255, 238, 136));
  line(288, 112, 288, 116, COL(255, 224, 84), 3);
  line(288, 148, 288, 152, COL(255, 224, 84), 3);
  line(268, 132, 272, 132, COL(255, 224, 84), 3);
  line(304, 132, 308, 132, COL(255, 224, 84), 3);

  // 草地小花（红 / 黄，带绿茎）
  fillRect(92, 178, 5, 5, COL(244, 96, 108));
  fillRect(93, 183, 3, 7, COL(86, 158, 66));
  fillRect(266, 206, 5, 5, COL(255, 220, 92));
  fillRect(267, 211, 3, 7, COL(86, 158, 66));

  petLcdBars();                                        // 顶部 4 条状态
  // 脚下阴影（先画，被宠物压住中间，两侧露出）
  for (int dy = -3; dy <= 3; dy++) {
    int w = 42 - abs(dy) * 5;
    fillRect(CX - w, 236 + dy, w * 2, 1, COL(96, 164, 72));
  }
  petDrawBody(CX, 200);

  // 铭牌：阶段 + 年龄 + 心情（草地上深绿字）
  char tag[40];
  snprintf(tag, sizeof(tag), "%s %d  %s", petStage(), pet.ageMin, petMoodText());
  drawText(CX, 246, tag, COL(30, 66, 30), 16, 1, 1);

  // 底部四键：统一 UI 组件
  static const int BTX[4] = {120, 168, 216, 264};
  static const int BTY[4] = {286, 294, 294, 286};
  static const char *const BTL[4] = {"食", "玩", "睡", "洗"};
  for (int i = 0; i < 4; i++)
    uiBtn(BTX[i], BTY[i], 20, BTL[i], (i == 2 && pet.sleeping) ? UI_S : UI_N);
}

static void petTap(int x, int y) {
  petTick();
  static const int BTX[4] = {120, 168, 216, 264};
  static const int BTY[4] = {286, 294, 294, 286};
  for (int i = 0; i < 4; i++) {
    if (!hit(x, y, BTX[i], BTY[i], 26)) continue;
    if (pet.dead) {
      // 死后四颗键都变复活：重新给一组健康初值
      pet.dead = 0; pet.sick = 0; pet.hunger = 70; pet.mood = 75;
      pet.energy = 80; pet.clean = 70; pet.sleeping = 0;
      showToast("REBORN");
    } else if (i == 0) {          // 喂食
      pet.hunger = constrain(pet.hunger + 25, 0, 100);
      pet.exp += 4; pet.sick = max(0, pet.sick - 3);
      if (pet.sleeping) { pet.sleeping = 0; showToast("WAKE UP"); }
      else showToast("YUMMY");
    } else if (i == 1) {          // 玩耍
      if (pet.sleeping) { showToast("SLEEPING"); break; }
      pet.mood = constrain(pet.mood + 20, 0, 100);
      pet.energy = constrain(pet.energy - 10, 0, 100);
      pet.hunger = constrain(pet.hunger - 4, 0, 100);
      pet.exp += 6;
      showToast("HAPPY");
    } else if (i == 2) {          // 睡觉 / 起床
      pet.sleeping = !pet.sleeping;
      showToast(pet.sleeping ? "GOOD NIGHT" : "GOOD MORNING");
    } else {                      // 洗澡
      pet.clean = constrain(pet.clean + 30, 0, 100);
      pet.mood = constrain(pet.mood + 6, 0, 100);
      pet.sick = max(0, pet.sick - 8);
      showToast("CLEAN");
    }
    petSave();
    render();
    return;
  }
}

// 串口诊断：PET / PETFEED <n>
static void petCmd(const String &cmd) {
  if (cmd == "PET") {
    Serial.printf("[PET] %s age=%d hg=%d md=%d en=%d cl=%d exp=%d sick=%d dead=%d sleep=%d\n",
                  petStage(), pet.ageMin, pet.hunger, pet.mood, pet.energy,
                  pet.clean, pet.exp, pet.sick, pet.dead, pet.sleeping);
  } else if (cmd.startsWith("PETSET ")) {
    // 直接改四个主状态，方便验收表情与死亡逻辑：PETSET 10 10 10 10
    int a, b, c, d;
    if (sscanf(cmd.c_str(), "PETSET %d %d %d %d", &a, &b, &c, &d) == 4) {
      pet.hunger = a; pet.mood = b; pet.energy = c; pet.clean = d;
      petSave(); render();
      Serial.println("[PET] state injected");
    }
  }
}

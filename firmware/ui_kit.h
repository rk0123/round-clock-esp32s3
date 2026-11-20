#pragma once
// ============================================================
//  二期统一 UI 组件 v1.0
//  规则（所有二期页面共用，别再各写一套）：
//    · 页面底：clear(C_DARK) + fillCircle(180,180,172,COL(18,24,32))
//              + circle(180,180,168,ring,2)
//    · 标题  ：y=36 / FONT16 / 居中；分隔线 y=64, x=132..240
//    · 返回  ：v2.8 起页面一律不画返回键 —— 左右滑（或下滑）回上一层，上滑回表盘
//    · 圆键  ：一律 uiBtn()，半径 24（动作）/ 28（选项）/ 44（语音）
//              文字一律 FONT16（语音主键 24），避免 FONT12 中文空白
//    · 配色  ：UI_N 普通 / UI_A 强调橙 / UI_G 绿 / UI_D 危险红 / UI_S 选中
//  本头在 main.cpp 颜色宏之后、pet.h / words.h 之前 include。
// ============================================================
static void drawBtnText(int x, int boxY, int boxH, const char *s, uint16_t col,
                        int font, int scale, int align);

#define UI_N  0    // 普通：深蓝灰
#define UI_A  1    // 强调：橙
#define UI_G  2    // 绿（确认 / 添加）
#define UI_D  3    // 危险：红（清空 / 删除）
#define UI_S  4    // 选中：亮橙描边 + 黄字

#define UI_RING     COL(52, 64, 78)
#define UI_DIV      COL(60, 72, 86)
#define UI_PAGE_BG  COL(18, 24, 32)

// 统一页面底 + 标题 + 分隔线。ring 传 0 用默认冷灰。
// divider=false 用于标题下方要放整条胶囊/长控件的页面（分隔线会压在控件上）。
static void uiPage(const char *title, uint16_t ring = 0, bool divider = true) {
  clear(C_DARK);
  fillCircle(180, 180, 172, UI_PAGE_BG);
  circle(180, 180, 168, ring ? ring : UI_RING, 2);
  drawText(180, 36, title, C_CREAM, 16, 1, 1);
  if (divider) line(132, 64, 240, 64, UI_DIV);
}

// 统一圆键：label 用 FONT16（中文必需），主语音键可传 font=24。
static void uiBtn(int cx, int cy, int r, const char *label, int style, int font = 16) {
  uint16_t bg, bd, fg;
  switch (style) {
    case UI_A: bg = C_ORANGE_D;          bd = C_ORANGE;            fg = C_YELLOW; break;
    case UI_G: bg = COL(30, 72, 54);     bd = C_GREEN;             fg = COL(160, 240, 195); break;
    case UI_D: bg = COL(88, 40, 40);     bd = COL(196, 88, 88);    fg = COL(250, 195, 195); break;
    case UI_S: bg = COL(52, 42, 28);     bd = C_YELLOW;            fg = C_YELLOW; break;
    default:   bg = COL(38, 46, 58);     bd = COL(86, 100, 120);   fg = C_WHITE;  break;
  }
  // 立体感：下沉阴影 + 主体圆 + 内圈高光晕（不划横线，横线像"删除线"很丑）
  fillCircle(cx, cy + 2, r, mix(bg, C_DARK, 120));
  fillCircle(cx, cy, r, bg);
  circle(cx, cy, r, bd, 2);
  if (r >= 26) circle(cx, cy, r - 3, mix(bg, bd, 90), 1);
  int boxH = font + 8;
  drawBtnText(cx, cy - boxH / 2, boxH, label, fg, font, 1, 1);
}

// 统一矩形/胶囊选项键（单词四选一、设置项）。
static void uiPill(int x, int y, int w, int h, const char *label, int style,
                   int font = 16, int radius = 10) {
  uint16_t bg, bd, fg;
  switch (style) {
    case UI_A: bg = C_ORANGE_D;          bd = C_ORANGE;            fg = C_YELLOW; break;
    case UI_G: bg = COL(28, 76, 55);     bd = C_GREEN;             fg = COL(150, 240, 185); break;
    case UI_D: bg = COL(84, 40, 42);     bd = COL(220, 90, 90);    fg = COL(250, 190, 190); break;
    case UI_S: bg = COL(52, 42, 28);     bd = C_YELLOW;            fg = C_YELLOW; break;
    default:   bg = COL(38, 47, 60);     bd = COL(76, 90, 112);    fg = C_WHITE;  break;
  }
  fillRoundRect(x, y, w, h, radius, bg);
  roundRect(x, y, w, h, radius, bd);
  drawBtnText(x + w / 2, y + (h - 16) / 2, 16, label, fg, font, 1, 1);
}

// 统一蛋形/椭圆填充：逐行 fillRect，固件与离线预览用同一算法。
// cx,cy 中心；rx,ry 半轴；x/y 方向逐行扫描，圆整误差 ≤1px。
static void fillEllipse(int cx, int cy, int rx, int ry, uint16_t c) {
  for (int dy = -ry; dy <= ry; dy++) {
    float t = 1.0f - (float)(dy * dy) / (float)(ry * ry);
    if (t < 0) continue;
    int dx = (int)(rx * sqrtf(t));
    if (dx <= 0) continue;
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
  }
}

#include "gfx.h"
#include "font_data.h"
#include "font_aero.h"
#include "font_dial.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "Display_ST77916.h"

uint16_t *FB = nullptr;

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);
}

uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  uint8_t ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  uint8_t br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return rgb((ar * (255 - t) + br * t) * 8 / 255,
             (ag * (255 - t) + bg * t) * 4 / 255,
             (ab * (255 - t) + bb * t) * 8 / 255);
}

void gfxInit() {
  FB = (uint16_t *)ps_malloc(SCREEN_W * SCREEN_H * sizeof(uint16_t));
  if (!FB) {
    Serial.println("[GFX] PSRAM framebuffer alloc FAILED");
    while (1) delay(1000);
  }
  Serial.printf("[GFX] framebuffer %u bytes @ %p\n",
                SCREEN_W * SCREEN_H * 2, FB);
  clear(rgb(0, 0, 0));
}

// LCD_addWindow 会原地做 16-bit 字节交换，所以每帧必须全屏重绘，
// 分 8 块（每块 45 行）送屏，避免一次 259KB 的 DMA 传输。
void flushScreen() {
  for (int y = 0; y < SCREEN_H; y += 45) {
    LCD_addWindow(0, y, SCREEN_W - 1, y + 44, FB + y * SCREEN_W);
  }
}

void clear(uint16_t c) {
  uint32_t n = SCREEN_W * SCREEN_H;
  for (uint32_t i = 0; i < n; i++) FB[i] = c;
}

void setPixel(int x, int y, uint16_t c) {
  if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
  FB[y * SCREEN_W + x] = c;
}

void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  int x0 = max(0, x), y0 = max(0, y);
  int x1 = min(SCREEN_W, x + w), y1 = min(SCREEN_H, y + h);
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *p = FB + yy * SCREEN_W;
    for (int xx = x0; xx < x1; xx++) p[xx] = c;
  }
}

void rect(int x, int y, int w, int h, uint16_t c, int t) {
  fillRect(x, y, w, t, c);
  fillRect(x, y + h - t, w, t, c);
  fillRect(x, y, t, h, c);
  fillRect(x + w - t, y, t, h, c);
}

void fillCircle(int cx, int cy, int r, uint16_t c) {
  if (r < 0) return;
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrtf((float)(r * r - dy * dy));
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
  }
}

void circle(int cx, int cy, int r, uint16_t c, int t) {
  // 中点圆 + 8 对称，thickness 用半径 t/2 的小圆补
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    int pts[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y},
                     {-x, -y}, {-y, -x}, {y, -x}, {x, -y}};
    for (auto &p : pts) {
      if (t <= 1) setPixel(cx + p[0], cy + p[1], c);
      else fillCircle(cx + p[0], cy + p[1], t / 2, c);
    }
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

void line(int x0, int y0, int x1, int y1, uint16_t c, int t) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = max(abs(dx), abs(dy));
  if (steps == 0) { fillCircle(x0, y0, t / 2, c); return; }
  for (int i = 0; i <= steps; i++) {
    int x = x0 + dx * i / steps, y = y0 + dy * i / steps;
    if (t <= 1) setPixel(x, y, c);
    else fillCircle(x, y, t / 2, c);
  }
}

void arcRing(int cx, int cy, int r, int a0, int a1, int width, uint16_t c) {
  if (a1 < a0) return;
  float step = 0.25f;
  for (float a = a0; a <= a1; a += step) {
    float rad = (a - 90.0f) * PI / 180.0f;
    float cs = cosf(rad), sn = sinf(rad);
    for (int d = -width / 2; d <= width / 2; d++) {
      int rr = r + d;
      setPixel(cx + (int)(rr * cs), cy + (int)(rr * sn), c);
    }
  }
}

static inline float edgeFn(int ax, int ay, int bx, int by, int px, int py) {
  return (bx - ax) * (float)(py - ay) - (by - ay) * (float)(px - ax);
}
void fillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
  int minx = min(min(x0, x1), x2), maxx = max(max(x0, x1), x2);
  int miny = min(min(y0, y1), y2), maxy = max(max(y0, y1), y2);
  float area = edgeFn(x0, y0, x1, y1, x2, y2);
  if (fabsf(area) < 0.001f) return;
  bool pos = area > 0;
  for (int y = miny; y <= maxy; y++) {
    for (int x = minx; x <= maxx; x++) {
      float w0 = edgeFn(x1, y1, x2, y2, x, y);
      float w1 = edgeFn(x2, y2, x0, y0, x, y);
      float w2 = edgeFn(x0, y0, x1, y1, x, y);
      if (pos ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
              : (w0 <= 0 && w1 <= 0 && w2 <= 0)) setPixel(x, y, c);
    }
  }
}

void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
  if (r <= 0) { fillRect(x, y, w, h, c); return; }
  fillRect(x + r, y, w - 2 * r, h, c);
  fillRect(x, y + r, w, h - 2 * r, c);
  fillCircle(x + r, y + r, r, c);
  fillCircle(x + w - r - 1, y + r, r, c);
  fillCircle(x + r, y + h - r - 1, r, c);
  fillCircle(x + w - r - 1, y + h - r - 1, r, c);
}

void roundRect(int x, int y, int w, int h, int r, uint16_t c) {
  // 用外实心圆角矩形减内实心圆角矩形（背景色由调用方先铺好，这里用描边近似）
  arcRing(x + r, y + r, r, 180, 270, 1, c);
  arcRing(x + w - r - 1, y + r, r, 270, 360, 1, c);
  arcRing(x + r, y + h - r - 1, r, 90, 180, 1, c);
  arcRing(x + w - r - 1, y + h - r - 1, r, 0, 90, 1, c);
  line(x + r, y, x + w - r, y, c);
  line(x + r, y + h - 1, x + w - r, y + h - 1, c);
  line(x, y + r, x, y + h - r, c);
  line(x + w - 1, y + r, x + w - 1, y + h - r, c);
}

void radialGradientV(int y0, int y1, uint16_t top, uint16_t bottom) {
  for (int y = y0; y <= y1; y++) {
    uint8_t t = (uint8_t)(255L * (y - y0) / max(1, y1 - y0));
    uint16_t c = mix(top, bottom, t);
    for (int x = 0; x < SCREEN_W; x++) FB[y * SCREEN_W + x] = c;
  }
}

// ---------- 文字 ----------
// UTF-8 -> 码点；返回消耗的字节数
static int utf8Next(const char *s, uint32_t &cp) {
  uint8_t c = (uint8_t)s[0];
  if (c < 0x80) { cp = c; return 1; }
  if ((c & 0xE0) == 0xC0 && s[1]) {
    cp = ((c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F); return 2;
  }
  if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
    cp = ((c & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
    return 3;
  }
  // 4 字节（emoji / 生僻字）：字库没有这些字形，但要整体吞掉，
  // 否则会被拆成 4 个 '?' 画出来，小宇宙播客标题里很常见。
  if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
    cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((uint8_t)s[1] & 0x3F) << 12) |
         ((uint32_t)((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F);
    return 4;
  }
  cp = '?'; return 1;
}

static int glyphIndex(uint32_t cp, int font) {
  if (font == 12) return (cp >= 32 && cp <= 126) ? (int)(cp - 32) : -1;
  int asciiCount, cjkCount;
  const uint16_t *cjkCp;
  if (font == 16) {
    asciiCount = FONT16_ASCII_COUNT; cjkCount = FONT16_CJK_COUNT; cjkCp = FONT16_CJK_CP;
  } else {
    asciiCount = FONT24_ASCII_COUNT; cjkCount = FONT24_CJK_COUNT; cjkCp = FONT24_CJK_CP;
  }
  if (cp >= 32 && cp <= 126) return (int)(cp - 32);
  // v0.16：FONT16 铺了 GB2312 全部 6763 汉字，线性扫表每帧每字都要走几千次，
  // 改成二分查找（font_data.h 里的 CJK_CP[] 已按 Unicode 码点升序生成）。
  int lo = 0, hi = cjkCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t v = cjkCp[mid];
    if (v == cp) return asciiCount + mid;
    if (v < cp) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

static int charAdvance(uint32_t cp, int font, int scale) {
  int base;
  if (font == 12)      base = 8;
  else if (font == 16) base = (cp < 128) ? 10 : 16;
  else                 base = (cp < 128) ? 14 : 24;
  return base * scale;
}

int textWidth(const char *s, int font, int scale) {
  int w = 0; uint32_t cp;
  while (*s) { int n = utf8Next(s, cp); s += n; w += charAdvance(cp, font, scale); }
  return w;
}

int drawText(int x, int y, const char *s, uint16_t color, int font, int scale, int align) {
  int total = textWidth(s, font, scale);
  const int size = (font == 12) ? 12 : (font == 16) ? 16 : 24;
  const int rowb = (font == 12) ? FONT12_ROW_BYTES
                 : (font == 16) ? FONT16_ROW_BYTES : FONT24_ROW_BYTES;
  const uint8_t *tab = (font == 12) ? &FONT12_GLYPHS[0][0]
                     : (font == 16) ? &FONT16_GLYPHS[0][0] : &FONT24_GLYPHS[0][0];
  if (align == 1) {
    // 旧算法直接 x -= total/2，但点阵 canvas 比字符步进宽（FONT24 ASCII 24 vs 14），
    // 整串会系统性右偏 (24-14)/2*scale，放大 3 倍时右偏 15px，冒号自然不在圆心。
    // 这里按真实绘制宽度 = 步进和 - 末字步进 + 末字完整 canvas 来居中。
    const char *p = s;
    uint32_t cp;
    int lastAdv = size * scale;
    while (*p) { int n = utf8Next(p, cp); p += n; lastAdv = charAdvance(cp, font, scale); }
    x -= (total - lastAdv + size * scale) / 2;
  } else if (align == 2) x -= total;
  int ox = x;
  uint32_t cp;
  while (*s) {
    int n = utf8Next(s, cp); s += n;
    int gi = glyphIndex(cp, font);
    int adv = charAdvance(cp, font, scale);
    if (gi >= 0) {
      const uint8_t *g = tab + (size_t)gi * size * rowb;
      for (int gy = 0; gy < size; gy++) {
        for (int gx = 0; gx < size; gx++) {
          if (g[gy * rowb + (gx >> 3)] & (0x80 >> (gx & 7))) {
            if (scale == 1) setPixel(ox + gx, y + gy, color);
            else fillRect(ox + gx * scale, y + gy * scale, scale, scale, color);
          }
        }
      }
    }
    ox += adv;
  }
  return total;
}

// ---------------- 4-bit 抗锯齿数字 ----------------
// FONT_BIG_AA：44x64，每字节两个 4-bit 灰度像素（高半字节 = 左像素）
static inline uint8_t aaPix(const uint8_t *g, int x, int y) {
  if (x < 0 || y < 0 || x >= FONT_BIG_W || y >= FONT_BIG_H) return 0;
  uint8_t p = g[y * FONT_BIG_ROW_BYTES + (x >> 1)];
  return (x & 1) ? (p & 0x0F) : (p >> 4);
}

static inline void blendPixel(int x, int y, uint16_t c, uint8_t t) {
  if (t == 0 || x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
  uint32_t i = (uint32_t)y * SCREEN_W + x;
  uint16_t b = FB[i];
  uint32_t br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  uint32_t cr = c >> 11, cg = (c >> 5) & 63, cb = c & 31;
  uint32_t r  = (br * (255 - t) + cr * t) / 255;
  uint32_t g2 = (bg * (255 - t) + cg * t) / 255;
  uint32_t bl = (bb * (255 - t) + cb * t) / 255;
  FB[i] = (uint16_t)((r << 11) | (g2 << 5) | bl);
}

// step: 8.8 定点「每输出像素对应多少母版像素」。204 = 4/5（5/4 放大），512 = 2（1/2 缩小）
static void aaBlit(const uint8_t *g, int x0, int y0, int outW, int outH,
                   int step, uint16_t color) {
  const int half = step / 2 - 128;      // 中心采样：step=2 时恰为 2x2 盒式平均
  for (int oy = 0; oy < outH; oy++) {
    int py = y0 + oy;
    if (py < 0 || py >= SCREEN_H) continue;
    int syf = oy * step + half; if (syf < 0) syf = 0;
    int sy = syf >> 8, fy = syf & 255;
    for (int ox = 0; ox < outW; ox++) {
      int px = x0 + ox;
      if (px < 0 || px >= SCREEN_W) continue;
      int sxf = ox * step + half; if (sxf < 0) sxf = 0;
      int sx = sxf >> 8, fx = sxf & 255;
      int a00 = aaPix(g, sx, sy),     a10 = aaPix(g, sx + 1, sy);
      int a01 = aaPix(g, sx, sy + 1), a11 = aaPix(g, sx + 1, sy + 1);
      int a = (a00 * (256 - fx) * (256 - fy) + a10 * fx * (256 - fy) +
               a01 * (256 - fx) * fy       + a11 * fx * fy + 32768) >> 16;
      if (a > 0) blendPixel(px, py, color, (uint8_t)(a * 17));
    }
  }
}

static int aaAdvance(char c, int digitAdv) {
  if (c == ':') return digitAdv * 65 / 100;
  if (c == '.' || c == ' ') return digitAdv / 2;
  if (c == '-' || c == '/') return digitAdv * 85 / 100;
  return digitAdv;
}

int aaTextWidth(const char *s, bool big) {
  int d = big ? 50 : 20, w = 0;
  while (*s) { w += aaAdvance(*s++, d); }
  return w;
}

static int aaText(int x, int y, const char *s, uint16_t color, bool big, int align) {
  const int step = big ? 204 : 512;          // 5/4 放大 / 1/2 缩小
  const int outW = big ? 55 : 22;
  const int outH = big ? 80 : 32;
  const int dAdv = big ? 50 : 20;
  const char *begin = s;
  int adv[24], n = 0, total = 0, colon = -1;
  while (*s && n < 24) {
    adv[n] = aaAdvance(*s, dAdv);
    if (*s == ':' && colon < 0) colon = n;
    total += adv[n];
    s++; n++;
  }
  if (n == 0) return 0;
  int center = total / 2;
  if (colon >= 0) {
    center = 0;
    for (int i = 0; i < colon; i++) center += adv[i];
    center += adv[colon] / 2;               // 冒号墨迹中心
  }
  int cur = (align == 1) ? (x - center) : (align == 2) ? (x - total) : x;
  const char *p = begin;
  for (int i = 0; i < n; i++, p++) {
    int gi = -1;
    for (int k = 0; k < FONT_BIG_COUNT; k++)
      if (FONT_BIG_CHARS[k] == *p) { gi = k; break; }
    if (gi >= 0 && *p != ' ') {
      // 让字形墨迹中心（母版 x=22）精确对齐字符槽中心
      int ox = cur + (adv[i] - outW) / 2;
      aaBlit(FONT_BIG_AA[gi], ox, y, outW, outH, step, color);
    }
    cur += adv[i];
  }
  return total;
}

int drawBigText(int cx, int y, const char *s, uint16_t color) {
  return aaText(cx, y, s, color, true, 1);
}

int drawMidText(int x, int y, const char *s, uint16_t color, int align) {
  return aaText(x, y, s, color, false, align);
}

int drawMidTextBold(int x, int y, const char *s, uint16_t color, int align) {
  aaText(x, y, s, color, false, align);
  return aaText(x + 1, y, s, color, false, align);   // 右移 1px 再叠一层
}

void drawDegree(int x, int y, uint16_t color, int r) {
  circle(x, y, max(1, r), color, 1);
}

// 花体「AERO」：整词一张 4-bit AA 位图，1:1 贴到帧缓冲，逐像素 alpha 混合。
// 不做缩放 —— Vladimir Script 是细线书法，一旦放大就会糊成一团。
void drawAeroLogo(int cx, int y, uint16_t color) {
  int x0 = cx - AERO_LOGO_W / 2;
  for (int oy = 0; oy < AERO_LOGO_H; oy++) {
    int py = y + oy;
    if (py < 0 || py >= SCREEN_H) continue;
    const uint8_t *row = AERO_LOGO_AA + oy * AERO_LOGO_ROW_BYTES;
    for (int ox = 0; ox < AERO_LOGO_W; ox++) {
      uint8_t p = row[ox >> 1];
      uint8_t a = (ox & 1) ? (uint8_t)(p & 0x0F) : (uint8_t)(p >> 4);
      if (a) blendPixel(x0 + ox, py, color, (uint8_t)(a * 17));
    }
  }
}

// 衬线字（日期窗 / 星期窗）：Baskerville 4-bit AA，比例字宽，逐像素 alpha 混合。
// 生成器把每个字形按自身墨迹宽度居中放进 DIAL_W 宽的画布，所以绘制时要回退 (DIAL_W - inkW)/2，
// 否则窄字（如 '1'）会系统性右偏。baselineY 是**基线**——数字 cap 24、字母 cap 16，
// 两者共用基线，视觉上才算排在一条线上。
int drawDialText(int cx, int baselineY, const char *s, uint16_t color) {
  int total = 0;
  for (const char *p = s; *p; p++) {
    char c = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    const char *q = strchr(DIAL_CHARS, c);
    total += q ? DIAL_ADV[q - DIAL_CHARS] : DIAL_W;
  }
  if (total == 0) return 0;
  total -= 1;                          // 末字不带 1px 字距
  int cur = cx - total / 2;
  const int y0 = baselineY - DIAL_H;
  for (const char *p = s; *p; p++) {
    char c = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    const char *q = strchr(DIAL_CHARS, c);
    if (!q) { cur += DIAL_W; continue; }
    int gi = (int)(q - DIAL_CHARS);
    int inkW = DIAL_ADV[gi] - 1;
    int off = (DIAL_W - inkW) / 2;
    const uint8_t *g = DIAL_AA[gi];
    for (int oy = 0; oy < DIAL_H; oy++) {
      int py = y0 + oy;
      if (py < 0 || py >= SCREEN_H) continue;
      const uint8_t *row = g + oy * DIAL_ROW_BYTES;
      for (int ox = 0; ox < DIAL_W; ox++) {
        uint8_t b = row[ox >> 1];
        uint8_t a = (ox & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
        if (a) blendPixel(cur - off + ox, py, color, (uint8_t)(a * 17));
      }
    }
    cur += DIAL_ADV[gi];
  }
  return total;
}

void polar(int cx, int cy, int r, float deg, int &x, int &y) {
  float a = (deg - 90.0f) * PI / 180.0f;
  x = cx + (int)(r * cosf(a));
  y = cy + (int)(r * sinf(a));
}

// 表盘双窗的立体窗框。假定光从左上来（和表盘三针的高光方向一致）：
//   框顶面（朝上/朝左）受光 -> 暖银高光
//   框外侧右下（背光）      -> 中性灰，仍比背景亮，靠下方 1px 投影定出边界
//   槽内左壁（朝右）        -> 最暗，光进不去
//   槽内右下壁（朝左上）    -> 受光的暖银，比外框稍弱
// 四个灰阶 238/196/128/58 拉开层次，1px 就够在 360 屏上读出立体感。
// 表盘双窗的立体窗框。假定光从左上来（和表盘三针的高光方向一致），三圈同心边：
//   第 0 圈：外沿金属棱 —— 左上最亮（240）、右下背光（128）
//   第 1 圈：框的金属面 —— 左上次亮（200）、右下更暗（96）
//   第 2 圈：槽壁，方向反转 —— 左上暗（46，光进不去）、右下亮（150，受反射）
//   内部墨底。窗外右下再加 1px 投影，窗就浮起来了。
// 每相邻两圈都是"亮-暗"或"暗-亮"反差，1px 宽也能读出斜面。
void drawDialWindow(int x, int y, int w, int h) {
  const uint16_t HI  = rgb(246, 240, 220);   // 0 圈 左上：棱线受光
  const uint16_t HI2 = rgb(205, 199, 180);   // 1 圈 左上：金属面
  const uint16_t HI3 = rgb(195, 194, 182);   // 2 圈 右下：槽壁反光（最亮的一档内侧）
  const uint16_t MID = rgb(145, 146, 143);   // 0 圈 右下：金属背光
  const uint16_t MID2= rgb(112, 115, 116);   // 1 圈 右下：背光更深
  const uint16_t DK  = rgb(74, 80, 86);      // 2 圈 左上：槽壁阴影（不能压到和墨底同色，
                                             // 否则读不出"槽壁上沿"，只是边框变粗）
  const uint16_t INK = rgb(9, 14, 20);       // 窗内墨底
  const uint16_t SH  = rgb(4, 7, 10);        // 投影

  // 窗外右下 2px 投影：1px 太细，360 屏上几乎看不见
  fillRect(x + 1, y + h, w + 1, 2, SH);
  fillRect(x + w, y + 1, 2, h + 1, SH);

  // 第 0 圈：外沿金属棱
  fillRect(x, y, w, 1, HI);
  fillRect(x, y, 1, h, HI);
  fillRect(x, y + h - 1, w, 1, MID);
  fillRect(x + w - 1, y, 1, h, MID);

  // 第 1 圈：金属面
  fillRect(x + 1, y + 1, w - 2, 1, HI2);
  fillRect(x + 1, y + 1, 1, h - 2, HI2);
  fillRect(x + 1, y + h - 2, w - 2, 1, MID2);
  fillRect(x + w - 2, y + 1, 1, h - 2, MID2);

  // 第 2 圈：槽壁（方向反转，槽是沉进去的）
  fillRect(x + 2, y + 2, w - 4, 1, DK);
  fillRect(x + 2, y + 2, 1, h - 4, DK);
  fillRect(x + 2, y + h - 3, w - 4, 1, HI3);
  fillRect(x + w - 3, y + 2, 1, h - 4, HI3);

  // 墨底
  fillRect(x + 3, y + 3, w - 6, h - 6, INK);
}

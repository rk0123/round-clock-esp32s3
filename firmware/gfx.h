#pragma once
#include <stdint.h>
#include "config.h"

// 360x360 RGB565 帧缓冲（小端），每帧全屏重绘后 flushScreen() 送屏
extern uint16_t *FB;

void     gfxInit();
void     flushScreen();

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b);
uint16_t mix(uint16_t a, uint16_t b, uint8_t t);   // t=0..255

void clear(uint16_t c);
void setPixel(int x, int y, uint16_t c);
void fillRect(int x, int y, int w, int h, uint16_t c);
void rect(int x, int y, int w, int h, uint16_t c, int t = 1);
void roundRect(int x, int y, int w, int h, int r, uint16_t c);
void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c);
void fillCircle(int cx, int cy, int r, uint16_t c);
void circle(int cx, int cy, int r, uint16_t c, int t = 1);
void line(int x0, int y0, int x1, int y1, uint16_t c, int t = 1);
void arcRing(int cx, int cy, int r, int a0, int a1, int width, uint16_t c);
void fillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void radialGradientV(int y0, int y1, uint16_t top, uint16_t bottom);

// font: 12 / 16 / 24（1-bit 点阵）；scale: 1..4；align 0=left 1=center 2=right
int textWidth(const char *s, int font, int scale);
int drawText(int x, int y, const char *s, uint16_t color, int font, int scale, int align = 0);

// 4-bit 抗锯齿数字：由 44x64 母版双线性缩放而来，alpha 混合，不再有 1-bit 放大锯齿。
// drawBigText  : 5/4 放大 -> 55x80 字形，墨迹约 57px 高（电子表盘时间 / 番茄钟）
// drawMidText  : 1/2 缩小 -> 22x32 字形，墨迹约 23px 高（温度 / 秒 / 日期）
// cx / x 为对齐基准；align=1（居中）时若有冒号，冒号的墨迹中心精确落在 cx 上。
int aaTextWidth(const char *s, bool big);
int drawBigText(int cx, int y, const char *s, uint16_t color);
int drawMidText(int x, int y, const char *s, uint16_t color, int align = 1);
// 加粗版：同一串 AA 字形按 1px 水平偏移叠绘两遍，笔画向右扩 1px 且墨色更实。
// 用于模拟表盘日期窗，和左侧 24px 点阵中文的笔画重量对齐。
int drawMidTextBold(int x, int y, const char *s, uint16_t color, int align = 1);
void drawDegree(int x, int y, uint16_t color, int r = 2);

// 模拟表盘顶部「AERO」花体标志（Vladimir Script，121x32 4-bit AA，见 font_aero.h）
// cx = 水平中心，y = 位图顶部。整词一张位图，不与点阵字库混排。
void drawAeroLogo(int cx, int y, uint16_t color);

// 模拟表盘日期窗 / 星期窗的衬线字（Palatino Bold，0-9 cap 27px、A-Z cap 18px，见 font_dial.h）。
// cx = 水平中心，baselineY = **基线**（字形底边所在行）。数字与字母字高不同，
// 各自在窗内垂直居中即可，不必共用基线。比例字宽，字符间不带额外字距。只支持 0-9 与 A-Z。
int drawDialText(int cx, int baselineY, const char *s, uint16_t color);

// 表盘双窗的立体窗框（v0.25）：金属倒角 + 内凹槽 + 右下投影。
// 上/左受光 -> 亮银，下/右背光 -> 暗灰，形成 45° 光的斜面；
// 第二圈反向（上/左暗、下/右亮）做出凹进去的槽壁，所以框是凸起的、窗是沉的。
// 墨底从 +2 开始，内区 = (w-4) x (h-4)。
void drawDialWindow(int x, int y, int w, int h);

// 极坐标辅助：0° = 正北，顺时针
void polar(int cx, int cy, int r, float deg, int &x, int &y);

# -*- coding: utf-8 -*-
"""生成背单词用的衬线英文字库（Georgia，4-bit AA，30px）。

为什么不用现成的 FONT24 / font_dial.h：
  · FONT24 的 ASCII 是无衬线点阵，24px 下单词像代码；
  · font_dial.h 是 Palatino 表盘字，但只有 0-9 / A-Z 大写，单词表是小写。

输出两个文件：
  src/serif_font.cpp  —— 95 个 ASCII 的字模 + 绘制实现（Arduino 递归编译 src/，独立 TU）
  font_serif.h        —— 只有尺寸常量和函数声明（避免把大数组塞进 sketch 单一编译单元）

字模格式：固定 32x36 画布 / 基线 y=28，每行两个像素打包进一个字节
（高 4 位 = 左像素，低 4 位 = 右像素），alpha 0..15 直接喂 mix(bg, fg, a*17)。

用法：
    python tools/gen_serif_font.py
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT_H = ROOT / "font_serif.h"
OUT_CPP = ROOT / "src" / "serif_font.cpp"
FONT = "C:/Windows/Fonts/georgia.ttf"
SIZE = 30
CELL_W, CELL_H = 32, 36
BASELINE = 28

font = ImageFont.truetype(FONT, SIZE)

adv = []
bits = []
for code in range(32, 127):
    ch = chr(code)
    img = Image.new("L", (CELL_W, CELL_H), 0)
    ImageDraw.Draw(img).text((1, BASELINE), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    rowbytes = []
    for y in range(CELL_H):
        for x in range(0, CELL_W, 2):
            lo = px[x, y] >> 4
            hi = px[x + 1, y] >> 4
            rowbytes.append((lo << 4) | hi)
    bits.append(rowbytes)
    a = round(font.getlength(ch)) + 1
    adv.append(max(4, min(CELL_W, a)))

OUT_H.write_text("""#pragma once
// 自动生成，勿手改；改字体/字号请跑 tools/gen_serif_font.py
// Georgia 30px 衬线英文，4-bit AA。数据/实现在 src/serif_font.cpp。
#include <stdint.h>
#define SERIF_W   32
#define SERIF_H   36
#define SERIF_BASELINE 28
#define SERIF_FIRST 32

// cx = 水平中心，baselineY = 基线（小写 g/p/y 会落到基线下方）
// 返回实际像素宽度；必须 include 本头文件之后才能用（不需要 gfx.h 类型）。
int serifTextWidth(const char *s);
int drawSerifText(int cx, int baselineY, const char *s, uint16_t color);
""", encoding="utf-8", newline="\n")

with OUT_CPP.open("w", encoding="utf-8", newline="\n") as f:
    f.write("""// 自动生成，勿手改；改字体/字号请跑 tools/gen_serif_font.py
// Georgia 30px 衬线英文，4-bit AA：每行 2 像素/字节（高 4 位左、低 4 位右）。
#include <Arduino.h>
#include "gfx.h"
#include "font_serif.h"

static const uint8_t SERIF_ADV[95] = {
""")
    for i in range(0, 95, 16):
        f.write("  " + ",".join(str(v) for v in adv[i:i + 16]) + ",\n")
    f.write("};\n\nstatic const uint8_t SERIF_AA[95][SERIF_H * SERIF_W / 2] = {\n")
    for i, rows in enumerate(bits):
        f.write("  { // '%s' %d\n    " % (chr(32 + i).replace("'", "\\'"), 32 + i))
        for j in range(0, len(rows), 16):
            f.write(",".join("0x%02X" % v for v in rows[j:j + 16]))
            f.write(",\n    " if j + 16 < len(rows) else "\n")
        f.write("  },\n")
    f.write("};\n\n")

    f.write("""// 4-bit AA 直接和帧缓冲里的背景色混合，不用先清块。
int serifTextWidth(const char *s) {
  if (!s) return 0;
  int w = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    int i = (*p >= SERIF_FIRST && *p < SERIF_FIRST + 95) ? *p - SERIF_FIRST : 0;
    w += SERIF_ADV[i];
  }
  return w;
}

int drawSerifText(int cx, int baselineY, const char *s, uint16_t color) {
  if (!s || !*s) return 0;
  int x = cx - serifTextWidth(s) / 2;
  int y0 = baselineY - SERIF_BASELINE;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    int i = (*p >= SERIF_FIRST && *p < SERIF_FIRST + 95) ? *p - SERIF_FIRST : 0;
    int adv = SERIF_ADV[i];
    for (int row = 0; row < SERIF_H; ++row) {
      int py = y0 + row;
      if (py < 0 || py >= 360) continue;
      const uint8_t *src = SERIF_AA[i] + row * (SERIF_W / 2);
      for (int col = 0; col < SERIF_W; ++col) {
        int a = (col & 1) ? (src[col >> 1] & 0x0F) : (src[col >> 1] >> 4);
        if (!a) continue;
        int px = x + col;
        if (px < 0 || px >= 360) continue;
        uint16_t &dst = FB[py * 360 + px];
        dst = mix(dst, color, a * 17);
      }
    }
    x += adv;
  }
  return serifTextWidth(s);
}
""")

print("wrote %s + %s (%d chars, ~%d bytes glyphs)" %
      (OUT_H, OUT_CPP, len(bits), sum(len(r) for r in bits)))

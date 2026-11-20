#pragma once
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

# -*- coding: utf-8 -*-
"""生成模拟表盘的「AERO」花体标志 -> font_aero.h

  用 Windows 自带的 Vladimir Script（细线英文书法体），渲染整词一张 4-bit
  灰度位图，绘制时与背景做 alpha 混合（复用 gfx.cpp 的 blendPixel），
  所以边缘不会有 1-bit 点阵的锯齿。

  Vladimir Script 笔画细、有压力变化，这本身就是它优雅的地方。
  **不要做膨胀加粗**（v0.22 第一版加了 3x3 MaxFilter，屏幕上又粗又钝，被否掉）。

  输出：AERO_LOGO_W / AERO_LOGO_H / AERO_LOGO_AA[]
        每字节两个像素（高半字节=左像素），值 0..15。
"""
from PIL import Image, ImageDraw, ImageFont
import os

FONT = "C:/Windows/Fonts/VLADIMIR.TTF"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "font_aero.h")
TARGET_W = 88           # 目标墨迹宽度：约占屏宽 24%，压在 12 点下方不抢指针

# 1) 用大字号渲染再测量真实墨迹框，避免靠猜字号
def render(size):
    f = ImageFont.truetype(FONT, size)
    img = Image.new("L", (size * 5, size * 3), 0)
    d = ImageDraw.Draw(img)
    d.text((size, size), "AERO", font=f, fill=255)
    bb = img.getbbox()
    return img.crop(bb) if bb else None

# 二分找最接近 TARGET_W 的字号
lo, hi, best = 12, 200, None
while lo <= hi:
    mid = (lo + hi) // 2
    im = render(mid)
    if im is None:
        lo = mid + 1
        continue
    if im.width <= TARGET_W:
        best = (mid, im)
        lo = mid + 1
    else:
        hi = mid - 1
size, img = best
print(f"font size={size}  ink={img.width}x{img.height}")

W, H = img.size
print(f"final={W}x{H} (no dilation, keep the hairline strokes)")

# 3) 转 4-bit 打包（高半字节=偶数列）
row_bytes = (W + 1) // 2
data = bytearray()
px = img.load()
for y in range(H):
    for xb in range(row_bytes):
        x0 = xb * 2
        hi = px[x0, y] >> 4
        lo = (px[x0 + 1, y] >> 4) if x0 + 1 < W else 0
        data.append(((hi & 0x0F) << 4) | (lo & 0x0F))

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("// 自动生成，请勿手改。生成脚本: tools/gen_aero.py\n")
    f.write("// 模拟表盘顶部「AERO」花体标志（Vladimir Script \u2192 4-bit AA）\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"#define AERO_LOGO_W {W}\n")
    f.write(f"#define AERO_LOGO_H {H}\n")
    f.write(f"#define AERO_LOGO_ROW_BYTES {row_bytes}\n\n")
    f.write(f"// {W}x{H} 4-bit AA, {len(data)} bytes\n")
    f.write("static const uint8_t AERO_LOGO_AA[] = {\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",\n")
    f.write("};\n")

print(f"written {OUT}  ({len(data)} bytes)")

# 顺手出一张预览（白字黑底，模拟屏幕效果）
prev = Image.new("RGB", (180, 60), (17, 26, 33))
prev.paste(Image.merge("RGB", [img, img, img]), (29, 10), img)
prev.resize((360, 120), Image.NEAREST).save(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "_aero_preview.png"))

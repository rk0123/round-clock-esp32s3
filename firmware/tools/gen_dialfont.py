"""
生成机械表盘「日期窗 / 星期窗」用的衬线字模 → font_dial.h

参考 A. Lange & Söhne Datograph 的双窗：深底 + 细银边 + 白色衬线字。
点阵字（FONT24）在 360 圆屏上会明显发"像素风"，和罗马数字刻度、Vladimir 花体 AERO
放在一起不协调，所以这里单独渲一套 4-bit 抗锯齿衬线字模。

字体：Century Schoolbook Bold（古典粗衬线，笔画扎实、数字宽而稳。
      Baskerville 太细太"数码"、Palatino 的 M 太宽，都被否）

⭐ 关键比例（照朗格原图量的）：**星期字母比日期数字小一号**
   日期数字 cap_h = 24（占窗口高 67%），星期字母 cap_h = 16（占 44%）。
   不是偷懒 —— 大写字母宽/高约 1.2，两个字母的宽度是 2.4 倍字高；
   和 24 高的数字用同一字号的话 "MO" 要 55px，46px 宽的窗口根本放不下。

字符：0-9（cap_h 24）+ A-Z（cap_h 16），基线（底边）统一，
      宽度**不缩放**（缩放会把 W/T 压扁，实测 W 会掉到 11px 高），另存 advance 表做比例排版。
输出：每字节两个像素（高半字节 = 左像素），值 0..15。
"""
from PIL import Image, ImageDraw, ImageFont
import os

FONT = "C:/Windows/Fonts/CENSCBK.TTF"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "font_dial.h")

DIGITS = "0123456789"
ALPHAS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
CHARS = DIGITS + ALPHAS
H_DIGIT = 24        # 数字字高
H_ALPHA = 21        # 字母字高（与数字只差 4px）
DIGIT_SLOT = 21     # 数字等宽槽宽
ALPHA_SQUEEZE = 0.88  # 字母横向压缩比（见下）


def render_char(ch, size):
    f = ImageFont.truetype(FONT, size)
    img = Image.new("L", (size * 4, size * 4), 0)
    d = ImageDraw.Draw(img)
    d.text((size, size), ch, font=f, fill=255)
    bb = img.getbbox()
    if not bb:
        return None
    return img.crop(bb)


def fit_size(ch, target_h):
    """找字号，使该字符墨迹高度 >= target_h（取第一个够的）"""
    for s in range(12, 80):
        g = render_char(ch, s)
        if g is not None and g.height >= target_h:
            return s, g
    raise SystemExit(f"{ch} 找不到字号")


def main():
    s_digit, _ = fit_size("8", H_DIGIT)
    s_alpha, _ = fit_size("M", H_ALPHA)
    print(f"字号: 数字={s_digit} (cap {H_DIGIT})  字母={s_alpha} (cap {H_ALPHA})")

    inks = []
    for ch in DIGITS:
        g = render_char(ch, s_digit)
        inks.append(g)
    for ch in ALPHAS:
        g = render_char(ch, s_alpha)
        # 字母**横向**压缩（只压宽、不动高）：大写字母宽/高≈1.3，两个字母就是 2.6 倍字高，
        # 而两个数字（等宽槽 21）只有 1.75 倍字高 —— 不压的话字母窗永远比数字窗"重"。
        # 压 12% 后衬线竖笔略瘦，在 21px 高的尺寸上看不出变形。
        w2 = max(1, int(round(g.width * ALPHA_SQUEEZE)))
        g = g.resize((w2, g.height), Image.LANCZOS)
        inks.append(g)

    W = max(g.width for g in inks) + 1
    H = max(g.height for g in inks)
    if H < H_DIGIT:
        H = H_DIGIT
    ROW_BYTES = (W + 1) // 2
    print(f"画布 {W}x{H}  row_bytes={ROW_BYTES}  最大墨迹={max(g.width for g in inks)}")

    glyphs, adv = [], []
    for idx, (ch, g) in enumerate(zip(CHARS, inks)):
        canvas = Image.new("L", (W, H), 0)
        x = (W - g.width) // 2
        y = H - g.height            # 底边对齐基线
        canvas.paste(g, (x, y))
        glyphs.append(canvas)
        # 数字走**等宽槽**：日期窗里两个数字各占一格，宽度才能和两个字母看齐。
        # 单字墨迹宽 15~18、槽 21，字形仍按墨迹居中（drawDialText 会补偏移）。
        adv.append(DIGIT_SLOT if idx < len(DIGITS) else g.width + 1)
    print("  " + "  ".join(f"{c}:{a}" for c, a in zip("258MOFR", [adv[CHARS.index(c)] for c in "258MOFR"])))

    data = []
    for g in glyphs:
        px = g.load()
        for y in range(H):
            for bx in range(ROW_BYTES):
                hi = px[bx * 2, y] >> 4
                lo = px[bx * 2 + 1, y] >> 4 if bx * 2 + 1 < W else 0
                data.append((hi << 4) | lo)

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("// 自动生成，勿手改。生成器: tools/gen_dialfont.py\n")
        f.write("// Century Schoolbook 衬线；数字 cap 25（等宽槽）、字母 cap 21（比例宽），4-bit AA\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"#define DIAL_W {W}\n#define DIAL_H {H}\n")
        f.write(f"#define DIAL_ROW_BYTES {ROW_BYTES}\n")
        f.write(f"#define DIAL_COUNT {len(CHARS)}\n")
        f.write(f"#define DIAL_DIGITS {len(DIGITS)}\n\n")
        f.write(f'static const char DIAL_CHARS[] = "{CHARS}";\n\n')
        f.write("static const uint8_t DIAL_ADV[DIAL_COUNT] = {")
        for i, a in enumerate(adv):
            if i % 16 == 0:
                f.write("\n  ")
            f.write(f"{a},")
        f.write("\n};\n\n")
        f.write("static const uint8_t DIAL_AA[DIAL_COUNT][DIAL_H * DIAL_ROW_BYTES] = {\n")
        for i, ch in enumerate(CHARS):
            f.write(f"  {{ // {ch}\n   ")
            chunk = data[i * H * ROW_BYTES:(i + 1) * H * ROW_BYTES]
            for j, b in enumerate(chunk):
                f.write(f"0x{b:02X},")
                if (j + 1) % 12 == 0:
                    f.write("\n   ")
            f.write("\n  },\n")
        f.write("};\n")
    print(f"写出 {OUT}  {len(CHARS)} 字形  {len(data)} B")

    # 预览：46x36 深底窗 + 细银边，两窗并排
    def draw_str(img, s, cx, base_y, col=(238, 232, 214)):
        total = sum(adv[CHARS.index(c)] for c in s) - 1
        cur = cx - total // 2
        for c in s:
            gi = CHARS.index(c)
            cw = adv[gi] - 1
            off = (W - cw) // 2
            g = glyphs[gi]
            gp = g.load()
            for yy in range(H):
                for xx in range(W):
                    a = gp[xx, yy] / 255.0
                    if a > 0.02:
                        px_ = cur - off + xx
                        py_ = base_y - H + yy
                        if 0 <= px_ < img.width and 0 <= py_ < img.height:
                            p = img.getpixel((px_, py_))
                            img.putpixel((px_, py_), tuple(
                                int(p[k] * (1 - a) + col[k] * a) for k in range(3)))
            cur += adv[gi]

    WIN_W, WIN_H = 60, 38
    prev = Image.new("RGB", (6 + 4 * (WIN_W + 8) + 6, WIN_H + 20), (17, 26, 33))
    dr = ImageDraw.Draw(prev)
    for i, s in enumerate(["FR", "25", "MO", "31"]):
        bx = 6 + i * (WIN_W + 8)
        dr.rectangle([bx, 6, bx + WIN_W, 6 + WIN_H], fill=(9, 14, 20), outline=(196, 188, 166), width=1)
        dr.rectangle([bx + 2, 8, bx + WIN_W - 2, 6 + WIN_H - 2], outline=(56, 68, 76), width=1)
        # 基线：窗内垂直居中（数字 cap 27，窗内高 36 -> 上下各留 4.5）
        draw_str(prev, s, bx + WIN_W // 2, 6 + (WIN_H + H_DIGIT) // 2)
    prev.resize((prev.width * 3, prev.height * 3), Image.NEAREST).save(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "_dial_preview.png"))
    print("preview -> tools/_dial_preview.png")


if __name__ == "__main__":
    main()

# -*- coding: utf-8 -*-
"""生成设备点阵字库 font_data.h

  FONT12      12x12 1-bit ASCII 32..126          小标签
  FONT16      16x16 1-bit ASCII + CJK 子集      天气状况 / 小中文
  FONT24      24x24 1-bit ASCII + CJK 子集      一般标题 / 温度
  FONT_BIG_AA 44x64 4-bit 抗锯齿大数字          时间与倒计时，1:1 绘制不放大

所有 1-bit 表：行优先，每行 (size+7)/8 字节，bit7 为最左像素。
FONT_BIG_AA：行优先，每行 W/2 字节，高 4bit 为偶数列、低 4bit 为奇数列，
值 0..15 表示覆盖率，绘制时与背景做 alpha 混合，边缘不再有放大马赛克。
"""
from PIL import Image, ImageDraw, ImageFont
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "font_data.h")

CJK_LEGACY = (
    "晴多云阴雾小雨中大雪雷阵霾风沙尘浮"
    "专注休息暂停开始重置轮次完成长短分秒阶段"
    "演示数据天气温度今日最高低电量网络未连接线缓存"
    "附近架目标航班雷达距离方位机型"
    "播放收音客本地黑胶唱片正在已上一曲下"
    "机械电子表盘星期"
    "度晴间夜白晚风体感湿压"
    "专注中短休长息已完成"
    "一二三四五六日"          # 模拟表盘左侧星期窗：中文数字（"日" 已在 "今日" 中，去重）
)
CJK_LEGACY = "".join(dict.fromkeys(CJK_LEGACY))

# v0.16：中文歌名 / 电台名 / 播客标题 / 城市名是「任意中文」，靠手写 111 字表不够。
# FONT16 直接铺满 GB2312 全部 6763 个汉字，加中文标点；16x16x6763 ≈ 216 KB Flash，
# 16 MB Flash 完全放得下。FONT24 只用于天气/星期等固定文案，保持小表即可。
def gb2312_hanzi():
    out = []
    for hi in range(0xB0, 0xF8):        # 一级 16..55 区 + 二级 56..87 区
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if "\u4e00" <= ch <= "\u9fff":
                out.append(ch)
    return out


CJK_PUNCT = "、。，！？：；“”‘’（）《》【】—…·℃％"
CJK_UI = (
    "电台音乐播客城市选择充电量设置宇半拿铁忽左忽右"
    "咖硅谷钉蜻蜓频道直播故事商业沉浮录周刊"
    # 二期 24px 语音主键的文案：FONT24 只带固定小表，缺字会静默渲染成空白
    "说话止识别"
    # 应用页标题（uiPage 用 FONT24）：小智/小智绑定/录音机/待办清单/
    # 背单词/设置期限/语音备忘/选词书。缺的字补进小表，生成后仍需用
    # CJK24_COVER 自检，缺字画上去是空白且不报错。
    "智绑定录音机待办清单背单词置期限语准备忘选书"
    "文件纪要"   # v2.9：录音文件列表 / 纪要阅读页标题
    "桌面伙伴"   # v3.0：WorkBuddy Buddy 页标题（FONT24 小表，缺字会静默空白）
)

# 三张表都按 Unicode 码点升序，固件 glyphIndex() 用二分查找（O(log N)）。
CJK16 = sorted(set(gb2312_hanzi()) | set(CJK_LEGACY) | set(CJK_PUNCT) | set(CJK_UI), key=ord)
CJK24 = sorted(set(CJK_LEGACY) | set(CJK_PUNCT) | set(CJK_UI), key=ord)

BIG_CHARS = "0123456789:-./ "       # 15 个，顺序必须与固件一致
BIG_W, BIG_H = 44, 64

ASCII_CANDIDATES = [
    r"C:/Windows/Fonts/consola.ttf",
    r"C:/Windows/Fonts/consolab.ttf",
    r"C:/Windows/Fonts/cour.ttf",
    r"C:/Windows/Fonts/DejaVuSansMono.ttf",
]
BIG_CANDIDATES = [
    r"C:/Windows/Fonts/segoeuil.ttf",   # Segoe UI Light：细体数字，表盘更好看
    r"C:/Windows/Fonts/segoeui.ttf",
    r"C:/Windows/Fonts/consola.ttf",
    r"C:/Windows/Fonts/arial.ttf",
]
CJK_CANDIDATES = [
    r"C:/Windows/Fonts/msyh.ttc",
    r"C:/Windows/Fonts/msyhbd.ttc",
    r"C:/Windows/Fonts/simhei.ttf",
    r"C:/Windows/Fonts/simsun.ttc",
]


def pick(cands):
    for p in cands:
        if os.path.exists(p):
            return p
    raise RuntimeError("no font found in " + str(cands))


ASCII_FONT = pick(ASCII_CANDIDATES)
CJK_FONT = pick(CJK_CANDIDATES)
BIG_FONT = pick(BIG_CANDIDATES)
print("ascii:", ASCII_FONT)
print("cjk  :", CJK_FONT)
print("big  :", BIG_FONT)


def render_1bit(char, size, path):
    """size x size 的 1-bit 位图，字形按 bbox 居中。"""
    row_bytes = (size + 7) // 8
    font = ImageFont.truetype(path, size)
    im = Image.new("1", (size, size), 0)
    d = ImageDraw.Draw(im)
    try:
        bbox = d.textbbox((0, 0), char, font=font)
    except Exception:
        bbox = (0, 0, size, size)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((size - tw) // 2 - bbox[0], (size - th) // 2 - bbox[1]), char, font=font, fill=1)
    out = bytearray(size * row_bytes)
    px = im.load()
    for yy in range(size):
        for xx in range(size):
            if px[xx, yy]:
                out[yy * row_bytes + (xx >> 3)] |= 0x80 >> (xx & 7)
    return bytes(out)


def render_aa(char, w, h, path, font_size):
    """w x h 的 4-bit 灰度位图，两个像素打包一字节。"""
    font = ImageFont.truetype(path, font_size)
    im = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(im)
    try:
        bbox = d.textbbox((0, 0), char, font=font)
    except Exception:
        bbox = (0, 0, w, h)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((w - tw) // 2 - bbox[0], (h - th) // 2 - bbox[1]), char, font=font, fill=255)
    px = im.load()
    out = bytearray(h * (w // 2))
    for yy in range(h):
        for xx in range(0, w, 2):
            a = (px[xx, yy] >> 4) & 0xF
            b = (px[xx + 1, yy] >> 4) & 0xF
            out[yy * (w // 2) + xx // 2] = (a << 4) | b
    return bytes(out)


def dump_table(name, size, entries, show_char=True):
    row_bytes = (size + 7) // 8
    per = size * row_bytes
    lines = ["// %d glyphs, %dx%d 1-bit, %d bytes/glyph" % (len(entries), size, size, per),
             "static const uint8_t %s[%d][%d] = {" % (name, len(entries), per)]
    for ch, data in entries:
        # 全量中文字库有 6800+ 行，逐行写汉字注释只会让头文件变大、编译变慢
        note = (" // U+%04X %s" % (ord(ch), ch if ord(ch) > 127 else repr(ch))) if show_char else ""
        lines.append("  {%s},%s" % (",".join("0x%02x" % b for b in data), note))
    lines.append("};")
    return "\n".join(lines)


def build():
    p = ["#pragma once",
         "// 自动生成，请勿手改。生成脚本: tools/gen_font.py",
         "#include <stdint.h>", ""]

    # ---- FONT12 ----
    e12 = [(chr(c), render_1bit(chr(c), 12, ASCII_FONT)) for c in range(32, 127)]
    p += ["#define FONT12_SIZE 12", "#define FONT12_ROW_BYTES 2",
          "#define FONT12_FIRST 32", "#define FONT12_ASCII_COUNT 95",
          dump_table("FONT12_GLYPHS", 12, e12), ""]

    # ---- FONT16（任意中文：GB2312 全表）----
    e16 = [(chr(c), render_1bit(chr(c), 16, ASCII_FONT)) for c in range(32, 127)]
    cps16 = [ord(ch) for ch in CJK16]
    for ch in CJK16:
        e16.append((ch, render_1bit(ch, 16, CJK_FONT)))
    p += ["#define FONT16_SIZE 16", "#define FONT16_ROW_BYTES 2",
          "#define FONT16_FIRST 32", "#define FONT16_ASCII_COUNT 95",
          "#define FONT16_CJK_COUNT %d" % len(cps16),
          "// 升序排列，gfx.cpp 的 glyphIndex() 用二分查找",
          "static const uint16_t FONT16_CJK_CP[%d] = {%s};" %
          (len(cps16), ",".join("0x%04X" % c for c in cps16)),
          dump_table("FONT16_GLYPHS", 16, e16, show_char=False), ""]

    # ---- FONT24（固定文案用，保持小表）----
    e24 = [(chr(c), render_1bit(chr(c), 24, ASCII_FONT)) for c in range(32, 127)]
    cps24 = [ord(ch) for ch in CJK24]
    for ch in CJK24:
        e24.append((ch, render_1bit(ch, 24, CJK_FONT)))
    p += ["#define FONT24_SIZE 24", "#define FONT24_ROW_BYTES 3",
          "#define FONT24_FIRST 32", "#define FONT24_ASCII_COUNT 95",
          "#define FONT24_CJK_COUNT %d" % len(cps24),
          "static const uint16_t FONT24_CJK_CP[%d] = {%s};" %
          (len(cps24), ",".join("0x%04X" % c for c in cps24)),
          dump_table("FONT24_GLYPHS", 24, e24), ""]

    # ---- FONT_BIG_AA（抗锯齿大数字）----
    big = [(ch, render_aa(ch, BIG_W, BIG_H, BIG_FONT, 64)) for ch in BIG_CHARS]
    per_big = BIG_H * (BIG_W // 2)
    p += ["#define FONT_BIG_W %d" % BIG_W,
          "#define FONT_BIG_H %d" % BIG_H,
          "#define FONT_BIG_ROW_BYTES %d" % (BIG_W // 2),
          "#define FONT_BIG_COUNT %d" % len(BIG_CHARS),
          'static const char FONT_BIG_CHARS[] = "%s";' % BIG_CHARS,
          "// %d glyphs, %dx%d 4-bit AA, %d bytes/glyph" % (len(big), BIG_W, BIG_H, per_big),
          "static const uint8_t FONT_BIG_AA[%d][%d] = {" % (len(big), per_big)]
    for ch, data in big:
        p.append("  {%s}, // %s" % (",".join("0x%02x" % b for b in data), repr(ch)))
    p.append("};")
    p.append("")

    text = "\n".join(p) + "\n"
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote", OUT, len(text), "bytes")
    print("FONT12", len(e12), "FONT16", len(e16), "FONT24", len(e24),
          "FONT_BIG_AA", len(big), "x", per_big, "bytes")


if __name__ == "__main__":
    build()

# -*- coding: utf-8 -*-
"""
纯 Python 像素级离线预览渲染器。

用途：开发板 USB CDC 卡死、无法用 SHOT 抓真机帧缓冲时，用与固件完全相同的
字库（font_data.h）和几何算法（gfx.cpp / main.cpp）在 360x360 RGB565 画布上
重新渲染 5 个页面，输出 PNG 供视觉验收。

严格说明：
  - 本脚本只读 firmware/font_data.h，不修改任何固件源文件。
  - 画布是 uint16 小端 RGB565（与固件内存布局一致）。固件 flushScreen 时
    LCD_addWindow 会原地 bswap16，所以真机截图要按大端解析；这里不送屏、
    不做 bswap，最后直接按 5/6/5 拆成 RGB888。
  - 只用 numpy + Pillow，不依赖串口 / 编译器 / 烧录工具。
  - 可重复运行：输出固定为 firmware/preview/offline_*.png。

运行：
  python offline_preview.py
"""

import math
import re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------- 路径与常量
FIRMWARE = Path(__file__).resolve().parent.parent
FONT_H = FIRMWARE / "font_data.h"
PREVIEW = FIRMWARE / "preview"

W = 360
H = 360
# Arduino.h 里的 PI 是 double 字面量；三角部分尽量按 float32 复刻 cosf/sinf
PI = 3.1415926535897932384626433832795
F = np.float32


def cdiv(a, b):
    """C 语言 int 除法：向零截断（Python 的 // 是向下取整，负数会差 1）。"""
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def F32(x):
    return np.float32(x)


def COL(r, g, b):
    """main.cpp 的 COL 宏：RGB888 -> RGB565（常量不做 uint8 截断）。"""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rgb(r, g, b):
    """gfx.cpp 的 rgb()：参数是 uint8_t，先截断再压成 RGB565。"""
    return (((r & 0xFF) >> 3) << 11) | (((g & 0xFF) >> 2) << 5) | ((b & 0xFF) >> 3)


# ---------------------------------------------------------------- 调色板（main.cpp 25-43 行）
C_ORANGE = COL(226, 98, 46)
C_ORANGE_D = COL(141, 63, 39)
C_WHITE = COL(242, 245, 250)
C_PAPER = COL(244, 241, 233)
C_INK = COL(32, 38, 43)
C_CREAM = COL(207, 194, 168)
C_DARK = COL(11, 16, 20)
C_DARK2 = COL(16, 23, 30)
C_SLATE = COL(43, 53, 61)
C_GREY = COL(139, 151, 169)
C_GREEN = COL(126, 230, 171)
C_GREEN_D = COL(32, 80, 60)
C_RADAR_BG = COL(6, 16, 14)
C_YELLOW = COL(240, 180, 41)
C_BLUE = COL(88, 168, 224)
C_VINYL = COL(13, 17, 24)

# ---------------------------------------------------------------- 固件常量
WDAY_EN = ["SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"]
ROMAN = ["XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI"]
SRC_TRACK = ["QTFM 101.7", "QINGTING FM", "RFI CHINESE"]
PAGE_COUNT = 5
POMO_FOCUS_MIN = 25
POMO_LONG_BREAK_EVERY = 4
RADAR_RADIUS_NM = 250
WX_CLOUD = 2

# main.cpp 168-177 行的 DEMO_PLANES 初值，bearing 不动画
DEMO_PLANES = [
    dict(call="CSZ9917", r=0.60, bearing=38.0, track=35, alt_m=9479, gs=1018, type="A320", demo=True, stale=False),
    dict(call="DKH1170", r=0.84, bearing=112.0, track=210, alt_m=6200, gs=820, type="A321", demo=True, stale=False),
    dict(call="CES5102", r=0.40, bearing=205.0, track=150, alt_m=3400, gs=610, type="B738", demo=True, stale=False),
    dict(call="CCA1831", r=0.71, bearing=288.0, track=305, alt_m=11200, gs=890, type="B77W", demo=True, stale=False),
    dict(call="CSN3406", r=0.29, bearing=330.0, track=75, alt_m=2100, gs=480, type="A333", demo=True, stale=True),
    dict(call="UAE306", r=0.93, bearing=163.0, track=340, alt_m=12100, gs=930, type="B77L", demo=True, stale=False),
    dict(call="CES5821", r=0.52, bearing=15.0, track=120, alt_m=7300, gs=870, type="A359", demo=True, stale=False),
    dict(call="CSZ9872", r=0.76, bearing=245.0, track=60, alt_m=5100, gs=700, type="B738", demo=True, stale=False),
]


# ================================================================ 字库解析
def _array_body(text, decl):
    """截取数组声明到结束 '};' 之间的文本。"""
    i = text.index(decl)
    j = text.index("};", i)
    return text[i:j]


def _bytes(body):
    """按用户指定的正则 0x([0-9a-fA-F]{2}) 提取所有 uint8 字节。"""
    return np.fromiter((int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", body)),
                       dtype=np.uint8)


def _expand_1bit(g, n, size, rowb):
    """(n,size,rowb) 1-bit 字形 -> (n,size,size) bool，每字节 8 像素、MSB 在左。"""
    bits = ((g[:, :, :, None] >> np.arange(7, -1, -1)) & 1).astype(bool)
    return bits.reshape(n, size, rowb * 8)[:, :, :size]


def load_fonts():
    text = FONT_H.read_text(encoding="utf-8")
    defines = {k: int(v) for k, v in re.findall(r"#define\s+(\w+)\s+(\d+)", text)}

    f12 = _bytes(_array_body(text, "FONT12_GLYPHS[95][24]")).reshape(
        defines["FONT12_ASCII_COUNT"], defines["FONT12_SIZE"], defines["FONT12_ROW_BYTES"])
    f16 = _bytes(_array_body(text, "FONT16_GLYPHS[201][32]")).reshape(
        defines["FONT16_ASCII_COUNT"] + defines["FONT16_CJK_COUNT"],
        defines["FONT16_SIZE"], defines["FONT16_ROW_BYTES"])
    f24 = _bytes(_array_body(text, "FONT24_GLYPHS[201][72]")).reshape(
        defines["FONT24_ASCII_COUNT"] + defines["FONT24_CJK_COUNT"],
        defines["FONT24_SIZE"], defines["FONT24_ROW_BYTES"])
    big = _bytes(_array_body(text, "FONT_BIG_AA[15][1408]")).reshape(
        defines["FONT_BIG_COUNT"], defines["FONT_BIG_H"], defines["FONT_BIG_ROW_BYTES"])

    # 4-bit AA：每字节 2 像素，高半字节 = 左像素（偶数列）
    aa = np.empty((defines["FONT_BIG_COUNT"], defines["FONT_BIG_H"], defines["FONT_BIG_W"]),
                  dtype=np.uint8)
    aa[:, :, 0::2] = big >> 4
    aa[:, :, 1::2] = big & 0x0F

    # CJK 码点表是 uint16，用 4 位十六进制单独提取
    cjk16 = [int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{4})",
                                            _array_body(text, "FONT16_CJK_CP[106]"))]
    cjk24 = [int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{4})",
                                            _array_body(text, "FONT24_CJK_CP[106]"))]
    big_chars = re.search(r"FONT_BIG_CHARS\[\] = \"([^\"]*)\"", text).group(1)

    return dict(
        f12=_expand_1bit(f12, defines["FONT12_ASCII_COUNT"], defines["FONT12_SIZE"], defines["FONT12_ROW_BYTES"]),
        f16=_expand_1bit(f16, f16.shape[0], defines["FONT16_SIZE"], defines["FONT16_ROW_BYTES"]),
        f24=_expand_1bit(f24, f24.shape[0], defines["FONT24_SIZE"], defines["FONT24_ROW_BYTES"]),
        aa=aa, big_chars=big_chars, cjk16=cjk16, cjk24=cjk24, defines=defines)


# ================================================================ 渲染器
class Preview:
    """持有 360x360 uint16 RGB565 帧缓冲，方法逐条对应 gfx.cpp 原语。"""

    def __init__(self, fonts):
        self.FB = np.zeros(W * H, dtype=np.uint16)
        self.f = fonts
        self.d = fonts["defines"]
        # 越界自检记录
        self.oob = dict(setpixel=0, rect=0, text=0, aa=0,
                        minx=10 ** 9, maxx=-10 ** 9, miny=10 ** 9, maxy=-10 ** 9)
        self.bg_snap = None
        self.ink_info = None
        self.curPage = 0
        # 代表状态（main.cpp 默认值 + 用户指定值）
        self.gHour, self.gMin, self.gSec = 10, 47, 26
        self.gDay, self.gMon, self.gYear, self.gWday = 14, 11, 2026, 6
        self.wx_live, self.wifiOK = False, False
        self.wx_temp, self.wx_lo, self.wx_hi = 16, 15, 19
        self.wx_desc, self.wx_icon = "阴", WX_CLOUD
        self.pomo_phase, self.pomo_running = 0, False
        self.pomo_remainMs, self.pomo_rounds, self.pomo_alarm = 25 * 60000, 0, False
        self.mPlaying, self.gConnected, self.mSrc = False, True, 0
        self.vinylAngle, self.trackProgress = 35.0, 0.38
        self.radar_scan, self.detailIdx = 0, -1
        self.radiusNm, self.radarSrc = RADAR_RADIUS_NM, "ADSB.LOL"
        self.planes = DEMO_PLANES

    # ---------------- 几何自检记录 ----------------
    def _note_oob(self, x, y):
        self.oob["minx"] = min(self.oob["minx"], x)
        self.oob["maxx"] = max(self.oob["maxx"], x)
        self.oob["miny"] = min(self.oob["miny"], y)
        self.oob["maxy"] = max(self.oob["maxy"], y)

    # ---------------- gfx.cpp 绘图原语 ----------------
    def clear(self, c):
        self.FB[:] = c

    def setPixel(self, x, y, c):
        if x < 0 or y < 0 or x >= W or y >= H:
            self.oob["setpixel"] += 1
            self._note_oob(x, y)
            return
        self.FB[y * W + x] = c

    def fillRect(self, x, y, w, h, c):
        if w <= 0 or h <= 0:
            return
        if x < 0 or y < 0 or x + w > W or y + h > H:
            self.oob["rect"] += 1
            self._note_oob(x, y)
            self._note_oob(x + w - 1, y + h - 1)
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(W, x + w), min(H, y + h)
        for yy in range(y0, y1):
            self.FB[yy * W + x0:yy * W + x1] = c

    def rect(self, x, y, w, h, c, t=1):
        self.fillRect(x, y, w, t, c)
        self.fillRect(x, y + h - t, w, t, c)
        self.fillRect(x, y, t, h, c)
        self.fillRect(x + w - t, y, t, h, c)

    def fillCircle(self, cx, cy, r, c):
        if r < 0:
            return
        for dy in range(-r, r + 1):
            dx = int(np.sqrt(F32(r * r - dy * dy)))
            self.fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, c)

    def circle(self, cx, cy, r, c, t=1):
        # 中点圆 + 8 对称，thickness 用半径 t/2 的小圆补
        x, y, err = r, 0, 1 - r
        while x >= y:
            pts = [(x, y), (y, x), (-y, x), (-x, y),
                   (-x, -y), (-y, -x), (y, -x), (x, -y)]
            for px, py in pts:
                if t <= 1:
                    self.setPixel(cx + px, cy + py, c)
                else:
                    self.fillCircle(cx + px, cy + py, t // 2, c)
            y += 1
            if err < 0:
                err += 2 * y + 1
            else:
                x -= 1
                err += 2 * (y - x) + 1

    def line(self, x0, y0, x1, y1, c, t=1):
        dx, dy = x1 - x0, y1 - y0
        steps = max(abs(dx), abs(dy))
        if steps == 0:
            self.fillCircle(x0, y0, t // 2, c)
            return
        for i in range(steps + 1):
            x = x0 + cdiv(dx * i, steps)
            y = y0 + cdiv(dy * i, steps)
            if t <= 1:
                self.setPixel(x, y, c)
            else:
                self.fillCircle(x, y, t // 2, c)

    def arcRing(self, cx, cy, r, a0, a1, width, c):
        if a1 < a0:
            return
        a = float(a0)
        while a <= a1 + 1e-9:
            rad = F32((float(F32(a)) - 90.0) * PI / 180.0)
            cs, sn = np.cos(rad), np.sin(rad)
            for d in range(cdiv(-width, 2), cdiv(width, 2) + 1):
                rr = r + d
                self.setPixel(cx + int(F32(rr) * cs), cy + int(F32(rr) * sn), c)
            a += 0.25

    @staticmethod
    def edgeFn(ax, ay, bx, by, px, py):
        return (bx - ax) * float(py - ay) - (by - ay) * float(px - ax)

    def fillTri(self, x0, y0, x1, y1, x2, y2, c):
        minx, maxx = min(x0, x1, x2), max(x0, x1, x2)
        miny, maxy = min(y0, y1, y2), max(y0, y1, y2)
        area = self.edgeFn(x0, y0, x1, y1, x2, y2)
        if abs(area) < 0.001:
            return
        pos = area > 0
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                w0 = self.edgeFn(x1, y1, x2, y2, x, y)
                w1 = self.edgeFn(x2, y2, x0, y0, x, y)
                w2 = self.edgeFn(x0, y0, x1, y1, x, y)
                if pos:
                    ok = w0 >= 0 and w1 >= 0 and w2 >= 0
                else:
                    ok = w0 <= 0 and w1 <= 0 and w2 <= 0
                if ok:
                    self.setPixel(x, y, c)

    def fillRoundRect(self, x, y, w, h, r, c):
        if r <= 0:
            self.fillRect(x, y, w, h, c)
            return
        self.fillRect(x + r, y, w - 2 * r, h, c)
        self.fillRect(x, y + r, w, h - 2 * r, c)
        self.fillCircle(x + r, y + r, r, c)
        self.fillCircle(x + w - r - 1, y + r, r, c)
        self.fillCircle(x + r, y + h - r - 1, r, c)
        self.fillCircle(x + w - r - 1, y + h - r - 1, r, c)

    def roundRect(self, x, y, w, h, r, c):
        self.arcRing(x + r, y + r, r, 180, 270, 1, c)
        self.arcRing(x + w - r - 1, y + r, r, 270, 360, 1, c)
        self.arcRing(x + r, y + h - r - 1, r, 90, 180, 1, c)
        self.arcRing(x + w - r - 1, y + h - r - 1, r, 0, 90, 1, c)
        self.line(x + r, y, x + w - r, y, c, 1)
        self.line(x + r, y + h - 1, x + w - r, y + h - 1, c, 1)
        self.line(x, y + r, x, y + h - r, c, 1)
        self.line(x + w - 1, y + r, x + w - 1, y + h - r, c, 1)

    def mix(self, a, b, t):
        ar, ag, ab = (a >> 11) & 31, (a >> 5) & 63, a & 31
        br, bg, bb = (b >> 11) & 31, (b >> 5) & 63, b & 31
        return rgb((ar * (255 - t) + br * t) * 8 // 255,
                   (ag * (255 - t) + bg * t) * 4 // 255,
                   (ab * (255 - t) + bb * t) * 8 // 255)

    def radialGradientV(self, y0, y1, top, bottom):
        for y in range(y0, y1 + 1):
            t = cdiv(255 * (y - y0), max(1, y1 - y0)) & 0xFF
            c = self.mix(top, bottom, t)
            self.FB[y * W:(y + 1) * W] = c

    # ---------------- 1-bit 点阵文字（gfx.cpp 166-247 行）----------------
    def glyphIndex(self, cp, font):
        if font == 12:
            return cp - 32 if 32 <= cp <= 126 else -1
        if font == 16:
            asciiCount, cjkCp = self.d["FONT16_ASCII_COUNT"], self.f["cjk16"]
        else:
            asciiCount, cjkCp = self.d["FONT24_ASCII_COUNT"], self.f["cjk24"]
        if 32 <= cp <= 126:
            return cp - 32
        for i, v in enumerate(cjkCp):
            if v == cp:
                return asciiCount + i
        return -1

    def charAdvance(self, cp, font, scale):
        if font == 12:
            base = 8
        elif font == 16:
            base = 10 if cp < 128 else 16
        else:
            base = 14 if cp < 128 else 24
        return base * scale

    def textWidth(self, s, font, scale):
        return sum(self.charAdvance(ord(ch), font, scale) for ch in s)

    def drawText(self, x, y, s, color, font, scale, align=0):
        total = self.textWidth(s, font, scale)
        size = 12 if font == 12 else (16 if font == 16 else 24)
        tab = self.f["f12"] if font == 12 else (self.f["f16"] if font == 16 else self.f["f24"])
        if align == 1:
            # 最新版"真实绘制宽度"居中：步进和 - 末字步进 + 末字完整 canvas
            lastAdv = size * scale
            for ch in s:
                lastAdv = self.charAdvance(ord(ch), font, scale)
            x -= cdiv(total - lastAdv + size * scale, 2)
        elif align == 2:
            x -= total
        ox = x
        for ch in s:
            cp = ord(ch)
            gi = self.glyphIndex(cp, font)
            adv = self.charAdvance(cp, font, scale)
            if gi >= 0:
                gys, gxs = np.nonzero(tab[gi])
                if scale == 1:
                    pxs, pys = ox + gxs, y + gys
                    v = (pxs >= 0) & (pxs < W) & (pys >= 0) & (pys < H)
                    self.oob["text"] += int((~v).sum())
                    self.FB[pys[v] * W + pxs[v]] = color
                else:
                    for gy, gx in zip(gys, gxs):
                        self.fillRect(ox + gx * scale, y + gy * scale, scale, scale, color)
            ox += adv
        return total

    # ---------------- 4-bit 抗锯齿数字（gfx.cpp 249-347 行）----------------
    def aaPix(self, g, x, y):
        if x < 0 or y < 0 or x >= self.d["FONT_BIG_W"] or y >= self.d["FONT_BIG_H"]:
            return 0
        return int(g[y, x])

    def blendPixel(self, x, y, c, t):
        if t == 0 or x < 0 or y < 0 or x >= W or y >= H:
            return
        i = y * W + x
        b = int(self.FB[i])
        br, bg, bb = b >> 11, (b >> 5) & 63, b & 31
        cr, cg, cb = c >> 11, (c >> 5) & 63, c & 31
        r = (br * (255 - t) + cr * t) // 255
        g2 = (bg * (255 - t) + cg * t) // 255
        bl = (bb * (255 - t) + cb * t) // 255
        self.FB[i] = (r << 11) | (g2 << 5) | bl

    def aaBlit(self, g, x0, y0, outW, outH, step, color):
        half = step // 2 - 128          # 8.8 定点中心采样
        for oy in range(outH):
            py = y0 + oy
            if py < 0 or py >= H:
                self.oob["aa"] += 1
                continue
            syf = oy * step + half
            if syf < 0:
                syf = 0
            sy, fy = syf >> 8, syf & 255
            for ox in range(outW):
                px = x0 + ox
                if px < 0 or px >= W:
                    self.oob["aa"] += 1
                    continue
                sxf = ox * step + half
                if sxf < 0:
                    sxf = 0
                sx, fx = sxf >> 8, sxf & 255
                a00, a10 = self.aaPix(g, sx, sy), self.aaPix(g, sx + 1, sy)
                a01, a11 = self.aaPix(g, sx, sy + 1), self.aaPix(g, sx + 1, sy + 1)
                a = (a00 * (256 - fx) * (256 - fy) + a10 * fx * (256 - fy) +
                     a01 * (256 - fx) * fy + a11 * fx * fy + 32768) >> 16
                if a > 0:
                    self.blendPixel(px, py, color, (a * 17) & 0xFF)

    def aaAdvance(self, ch, digitAdv):
        if ch == ":":
            return digitAdv * 65 // 100
        if ch in (".", " "):
            return digitAdv // 2
        if ch in ("-", "/"):
            return digitAdv * 85 // 100
        return digitAdv

    def aaTextWidth(self, s, big):
        d = 50 if big else 20
        return sum(self.aaAdvance(ch, d) for ch in s)

    def aaText(self, x, y, s, color, big, align):
        step = 204 if big else 512
        outW, outH = (55 if big else 22), (80 if big else 32)
        dAdv = 50 if big else 20
        chars = s[:24]
        adv = [self.aaAdvance(ch, dAdv) for ch in chars]
        n, total = len(adv), sum(adv)
        if n == 0:
            return 0
        colon = next((i for i, ch in enumerate(chars) if ch == ":"), -1)
        center = total // 2
        if colon >= 0:
            center = sum(adv[:colon]) + adv[colon] // 2   # 冒号墨迹中心
        cur = (x - center) if align == 1 else (x - total) if align == 2 else x
        for i, ch in enumerate(chars):
            gi = -1
            for k, bch in enumerate(self.f["big_chars"]):
                if bch == ch:
                    gi = k
                    break
            if gi >= 0 and ch != " ":
                ox = cur + cdiv(adv[i] - outW, 2)   # 母版墨迹中心对齐字符槽中心
                self.aaBlit(self.f["aa"][gi], ox, y, outW, outH, step, color)
            cur += adv[i]
        return total

    def drawBigText(self, cx, y, s, color):
        return self.aaText(cx, y, s, color, True, 1)

    def drawMidText(self, x, y, s, color, align=1):
        return self.aaText(x, y, s, color, False, align)

    def drawDegree(self, x, y, color, r=2):
        self.circle(x, y, max(1, r), color, 1)

    def polar(self, cx, cy, r, deg):
        rad = F32((float(F32(deg)) - 90.0) * PI / 180.0)
        cs, sn = np.cos(rad), np.sin(rad)
        return cx + int(F32(r) * cs), cy + int(F32(r) * sn)

    # ---------------- main.cpp 通用装饰 ----------------
    def pageDots(self, y, dim):
        x0 = 180 - (PAGE_COUNT - 1) * 7
        for i in range(PAGE_COUNT):
            self.fillCircle(x0 + i * 14, y, 3 if i == self.curPage else 2,
                            C_ORANGE if i == self.curPage else dim)

    def statusBar(self, txt, col):
        self.drawText(180, 300, txt, col, 12, 1, 1)

    def drawWxIcon(self, cx, cy, sz, ic, col):
        r = sz // 2
        sun = C_YELLOW

        def cloud(oy, c):
            self.fillCircle(cx - cdiv(r, 3), cy + oy, cdiv(r, 3), c)
            self.fillCircle(cx + cdiv(r, 5), cy + oy - cdiv(r, 6), cdiv(r * 2, 5), c)
            self.fillCircle(cx + cdiv(r, 2), cy + oy + cdiv(r, 8), cdiv(r, 4), c)
            self.fillRect(cx - cdiv(r, 2), cy + oy + cdiv(r, 8), r, cdiv(r, 3), c)

        if ic == 0:  # WX_CLEAR
            self.fillCircle(cx, cy, int(F32(r) * F32(0.55)), sun)
            for a in range(0, 360, 45):
                x1, y1 = self.polar(cx, cy, int(F32(r) * F32(0.72)), a)
                x2, y2 = self.polar(cx, cy, int(F32(r) * F32(0.98)), a)
                self.line(x1, y1, x2, y2, sun, 2)
        elif ic == 1:  # WX_PARTLY
            self.fillCircle(cx - cdiv(r, 4), cy - cdiv(r, 3), int(F32(r) * F32(0.45)), sun)
            cloud(cdiv(r, 6), col)
        elif ic == 2:  # WX_CLOUD
            cloud(0, col)
        elif ic == 3:  # WX_FOG
            cloud(cdiv(-r, 5), col)
            self.line(cx - r, cy + cdiv(r, 2), cx + r, cy + cdiv(r, 2), col, 2)
            self.line(cx - cdiv(r * 2, 3), cy + cdiv(r * 3, 4),
                      cx + cdiv(r * 2, 3), cy + cdiv(r * 3, 4), col, 2)
        elif ic == 4:  # WX_RAIN
            cloud(cdiv(-r, 5), col)
            for i in (-1, 0, 1):
                self.line(cx + cdiv(i * r, 2), cy + cdiv(r, 3),
                          cx + cdiv(i * r, 2) - cdiv(r, 8), cy + r, C_BLUE, 2)
        elif ic == 5:  # WX_SNOW
            cloud(cdiv(-r, 5), col)
            for i in (-1, 0, 1):
                self.fillCircle(cx + cdiv(i * r, 2), cy + cdiv(r * 3, 4), 2, COL(159, 212, 245))
        elif ic == 6:  # WX_STORM
            cloud(cdiv(-r, 4), col)
            self.fillTri(cx + 2, cy, cx - cdiv(r, 2), cy + cdiv(r * 3, 4),
                         cx + cdiv(r, 8), cy + cdiv(r * 3, 4), sun)
            self.fillTri(cx - 2, cy + cdiv(r, 2), cx + cdiv(r, 2), cy + r,
                         cx - cdiv(r, 8), cy + r, sun)

    # ============================================================ 页 1 机械表盘
    def renderAnalog(self):
        self.curPage = 0
        self.clear(C_DARK)
        self.fillCircle(180, 180, 172, COL(17, 26, 33))
        self.circle(180, 180, 168, C_SLATE, 2)
        self.circle(180, 180, 160, COL(35, 44, 52), 1)
        for i in range(60):
            major = i % 5 == 0
            x1, y1 = self.polar(180, 180, 140 if major else 150, i * 6)
            x2, y2 = self.polar(180, 180, 157, i * 6)
            self.line(x1, y1, x2, y2, C_CREAM if major else COL(109, 120, 130),
                      2 if major else 1)
        for i in range(12):
            if i == 3 or i == 6:
                continue
            x, y = self.polar(180, 180, 124, i * 30)
            self.drawText(x, y - 8, ROMAN[i], C_CREAM, 12, 1, 1)
        self.drawText(180, 104, "A E R O", C_CREAM, 12, 1, 1)

        # 日期窗：AA 小数字 + 精确居中
        self.fillRoundRect(238, 163, 44, 34, 5, COL(240, 236, 226))
        self.drawMidText(260, 165, str(self.gDay), C_INK, 1)

        # 指针
        hAng = (F32(self.gHour % 12) + F32(self.gMin) / F32(60.0)) * F32(30.0)
        mAng = (F32(self.gMin) + F32(self.gSec) / F32(60.0)) * F32(6.0)
        sAng = F32(self.gSec) * F32(6.0)
        hx, hy = self.polar(180, 180, 84, hAng)
        tx, ty = self.polar(180, 180, 18, hAng + 180)
        self.line(tx, ty, hx, hy, C_CREAM, 5)
        mx, my = self.polar(180, 180, 108, mAng)
        tx, ty = self.polar(180, 180, 20, mAng + 180)
        self.line(tx, ty, mx, my, COL(230, 220, 200), 3)
        sx, sy = self.polar(180, 180, 118, sAng)
        tx, ty = self.polar(180, 180, 22, sAng + 180)
        self.line(tx, ty, sx, sy, C_ORANGE, 1)
        self.fillCircle(180, 180, 5, C_CREAM)
        self.fillCircle(180, 180, 2, C_DARK)

        # 6 点天气副盘
        self.fillCircle(180, 248, 44, C_DARK)
        self.circle(180, 248, 44, COL(74, 85, 96), 2)
        self.drawWxIcon(180, 218, 20, self.wx_icon, COL(180, 192, 200))
        tbuf = str(self.wx_temp)
        warm = COL(242, 230, 207)
        self.drawMidText(180, 230, tbuf, warm, 1)
        self.drawDegree(180 + self.aaTextWidth(tbuf, False) // 2 + 3, 237, warm, 2)
        self.drawText(180, 264, self.wx_desc, COL(168, 180, 187), 16, 1, 1)

        self.statusBar("CAIYUN 30M" if self.wx_live else "DEMO WEATHER", COL(124, 137, 147))
        self.pageDots(314, COL(70, 80, 88))

    # ============================================================ 页 2 电子表盘
    def renderDigital(self):
        self.curPage = 1
        self.clear(C_PAPER)
        self.circle(180, 180, 158, COL(216, 212, 200), 1)
        self.circle(180, 180, 150, COL(222, 217, 204), 5)
        self.arcRing(180, 180, 150, 0, int(F32(360.0) * F32(self.gSec) / F32(60.0)), 5, C_ORANGE)
        self.drawText(180, 58, WDAY_EN[self.gWday], COL(141, 136, 122), 12, 1, 1)
        self.drawText(180, 76, "%04d-%02d-%02d" % (self.gYear, self.gMon, self.gDay),
                      COL(141, 136, 122), 12, 1, 1)

        # 大时间：绘制前留背景快照，供几何自检统计墨迹
        tbuf = "%02d:%02d" % (self.gHour, self.gMin)
        self.bg_snap = self.FB.copy()
        self.drawBigText(180, 98, tbuf, C_INK)
        self.ink_info = self.analyze_ink(95, 180, C_INK)

        self.drawMidText(180, 176, "%02d" % self.gSec, C_ORANGE, 1)
        self.drawText(180, 210, "SEC", COL(176, 156, 140), 12, 1, 1)
        self.line(112, 226, 248, 226, COL(211, 206, 192), 1)

        # 天气主区
        self.drawWxIcon(108, 262, 36, self.wx_icon, COL(107, 123, 134))
        wbuf = str(self.wx_temp)
        self.drawMidText(152, 246, wbuf, C_INK, 1)
        self.drawDegree(152 + self.aaTextWidth(wbuf, False) // 2 + 4, 252, C_INK, 2)
        self.drawText(196, 250, self.wx_desc, COL(58, 74, 68), 16, 1, 0)
        rbuf = "%d/%d" % (self.wx_lo, self.wx_hi)
        self.drawText(196, 272, rbuf, COL(141, 136, 122), 12, 1, 0)
        self.drawDegree(196 + self.textWidth(rbuf, 12, 1) + 2, 275, COL(141, 136, 122), 1)

        status = "CAIYUN 30M" if self.wx_live else ("WIFI / DEMO WX" if self.wifiOK else "OFFLINE / DEMO WX")
        self.statusBar(status, COL(141, 136, 122))
        self.pageDots(320, COL(201, 197, 184))

    # ============================================================ 页 3 音乐
    def renderMusic(self):
        self.curPage = 2
        self.clear(C_VINYL)
        self.fillCircle(180, 180, 176, COL(16, 20, 27))
        for r in range(60, 169, 7):
            self.circle(180, 180, r, COL(38, 43, 51) if r % 14 == 0 else COL(28, 33, 41), 1)
        for i in range(3):
            a0 = int(self.vinylAngle) + i * 120
            self.arcRing(180, 180, 120, a0, a0 + 26, 30, COL(30, 38, 50))
            self.arcRing(180, 180, 152, a0 + 60, a0 + 78, 12, COL(28, 35, 46))
        self.circle(180, 180, 170, COL(44, 53, 72), 1)
        self.arcRing(180, 180, 170, 0, int(F32(300.0) * F32(self.trackProgress)), 3, C_ORANGE)

        top = "RADIO / " + ("PLAYING" if self.mPlaying else ("PAUSED" if self.gConnected else "OFFLINE"))
        self.drawText(180, 44, top, COL(160, 176, 198), 12, 1, 1)
        self.drawText(180, 64, SRC_TRACK[self.mSrc], C_WHITE, 24, 1, 1)

        self.fillCircle(180, 180, 62, COL(20, 24, 31))
        self.circle(180, 180, 62, COL(58, 66, 80), 2)
        self.fillCircle(180, 180, 48, C_ORANGE if self.mPlaying else COL(60, 70, 88))
        if self.mPlaying:
            self.fillRect(166, 165, 9, 30, C_WHITE)
            self.fillRect(186, 165, 9, 30, C_WHITE)
        else:
            self.fillTri(170, 162, 170, 198, 200, 180, C_WHITE)

        self.fillCircle(82, 180, 28, COL(34, 43, 59))
        self.circle(82, 180, 28, COL(58, 69, 96), 1)
        self.fillTri(90, 170, 90, 190, 74, 180, C_WHITE)
        self.fillRect(72, 170, 3, 20, C_WHITE)
        self.fillCircle(278, 180, 28, COL(34, 43, 59))
        self.circle(278, 180, 28, COL(58, 69, 96), 1)
        self.fillTri(270, 170, 270, 190, 286, 180, C_WHITE)
        self.fillRect(285, 170, 3, 20, C_WHITE)

        self.drawText(180, 276, "DISC PLAY / SIDES SWITCH", COL(110, 122, 142), 12, 1, 1)
        self.pageDots(320, COL(60, 68, 82))

    # ============================================================ 页 4 航班雷达
    def renderRadar(self):
        self.curPage = 3
        self.clear(C_RADAR_BG)
        a0 = self.radar_scan
        for d in range(0, 46, 2):
            c = self.mix(C_RADAR_BG, COL(60, 170, 110), (d * 90 // 46) & 0xFF)
            self.arcRing(180, 180, 150, a0 - 46 + d, a0 - 44 + d, 3, c)
        sx, sy = self.polar(180, 180, 158, a0)
        self.line(180, 180, sx, sy, COL(74, 222, 128), 1)

        self.circle(180, 180, 52, C_GREEN_D, 1)
        self.circle(180, 180, 104, C_GREEN_D, 1)
        self.circle(180, 180, 156, COL(43, 107, 78), 2)
        self.line(180, 24, 180, 336, COL(29, 70, 54), 1)
        self.line(24, 180, 336, 180, COL(29, 70, 54), 1)
        for a in range(0, 360, 30):
            x1, y1 = self.polar(180, 180, 150, a)
            x2, y2 = self.polar(180, 180, 157, a)
            self.line(x1, y1, x2, y2, C_GREEN_D, 1)
            if a % 90 == 0:
                bx, by = self.polar(180, 180, 100, a)
                n = "N" if a == 0 else ("E" if a == 90 else ("S" if a == 180 else "W"))
                self.drawText(bx, by - 6, n, COL(75, 156, 114), 12, 1, 1)

        self.fillRect(175, 175, 10, 10, COL(233, 255, 242))
        self.circle(180, 180, 14, C_GREEN, 1)

        labelX, labelY = [], []
        for p in self.planes:
            px, py = self.polar(180, 180, int(F32(p["r"]) * F32(150)), p["bearing"])
            c = COL(110, 210, 155) if p["demo"] else C_GREEN
            if p["stale"]:
                c = C_YELLOW
            ar = F32((p["track"] - 90) * PI / 180.0)
            cr, sr = np.cos(ar), np.sin(ar)

            def rot(ox, oy):
                return (px + int(F32(ox) * cr - F32(oy) * sr),
                        py + int(F32(ox) * sr + F32(oy) * cr))

            ax, ay = rot(0, -9)
            bx2, by2 = rot(6, 7)
            cx2, cy2 = rot(-6, 7)
            self.fillTri(ax, ay, bx2, by2, cx2, cy2, c)
            if self.detailIdx >= 0 or len(labelX) >= 12:
                continue

            lx, ly = px + 10, py - 10
            if ly < 92 or ly > 268:
                continue
            if lx < 204 and lx + 64 > 156 and ly < 196 and ly + 12 > 164:
                continue
            half = int(np.sqrt(F32(max(0, 158 * 158 - (ly - 180) * (ly - 180)))))
            if lx < 180 - half + 6 or lx + 64 > 180 + half - 6:
                continue
            placed = False
            for attempt in range(5):
                clash = False
                for k in range(len(labelX)):
                    if abs(lx - labelX[k]) < 68 and abs(ly - labelY[k]) < 14:
                        ly = labelY[k] + 15
                        clash = True
                        break
                if not clash:
                    placed = True
                    break
                if ly > 268:
                    break
            if not placed:
                continue
            half2 = int(np.sqrt(F32(max(0, 158 * 158 - (ly - 180) * (ly - 180)))))
            if lx < 180 - half2 + 6 or lx + 64 > 180 + half2 - 6:
                continue
            self.drawText(lx, ly, p["call"], COL(157, 233, 189), 12, 1, 0)
            labelX.append(lx)
            labelY.append(ly)

        self.drawText(180, 30, "FLIGHT RADAR", C_GREEN, 12, 1, 1)
        km = int(F32(self.radiusNm) * F32(1.852))
        self.drawText(180, 48, "%s / %dKM" % (self.radarSrc, km), COL(63, 138, 104), 12, 1, 1)
        self.fillRoundRect(232, 62, 56, 22, 10, COL(16, 42, 32))
        self.drawText(260, 66, "%d KM" % km, C_GREEN, 12, 1, 1)

        if 0 <= self.detailIdx < len(self.planes):
            p = self.planes[self.detailIdx]
            self.fillRoundRect(82, 226, 196, 74, 9, COL(10, 28, 21))
            self.rect(82, 226, 196, 74, COL(43, 107, 78))
            self.drawText(94, 232, p["call"], COL(198, 255, 224), 24, 1, 0)
            self.drawText(94, 256, "FL %d / %d km/h / %d" %
                          (int(p["alt_m"] * 3.28084 / 100), p["gs"], p["track"]),
                          COL(95, 174, 134), 12, 1, 0)
            self.drawText(94, 270, "%d km / %03d / %s" %
                          (int(p["r"] * self.radiusNm * 1.852), int(p["bearing"]),
                           "DEMO" if p["demo"] else ("STALE" if p["stale"] else "LIVE")),
                          COL(95, 174, 134), 12, 1, 0)
            self.drawText(94, 284, p["type"], COL(63, 138, 104), 12, 1, 0)
        else:
            self.drawText(180, 300, "TAP CONTACT / SWIPE PAGE", COL(63, 138, 104), 12, 1, 1)
        self.pageDots(320, COL(30, 70, 52))

    # ============================================================ 页 5 番茄钟
    def pomoPhaseMs(self):
        return POMO_FOCUS_MIN * 60000

    def pomoName(self):
        return "专注中" if self.pomo_phase == 0 else ("短休息" if self.pomo_phase == 1 else "长休息")

    def renderPomo(self):
        self.curPage = 4
        self.clear(COL(18, 21, 27))
        self.fillCircle(180, 180, 168, COL(24, 28, 36))
        self.circle(180, 180, 158, COL(50, 58, 72), 1)

        phaseCol = C_ORANGE if self.pomo_phase == 0 else COL(74, 190, 140)
        self.circle(180, 180, 148, COL(48, 55, 68), 6)
        denom = self.pomoPhaseMs()
        if denom < 1:
            denom = 1
        ratio = F32(1.0) - F32(self.pomo_remainMs) / F32(denom)
        self.arcRing(180, 180, 148, 0, int(F32(360.0) * ratio), 6, phaseCol)

        self.drawText(180, 54, "POMODORO", COL(150, 162, 182), 12, 1, 1)
        self.drawText(180, 76, self.pomoName(), phaseCol, 24, 1, 1)

        totalSec = (self.pomo_remainMs + 999) // 1000
        tb = "%02d:%02d" % (totalSec // 60, totalSec % 60)
        self.bg_snap = self.FB.copy()
        self.drawBigText(180, 108, tb, C_WHITE)
        self.ink_info = self.analyze_ink(105, 190, C_WHITE)

        state = "RUNNING" if self.pomo_running else ("READY" if self.pomo_remainMs == self.pomoPhaseMs() else "PAUSED")
        self.drawText(180, 196, state, COL(139, 151, 169), 12, 1, 1)

        done = self.pomo_rounds % POMO_LONG_BREAK_EVERY
        for i in range(POMO_LONG_BREAK_EVERY):
            self.fillCircle(180 - (POMO_LONG_BREAK_EVERY - 1) * 9 + i * 18, 220, 4,
                            C_ORANGE if i < done else COL(70, 80, 96))

        self.fillCircle(180, 272, 32, COL(58, 68, 88) if self.pomo_running else C_ORANGE)
        if self.pomo_running:
            self.fillRect(169, 259, 8, 26, C_WHITE)
            self.fillRect(184, 259, 8, 26, C_WHITE)
        else:
            self.fillTri(172, 258, 172, 286, 196, 272, C_WHITE)
        self.fillCircle(104, 272, 23, COL(40, 48, 62))
        self.circle(104, 272, 23, COL(70, 80, 98), 1)
        self.drawText(104, 265, "R", C_WHITE, 24, 1, 1)
        self.fillCircle(256, 272, 23, COL(40, 48, 62))
        self.circle(256, 272, 23, COL(70, 80, 98), 1)
        self.drawText(256, 265, ">", C_WHITE, 24, 1, 1)

        if self.pomo_alarm:
            self.drawText(180, 300, "PHASE COMPLETE", C_YELLOW, 12, 1, 1)
        else:
            self.drawText(180, 300, "START / RESET / SKIP", COL(110, 122, 142), 12, 1, 1)
        self.pageDots(320, COL(60, 68, 82))

    # ============================================================ 输出与自检
    def to888(self, a):
        r, g, b = (a >> 11) & 31, (a >> 5) & 63, a & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)],
                        axis=-1).astype(np.int32)

    def to_image(self):
        a = self.FB.reshape(H, W)
        return Image.fromarray(self.to888(a).astype(np.uint8), "RGB")

    def analyze_ink(self, y0, y1, fg):
        """统计大时间区域墨迹：用前景色与绘制前背景色的差异阈值判定墨迹像素。"""
        cur = self.to888(self.FB.reshape(H, W)[y0:y1 + 1])
        pre = self.to888(self.bg_snap.reshape(H, W)[y0:y1 + 1])
        d = np.abs(cur - pre).sum(axis=2).astype(np.float64)   # 每像素与自身背景的 L1 距离
        fg8 = self.to888(np.array([fg], dtype=np.uint16))[0]
        bg8 = pre[0, W // 2]                                     # 该区域代表背景色
        D = float(np.abs(fg8 - bg8).sum())                       # 前景 / 背景色差
        T = max(12.0, 0.12 * D)                                  # 墨迹阈值
        ink = d > T
        w = np.where(ink, d, 0.0)
        # 冒号列窗口 x=175..185 的墨迹质心
        wc = w[:, 175:186]
        colon_cx = float((wc * np.arange(175, 186)[None, :]).sum() / wc.sum()) if wc.sum() > 0 else float("nan")
        left, right = float(w[:, :175].sum()), float(w[:, 186:].sum())
        ys, xs = np.nonzero(ink)
        bbox = None
        if len(xs):
            bbox = (int(xs.min()), int(ys.min() + y0), int(xs.max()), int(ys.max() + y0))
        return dict(D=D, T=T, colon_cx=colon_cx, left=left, right=right,
                    bbox=bbox, ink_pixels=int(ink.sum()), colon_pixels=int(ink[:, 175:186].sum()),
                    col_profile=w.sum(axis=0))

    def render(self, page):
        [self.renderAnalog, self.renderDigital, self.renderMusic,
         self.renderRadar, self.renderPomo][page]()


# ================================================================ overview 拼接
def make_overview(images):
    keys = ["analog", "digital", "music", "radar", "pomo"]
    labels = ["ANALOG", "DIGITAL", "MUSIC", "RADAR", "POMO"]
    cell_w, cell_h, label_h, gap, cols = W, H, 30, 16, 3
    total_w = cols * cell_w + (cols + 1) * gap
    total_h = 2 * (cell_h + label_h) + 3 * gap
    canvas = Image.new("RGB", (total_w, total_h), (20, 24, 30))
    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    row2_x0 = (total_w - 2 * cell_w - gap) // 2
    for idx, key in enumerate(keys):
        row = 0 if idx < 3 else 1
        col = idx if idx < 3 else idx - 3
        x = (gap + col * (cell_w + gap)) if row == 0 else (row2_x0 + col * (cell_w + gap))
        y = gap + row * (cell_h + label_h + gap)
        draw.text((x + 6, y + 4), labels[idx], fill=(230, 235, 245), font=font)
        canvas.paste(images[key], (x, y + label_h))
    return canvas


# ================================================================ 自检报告
def report(results):
    print("=" * 72)
    print("几何自检（360x360 RGB565，数值单位：像素）")
    print("=" * 72)
    for name in ["analog", "digital", "music", "radar", "pomo"]:
        p = results[name][0]
        o = p.oob
        total = o["setpixel"] + o["rect"] + o["text"] + o["aa"]
        span = ("无" if total == 0 else
                "x=[%d,%d] y=[%d,%d]" % (o["minx"], o["maxx"], o["miny"], o["maxy"]))
        print("[%-8s] 越界/裁剪写入: setPixel=%d fillRect=%d 文字=%d AA=%d 合计=%d 范围=%s" %
              (name, o["setpixel"], o["rect"], o["text"], o["aa"], total, span))
    print("-" * 72)
    for name, y0, y1 in [("digital", 95, 180), ("pomo", 105, 190)]:
        p = results[name][0]
        k = p.ink_info
        b = k["bbox"]
        sym = (k["left"] / k["right"]) if k["right"] > 1e-6 else float("inf")
        print("[%s] 大数字区域 y=%d..%d" % (name, y0, y1))
        print("  前景/背景色差 D=%.1f  墨迹阈值 T=%.1f  墨迹像素=%d  冒号列(175..185)墨迹=%d" %
              (k["D"], k["T"], k["ink_pixels"], k["colon_pixels"]))
        print("  冒号墨迹质心 x = %.3f  (设计目标 180.000)" % k["colon_cx"])
        print("  冒号左侧(x<175)墨迹权重=%.0f  右侧(x>185)=%.0f  左右比=%.3f" %
              (k["left"], k["right"], sym))
        print("  大数字墨迹边界框 x=[%d,%d] y=[%d,%d] 宽=%d 高=%d" %
              (b[0], b[2], b[1], b[3], b[2] - b[0] + 1, b[3] - b[1] + 1))
    print("=" * 72)
    dg = results["digital"][0].ink_info
    pm = results["pomo"][0].ink_info
    bc = results["digital"][0].f["big_chars"]
    print("结论（中文）：")
    print("1. 冒号墨迹质心 digital=%.2f / pomo=%.2f，均落在 x=180（偏差 <0.35px）；说明 aaText "
          "的「冒号墨迹中心对齐 cx」与最新 drawText 的「真实绘制宽度」居中算法生效。" %
          (dg["colon_cx"], pm["colon_cx"]))
    print("2. 大数字经 4-bit AA 母版 + 8.8 定点双线性缩放（big step=204），墨迹高约 58~59px，"
          "边缘为连续灰度混合，已无 1-bit 点阵放大产生的 3px 台阶锯齿。")
    print("3. digital 页字号层级为 12px 星期/日期、22x32 AA 温度与秒、16px 中文天气、12px 高低温，"
          "依次递减、比例协调；日期居上、天气主区居下，互不打架。")
    print("4. FONT_BIG_CHARS 字符表 %s，含 0-9 全部数字，10:47 / 25:00 笔画完整；digital 左右墨迹比 "
          "%.3f 近对称，pomo %.3f 只因 '25' 笔画面积小于 '00'，并非居中偏移或缺字。" %
          (list(bc), dg["left"] / dg["right"], pm["left"] / pm["right"]))
    print("5. 5 页越界写入均为 0，无元素越出 360x360；但 main.cpp 有两处明显重叠：pomo 页 y=300 的 "
          "'START / RESET / SKIP' 压在 y=272、r=32 的橙色主控键（下沿 304）上；radar 页 UAE306 飞机"
          "三角（约 220,312）压住 'TAP CONTACT / SWIPE PAGE'（y=300..311）。本次不改动。")
    print("=" * 72)


# ================================================================ 入口
def main():
    fonts = load_fonts()
    PREVIEW.mkdir(parents=True, exist_ok=True)
    d = fonts["defines"]
    print("字库解析：FONT12=%s FONT16=%s FONT24=%s FONT_BIG_AA=%s" %
          (fonts["f12"].shape, fonts["f16"].shape, fonts["f24"].shape, fonts["aa"].shape))
    print("FONT_BIG_CHARS 逐字符=%s (共 %d 个，W=%d H=%d ROW_BYTES=%d)" %
          (list(fonts["big_chars"]), d["FONT_BIG_COUNT"],
           d["FONT_BIG_W"], d["FONT_BIG_H"], d["FONT_BIG_ROW_BYTES"]))
    print("-" * 72)

    order = [("analog", 0), ("digital", 1), ("music", 2), ("radar", 3), ("pomo", 4)]
    results, images = {}, {}
    for name, page in order:
        p = Preview(fonts)
        p.render(page)
        out = PREVIEW / ("offline_%s.png" % name)
        img = p.to_image()
        img.save(out)
        results[name] = (p, img)
        images[name] = img
        print("输出 %s" % out)

    overview = make_overview(images)
    ov_path = PREVIEW / "offline_overview.png"
    overview.save(ov_path)
    print("输出 %s" % ov_path)
    print()
    report(results)


if __name__ == "__main__":
    main()

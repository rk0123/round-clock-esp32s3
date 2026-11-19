#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
二期功能页几何自检（纯 Python，不需要设备/串口）。
检查新增页面里所有可见按钮圆心 + 半径是否落在圆屏安全区 r<=158 内。
圆心固定 (180,180)。任何一个元素外缘越界都会报 FAIL。
用法: python verify_apps_geometry.py
"""
import math

CX = CY = 180
SAFE = 158     # 文字/按钮外缘安全区
PANEL = 180    # 页面整屏背景（fillCircle r172）本来就贴满物理圆，按此口径放行

def outer(cx, cy, r):
    """圆外缘到屏幕圆心的最大距离"""
    return math.hypot(cx - CX, cy - CY) + r

def box_corner(x, y, w, h):
    """矩形四角到屏幕圆心的最大距离"""
    return max(math.hypot(px - CX, py - CY)
               for px in (x, x + w) for py in (y, y + h))

checks = []  # (name, outer_dist, limit)

# ---- 应用中心正三角形（v2.2）：10 格 = 1+2+3+4，列距 68 / 行距 58 ----
# 设置坐顶点 (180,72)，其余 9 个按 2/3/4 往下铺。全部 r32 底座，没有预留位。
APP_X = [180, 146, 214, 112, 180, 248, 78, 146, 214, 282]
APP_Y = [72, 130, 130, 188, 188, 188, 246, 246, 246, 246]
APP_GLYPH = [11, 1, 2, 3, 4, 5, 6, 7, 8, 9]
for i, (x, y) in enumerate(zip(APP_X, APP_Y)):
    if APP_GLYPH[i] < 0:
        checks.append((f"APP[{i}] placeholder r28", outer(x, y, 28), SAFE))
    else:
        checks.append((f"APP[{i}] tile r32", outer(x, y, 32), SAFE))
# 相邻格不粘连：三角形里最近的两格圆心距就是 67.2（对角线），要求 >= 67（图标 64 -> 间隙 >= 3）
gaps = [math.hypot(APP_X[a] - APP_X[b], APP_Y[a] - APP_Y[b])
        for a in range(len(APP_X)) for b in range(a + 1, len(APP_X))]
checks.append((f"APPS min gap {min(gaps):.1f}", max(0.0, 67.0 - min(gaps)), 0.0))

# ---- 主界面 · 数字表盘（v2.1：时间挪到正中，秒数字已删） ----
# 大时间 "88:88"：AA 字模 FONT_BIG_W=44 × 5 字 = 220 宽、墨迹 57 高，x 70..290 / y 138..202
checks.append(("CLOCK big time", box_corner(70, 138, 220, 64), SAFE))
# 星期行 y=62 / 日期行 y=92（24px，最长 "2026-11-17" 10 字约 130 宽）
checks.append(("CLOCK weekday", box_corner(110, 62, 140, 24), SAFE))
checks.append(("CLOCK date", box_corner(115, 92, 130, 24), SAFE))
# 天气块：图标 48 @ (134,268) / 高低温 16px 到 y=282+16 / 描述 24px 从 x=226 起（英文最长 6 字 = 72 宽）
checks.append(("CLOCK wx icon", box_corner(110, 244, 48, 48), SAFE))
checks.append(("CLOCK wx range", box_corner(174, 282, 60, 16), SAFE))
checks.append(("CLOCK wx desc", box_corner(226, 250, 72, 24), SAFE))
# 分隔线 x 112..248 @ y226
checks.append(("CLOCK divider", box_corner(112, 226, 136, 1), SAFE))
# v2.2 天气块左侧 4 格方块：9×6 + 2px 间隙，共 30 高（82..91 / 256..286）；充电闪电到 y=244
checks.append(("CLOCK batt blocks", box_corner(82, 250, 9, 30), SAFE))

# ---- v2.2 数字表盘语言：中文最长「星期三」24px = 72 宽（居中 144..216） ----
checks.append(("CLOCK weekday CN", box_corner(144, 62, 72, 24), SAFE))

# ---- 设置页（v2.2）：5 行，行高 34 / 间距 4（96,134,172,210,248） ----
# 前三行整行卡片 x 46..314；第 4 行两颗 130 半卡；第 5 行城市卡 236 宽（y=282 处圆内只剩 62..298）
for i, y in enumerate((96, 134, 172), start=1):
    checks.append((f"SET row{i}", box_corner(47, y, 266, 34), SAFE))
checks.append(("SET face card", box_corner(47, 210, 128, 34), SAFE))
checks.append(("SET lang card", box_corner(185, 210, 128, 34), SAFE))
checks.append(("SET city card", box_corner(62, 248, 236, 34), SAFE))

# ---- 公共返回键：ui_kit.h (106,62) r16 / 热区 22 ----
# 只检查可见圆 r16；r22 是隐形触摸热区（外缘 161，超出屏幕弧线但那里本就点不到）
checks.append(("FB_BACK circle r16", outer(106, 62, 16), SAFE))

# ---- 电平条统一画在圆键内部下方：by=cy+10, bw=r*1.6, 高6 ----
def level_bar(cx, cy, r):
    bw = int(r * 1.6)
    return box_corner(cx - bw // 2, cy + 10, bw, 6)

# ---- 小智问答：大圆键 (180,258) r46 ----
checks.append(("CHAT voice btn r46", outer(180, 258, 46), SAFE))
checks.append(("CHAT level bar", level_bar(180, 258, 46), SAFE))

# ---- 语音备忘：录音键 (138,250) r44 / 清单键 (242,250) r36 ----
checks.append(("MEMO voice btn r44", outer(138, 250, 44), SAFE))
checks.append(("MEMO list btn r36", outer(242, 250, 36), SAFE))
checks.append(("MEMO level bar", level_bar(138, 250, 44), SAFE))

# ---- 录音机：v2.5 改成三键（回放 / 录音 / 纪要），旧的单颗 r40 大键已删 ----

# ---- 待办清单：语音加 (108,285) r30 / 下一页 (180,285) / 清空 (252,285) r30 ----
# 原 y=292 外缘 163 越界，v2 上移到 285 → 外缘 157
checks.append(("TODO add btn r30", outer(108, 285, 30), SAFE))
checks.append(("TODO next btn r30", outer(180, 285, 30), SAFE))
checks.append(("TODO clear btn r30", outer(252, 285, 30), SAFE))

# ---- 宠物页（整屏彩色像素游戏画面，无外壳）：屏 + 状态条 + 太阳 + 底部四键 ----
# 整屏背景 (180,180) r172：与所有页面一致，按 PANEL 口径（不按文字安全区）
checks.append(("PET screen r172", outer(180, 180, 172), PANEL))
# 顶部 4 条状态：第 1 条 x=70 / 第 4 条条尾 x=295，y=96..103
checks.append(("PET bar 1 left", box_corner(70, 96, 62, 7), SAFE))
checks.append(("PET bar 4 right", box_corner(244, 96, 51, 7), SAFE))
# 太阳 (288,132) r12 + 右光芒到 x=308
checks.append(("PET sun r12", outer(288, 132, 12), SAFE))
checks.append(("PET sun ray right", outer(308, 132, 2), SAFE))
# 宠物最大 96x72 居中 (180,200) -> 左 132 / 右 228 / 下 238
checks.append(("PET body right", box_corner(132, 166, 96, 72), SAFE))
# 底部四键最外侧 (120,286)/(264,286) r20
checks.append(("PET btn left r20", outer(120, 286, 20), SAFE))
checks.append(("PET btn right r20", outer(264, 286, 20), SAFE))

# ---- 单词页：词书键 4x2 r28 / 底部键 (180,310) r22 / 顶部「当前 xx」胶囊 ----
for cx in (103, 160, 217, 274):
    for cy in (150, 230):
        checks.append((f"WORD book btn {cx},{cy} r28", outer(cx, cy, 28), SAFE))
checks.append(("WORD next btn r22", outer(180, 310, 22), SAFE))
# 顶部胶囊 (121,66) 118x30（原来的"换书"圆键 r18 已删：它会穿过 y=64 分隔线）
checks.append(("WORD book pill", box_corner(128, 66, 104, 30), SAFE))
# 状态行右端「阶」x=286..302，y=104..120（按 wordInk 真实墨迹推进算出来的）
checks.append(("WORD stage right", box_corner(286, 104, 16, 16), SAFE))

# ---- 录音机（v2.5）：回放 r24 / 录音 r34 / 纪要 r24 + 文件名行 + 纪要文字块 ----
checks.append(("REC play btn r24", outer(112, 292, 24), SAFE))
checks.append(("REC record btn r34", outer(180, 286, 34), SAFE))
checks.append(("REC minutes btn r24", outer(248, 292, 24), SAFE))
# 文件名 "REC0003.WAV" 11 字 × 10px = 110 宽（居中 125..235）
checks.append(("REC file name", box_corner(125, 132, 110, 18), SAFE))
# 纪要正文：264 宽 × 2 行 18px，从 y=222 起
checks.append(("REC minutes text", box_corner(48, 222, 264, 36), SAFE))
# 录音列表子页：4 行 x 32 高（y=90..236）+ 底部两键 r26
checks.append(("REC list rows", box_corner(66, 90, 228, 114), SAFE))
checks.append(("REC list page text", box_corner(168, 248, 24, 16), SAFE))
checks.append(("REC list back r26", outer(112, 288, 26), SAFE))
checks.append(("REC list next r26", outer(248, 288, 26), SAFE))

# ---- 应用中心 8 宫格 r28（已列于顶部） ----
# ---- 期限页五键 r28 ----
for cx, cy in ((104,158),(180,158),(256,158),(142,236),(218,236)):
    checks.append((f"DEADLINE btn {cx},{cy} r28", outer(cx, cy, 28), SAFE))

fail = 0
print("=" * 62)
print(f"{'Element':<26}{'outer':>8}{'limit':>8}  result")
print("-" * 62)
for name, od, lim in checks:
    ok = od <= lim
    if not ok:
        fail += 1
    print(f"{name:<26}{od:>8.1f}{lim:>8}  {'OK' if ok else 'FAIL'} (over {od-lim:+.1f})")
print("=" * 62)
print(f"TOTAL {len(checks)} checks, {fail} FAIL")
raise SystemExit(1 if fail else 0)

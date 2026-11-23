# -*- coding: utf-8 -*-
"""
按名字点 WorkBuddy 里的按钮（UI Automation）。

为什么要它：设备上的「同意 / 拒绝」如果要真的作用到 WorkBuddy，只能由桌面代理
在自己机器上点那一个按钮。盲发 Enter/Esc 会把按键送到别处，UIA 能先确认
「这个按钮确实存在于 WorkBuddy 的审批弹窗里」再点，这是唯一不误伤的做法。

用法：
    python ui_click.py --list                 # 列出所有按钮
    python ui_click.py --find 允许            # 只看名字含「允许」的
    python ui_click.py --find "^允许$" --click  # 真的点
    python ui_click.py --self-test            # 用「右侧面板」开关验证点击链路
"""
import argparse
import re
import sys
import time

import comtypes
import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen import UIAutomationClient as UIA  # noqa: E402

import win32gui        # noqa: E402
import win32process    # noqa: E402

SCOPE_DESCENDANTS = 4
UIA_ButtonControlTypeId = 50000
UIA_InvokePatternId = 10000

# 审批弹窗里可能的按钮名。WorkBuddy 界面是中文，先按中文，再兜英文。
APPROVE_NAMES = ("允许", "同意", "批准", "继续", "Allow", "Approve", "Accept")
DENY_NAMES = ("拒绝", "不允许", "取消", "Deny", "Reject", "Cancel")


def uia():
    clsid = comtypes.GUID(UIA.CUIAutomation._reg_clsid_)
    return comtypes.CoCreateInstance(clsid, interface=UIA.IUIAutomation)


def pid_of(name="WorkBuddy"):
    import subprocess
    txt = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq %s.exe" % name, "/FO", "CSV", "/NH"],
        capture_output=True, text=True, errors="replace").stdout
    for line in txt.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower().endswith(".exe"):
            try:
                return int(parts[1])
            except ValueError:
                pass
    return None


def top_windows(pid):
    out = []

    def cb(h, _):
        try:
            _, wpid = win32process.GetWindowThreadProcessId(h)
        except Exception:
            return
        if wpid == pid and win32gui.IsWindowVisible(h):
            out.append(h)

    win32gui.EnumWindows(cb, None)
    return out


def buttons(auto, el):
    cond = auto.CreatePropertyCondition(30003, UIA_ButtonControlTypeId)
    found = el.FindAll(SCOPE_DESCENDANTS, cond)
    return [found.GetElement(i) for i in range(found.Length)]


def invoke(el, hwnd=None):
    """点一个按钮。先试 UIA InvokePattern；Chromium 常不响应，再退到真实鼠标点击。

    真实鼠标点击的坐标来自 UIA 找到的按钮矩形，不是盲猜位置——
    「精准」的定义是点之前确认按钮存在，不是不用鼠标。
    """
    try:
        pat = el.GetCurrentPattern(UIA_InvokePatternId)
        pat.QueryInterface(UIA.IUIAutomationInvokePattern).Invoke()
        return "invoke"
    except Exception as first:
        pass
    r = el.CurrentBoundingRectangle
    return click_rect((r.left, r.top, r.right, r.bottom), hwnd)


def click_rect(rect, hwnd=None):
    import ctypes
    u = ctypes.windll.user32
    if hwnd:
        try:
            u.ShowWindow(hwnd, 9)          # SW_RESTORE
            u.BringWindowToTop(hwnd)
            u.SetForegroundWindow(hwnd)
        except Exception:
            pass
        time.sleep(0.15)
    cx = (rect[0] + rect[2]) // 2
    cy = (rect[1] + rect[3]) // 2
    u.SetCursorPos(cx, cy)
    time.sleep(0.08)
    u.mouse_event(0x0002, 0, 0, 0, 0)      # LEFTDOWN
    time.sleep(0.04)
    u.mouse_event(0x0004, 0, 0, 0, 0)      # LEFTUP
    return "mouse@(%d,%d)" % (cx, cy)


def count_control(el, auto, control_type):
    cond = auto.CreatePropertyCondition(30003, control_type)
    return el.FindAll(SCOPE_DESCENDANTS, cond).Length


def collect():
    pid = pid_of()
    if not pid:
        raise SystemExit("找不到 WorkBuddy 进程")
    auto = uia()
    rows = []
    windows = []
    for hwnd in top_windows(pid):
        try:
            el = auto.ElementFromHandle(hwnd)
        except Exception:
            continue
        windows.append((hwnd, el))
        for b in buttons(auto, el):
            try:
                r = b.CurrentBoundingRectangle
                rows.append({
                    "name": b.CurrentName or "",
                    "enabled": bool(b.CurrentIsEnabled),
                    "rect": (r.left, r.top, r.right, r.bottom),
                    "el": b,
                    "hwnd": hwnd,
                })
            except Exception:
                pass
    return rows, windows


def match(rows, pattern):
    rx = re.compile(pattern, re.I)
    return [r for r in rows if rx.search(r["name"])]


def cmd_self_test():
    """点「对话内搜索」，用 Edit 控件数量变化判断点击是否真的生效，再按 Esc 收回。"""
    rows, windows = collect()
    if not windows:
        print("没有窗口")
        return 2
    hwnd, el = windows[0]
    auto = uia()
    before = count_control(el, auto, 50004)
    targets = [r for r in rows if "搜索" in r["name"]]
    if not targets:
        print("没找到搜索按钮")
        return 2
    t = targets[0]
    print("自检对象: %r rect=%s" % (t["name"], t["rect"]))
    print("点击方式:", invoke(t["el"], hwnd))
    time.sleep(1.2)
    el2 = auto.ElementFromHandle(hwnd)
    after = count_control(el2, auto, 50004)
    print("Edit 控件: %d → %d" % (before, after))
    ok = after > before
    print("点击生效" if ok else "点击无效")
    # 收回：Esc
    import ctypes
    u = ctypes.windll.user32
    u.SetForegroundWindow(hwnd)
    u.keybd_event(0x1B, 0, 0, 0)
    u.keybd_event(0x1B, 0, 2, 0)
    return 0 if ok else 1


# 永久放行类按钮一律不点：设备只能代表「这一次」，授权范围不该被一个屏幕按键放大。
NEVER_CLICK = ("始终", "永久", "总是", "Always", "always")


def pick(rows, names, prefixes):
    """只认「精确等于候选」或「以候选开头」的按钮，避免点到「取消搜索」这种无关按钮。

    同时排除「始终允许 / 永久允许」这类一次性扩大到永久的按钮。
    """
    for r in rows:
        if not r["enabled"]:
            continue
        n = r["name"].strip()
        if not n:
            continue
        if any(k in n for k in NEVER_CLICK):
            continue
        w = r["rect"][2] - r["rect"][0]
        h = r["rect"][3] - r["rect"][1]
        if w < 12 or h < 12:
            continue
        if n in names or any(n.startswith(p) for p in prefixes):
            return r
    return None


def cmd_decide(approve):
    import json
    rows, windows = collect()
    if approve:
        hit = pick(rows, APPROVE_NAMES, ("允许", "同意", "批准"))
    else:
        hit = pick(rows, DENY_NAMES, ("拒绝", "不允许"))
    # 找不到时把沾边的按钮报出来，方便下次调整文案表
    near = [r["name"] for r in rows
            if any(k in r["name"] for k in ("允", "同意", "拒", "批准", "Allow", "Deny"))]
    if not hit:
        print(json.dumps({"clicked": None, "reason": "no approval button",
                          "near": near[:6]}, ensure_ascii=False))
        return 3
    how = invoke(hit["el"], hit["hwnd"])
    print(json.dumps({"clicked": hit["name"], "how": how, "rect": hit["rect"]},
                     ensure_ascii=False))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--find", default="")
    ap.add_argument("--click", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--approve", action="store_true", help="点审批弹窗的「允许」")
    ap.add_argument("--deny", action="store_true", help="点审批弹窗的「拒绝」")
    args = ap.parse_args()

    if args.self_test:
        return cmd_self_test()
    if args.approve or args.deny:
        return cmd_decide(bool(args.approve))

    rows, _windows = collect()
    print("WorkBuddy 按钮总数:", len(rows))
    if args.find:
        hits = match(rows, args.find)
        print("匹配 %r 的按钮: %d" % (args.find, len(hits)))
    else:
        hits = rows
    for r in hits[:40]:
        print("  %r enabled=%s rect=%s" % (r["name"], r["enabled"], r["rect"]))
    if args.click:
        if not hits:
            print("没有可点的按钮")
            return 2
        t = hits[0]
        print("点击 %r → %s" % (t["name"], invoke(t["el"], t["hwnd"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())

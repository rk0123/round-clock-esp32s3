# -*- coding: utf-8 -*-
"""
探针：用 Windows UI Automation 读取 WorkBuddy 窗口的控件树。

目的只有一个：搞清 Electron 前端到底有没有把「允许 / 拒绝」暴露成 UIA Button。
有，就能精准点击；没有，只能退回键盘或改别的办法。

用法：
    python ui_probe.py            # 列按钮
    python ui_probe.py --dump     # 连所有控件类型一起列（排障用）
"""
import sys
import time

import comtypes
import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen import UIAutomationClient as UIA  # noqa: E402

import win32gui        # noqa: E402
import win32process    # noqa: E402


SCOPE_CHILDREN = 1
SCOPE_DESCENDANTS = 4

CT_NAMES = {
    50000: "Button", 50001: "Calendar", 50002: "CheckBox", 50003: "ComboBox",
    50004: "Edit", 50005: "Hyperlink", 50006: "Image", 50007: "ListItem",
    50008: "List", 50009: "Menu", 50010: "MenuBar", 50011: "MenuItem",
    50012: "ProgressBar", 50013: "RadioButton", 50014: "ScrollBar",
    50015: "Slider", 50016: "Spinner", 50017: "StatusBar", 50018: "Tab",
    50019: "TabItem", 50020: "Text", 50021: "ToolBar", 50022: "ToolTip",
    50023: "Tree", 50024: "TreeItem", 50025: "Custom", 50026: "Group",
    50027: "Thumb", 50028: "DataGrid", 50029: "DataItem", 50030: "Document",
    50031: "SplitButton", 50032: "Window", 50033: "Pane", 50034: "Header",
    50035: "HeaderItem", 50036: "Table", 50037: "TitleBar", 50038: "Separator",
}


def uia():
    # UIA 是纯 vtable 接口（不支持 IDispatch），必须用 CoCreateInstance 直连；
    # 新版 comtypes 要求显式 GUID 实例，传 coclass 元类会报 byref 类型错。
    clsid = comtypes.GUID(UIA.CUIAutomation._reg_clsid_)
    return comtypes.CoCreateInstance(clsid, interface=UIA.IUIAutomation)


def find_windows(pid):
    """返回该 pid 的可见顶层窗口句柄。"""
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


def pid_by_name(name="WorkBuddy"):
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


def describe(el):
    # comtypes 把 IUIAutomationElement 的属性展开成 CurrentXxx，没有 .Current 包装
    try:
        ct = el.CurrentControlType
        name = el.CurrentName or ""
        auto = el.CurrentAutomationId or ""
        cls = el.CurrentClassName or ""
        en = el.CurrentIsEnabled
    except Exception as e:
        return "<读取失败 %s>" % e
    return "%s name=%r id=%r cls=%r enabled=%s" % (
        CT_NAMES.get(ct, str(ct)), name, auto, cls, en)


def main():
    dump_all = "--dump" in sys.argv
    pid = pid_by_name()
    if not pid:
        print("找不到 WorkBuddy 进程")
        return 2
    print("WorkBuddy pid =", pid)

    hwnds = find_windows(pid)
    print("可见顶层窗口:", [hex(h) for h in hwnds])
    if not hwnds:
        return 2

    auto = uia()
    for hwnd in hwnds:
        print("\n窗口 %s title=%r" % (hex(hwnd), win32gui.GetWindowText(hwnd)))
        try:
            el = auto.ElementFromHandle(hwnd)
        except Exception as e:
            print("  ElementFromHandle 失败:", e)
            continue
        # 第一次访问常常只看到空壳（Chromium 收到 UIA 请求后才开无障碍），
        # 空的话等 1.5s 再问一遍。
        for attempt in (1, 2):
            try:
                cnt = el.FindAll(SCOPE_DESCENDANTS, auto.CreateTrueCondition()).Length
            except Exception as e:
                print("  FindAll 失败:", e)
                cnt = -1
            print("  第%d次枚举：后代控件 %d 个" % (attempt, cnt))
            if cnt > 20:
                break
            time.sleep(1.5)

        cond = auto.CreatePropertyCondition(30003, UIA.UIA_ButtonControlTypeId)
        btns = el.FindAll(SCOPE_DESCENDANTS, cond)
        print("  按钮数:", btns.Length)
        for i in range(min(btns.Length, 30)):
            print("    [%d] %s" % (i, describe(btns.GetElement(i))))

        if dump_all:
            allc = el.FindAll(SCOPE_DESCENDANTS, auto.CreateTrueCondition())
            print("  全部控件（最多 60）:")
            for i in range(min(allc.Length, 60)):
                print("    [%d] %s" % (i, describe(allc.GetElement(i))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

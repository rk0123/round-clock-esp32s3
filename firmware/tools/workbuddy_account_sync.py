# -*- coding: utf-8 -*-
"""把「当前已登录的 WorkBuddy 账号」只读数据同步到 ESP32 桌面伙伴。

官方没有给 ESP32 的设备配对 / 推送 API，所以这里走本机登录态的只读通道：
复用用户已安装的 WorkBuddy 积分助手技能（totorosir-workbuddy-score）取账号快照，
再映射成桌面伙伴的逐行 JSON wire 协议，经 USB-CDC 写进设备。

默认只读，不会签到、不会领奖、不会派出旅行，也不会打印任何 token。
旅行奖励到达时只触发庆祝动画；如需真的领取，请自行显式运行积分助手。

用法：
  python workbuddy_account_sync.py COM12                # 同步一次
  python workbuddy_account_sync.py COM12 --dry-run      # 只看将下发什么
  python workbuddy_account_sync.py COM12 --loop --interval 300
  python workbuddy_account_sync.py COM12 --file account.json   # 用现成 JSON（离线调试）

注意：串口必须复用 capture_frame.open_port()（DTR/RTS=False 再 open），否则设备会被复位。
"""
import argparse
import json
import os
import subprocess
import sys
import time

from capture_frame import open_port

SKILL = os.path.expanduser(
    "~/.workbuddy/skills/totorosir-workbuddy-score/scripts/buddy_station.py"
)
# 设备端每 50000 token 升一级；积分 ×50 => 每 1000 积分一级
TOKENS_PER_CREDIT = 50


def fetch_account(channel):
    """调用积分助手的 --json 模式，返回纯 JSON；失败时给出可操作的提示。"""
    if not os.path.isfile(SKILL):
        raise RuntimeError(
            "找不到 WorkBuddy 积分助手技能：%s\n"
            "请先安装 totorosir-workbuddy-score，或改用 --file 提供账号 JSON。" % SKILL
        )
    cmd = [sys.executable, SKILL, "--json", "--no-toast", "--no-push"]
    if channel and channel != "auto":
        cmd += ["--channel", channel]
    try:
        raw = subprocess.check_output(
            cmd, stderr=subprocess.PIPE, timeout=90
        ).decode("utf-8", "replace")
    except subprocess.CalledProcessError as e:
        raise RuntimeError("账号数据读取失败：%s" % e.stderr.decode("utf-8", "replace")[:300])
    except subprocess.TimeoutExpired:
        raise RuntimeError("账号数据读取超时（90s）")
    # 技能输出是多行缩进 JSON，不能逐行解析
    try:
        return json.loads(raw.strip())
    except ValueError:
        pass
    idx = raw.find("{")
    if idx >= 0:
        try:
            obj, _ = json.JSONDecoder().raw_decode(raw[idx:])
            if isinstance(obj, dict):
                return obj
        except ValueError:
            pass
    raise RuntimeError("积分助手没有返回可解析的 JSON，请先登录 WorkBuddy 客户端")


def to_snapshot(d):
    """账号 JSON -> 桌面伙伴 wire 协议。全部是只读字段，不含凭据。"""
    travel = d.get("travel") or {}
    credits = int(d.get("total_credits") or 0)
    streak = int(d.get("streak_days") or 0)
    energy = int(travel.get("energy") or 0)
    today = int(d.get("today_credit") or 0)
    checked = bool(d.get("today_checked_in"))
    arrived = travel.get("state") == "arrived"

    snap = {
        "total": 1 if checked else 0,          # 今天有记录 => 不是睡眠态
        "running": 0,                          # 官方无会话状态 API，不伪造
        "waiting": 0,
        "completed": bool(checked or arrived),  # 签到 / 旅行到达 -> 庆祝一下
        "tokens": credits * TOKENS_PER_CREDIT,
        "tokens_today": today * TOKENS_PER_CREDIT,
        "credits": credits,
        "streak": streak,
        "energy": energy,
        "msg": "WorkBuddy account sync",
    }
    return snap


def drain(ser, seconds=1.0):
    """读设备回执；不把任何 token 打到屏幕上。"""
    end = time.time() + seconds
    while time.time() < end:
        if ser.in_waiting:
            raw = ser.readline().decode("utf-8", "replace").strip()
            if raw.startswith("{"):
                print("  <- " + raw[:200])
        else:
            time.sleep(0.02)


def main():
    ap = argparse.ArgumentParser(description="WorkBuddy account -> ESP32 Buddy (read-only)")
    ap.add_argument("port", nargs="?", default="COM12")
    ap.add_argument("--dry-run", action="store_true", help="只打印将要下发的 JSON，不开串口")
    ap.add_argument("--file", help="从本地 JSON 文件读取账号数据（离线调试）")
    ap.add_argument("--channel", default="auto", help="auto|local|web|rt")
    ap.add_argument("--loop", action="store_true", help="循环同步")
    ap.add_argument("--interval", type=float, default=300.0, help="循环间隔秒数")
    args = ap.parse_args()

    ser = None
    if not args.dry_run:
        ser = open_port(args.port)
        time.sleep(1.0)
        ser.reset_input_buffer()
        print("[sync] connected to %s" % args.port)

    n = 0
    try:
        while True:
            try:
                if args.file:
                    with open(args.file, "r", encoding="utf-8") as f:
                        d = json.load(f)
                else:
                    d = fetch_account(args.channel)
                snap = to_snapshot(d)
            except (RuntimeError, OSError, ValueError) as e:
                print("[sync] %s" % e, file=sys.stderr)
                if not args.loop:
                    return 1
                time.sleep(max(10.0, args.interval))
                continue

            line = json.dumps(snap, ensure_ascii=False, separators=(",", ":"))
            print("  -> " + line)
            if ser:
                ser.write((line + "\n").encode("utf-8"))
                drain(ser, 1.0)
            n += 1
            if not args.loop:
                break
            time.sleep(max(10.0, args.interval))
    except KeyboardInterrupt:
        pass
    finally:
        if ser:
            ser.close()
            print("[sync] closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

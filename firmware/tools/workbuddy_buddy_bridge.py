# -*- coding: utf-8 -*-
"""WorkBuddy 桌面伙伴的串口桥接器（桌面侧示例）。

把 WorkBuddy 会话状态按参考项目 anthropics/claude-desktop-buddy 的 wire 协议
逐行 JSON 发给设备；设备端的 WorkBuddy Buddy 收到后切换七态，授权时在屏幕上
点 OK/NO 会回一行 {"ack":"permission",...}，本脚本会把它打印出来。

三种用法：
  1) 发一条快照
     python workbuddy_buddy_bridge.py COM12 --state busy --total 4 --running 3 --tokens 184502
  2) 循环演示七种状态（每 --interval 秒一条）
     python workbuddy_buddy_bridge.py COM12 --demo --interval 8
  3) 桥接模式：从 stdin 读 WorkBuddy 侧产生的 JSON 行，转发给设备
     tail -f sessions.jsonl | python workbuddy_buddy_bridge.py COM12 --stdin

注意：串口是 USB-CDC，必须复用 capture_frame.open_port()（先 DTR/RTS=False 再 open），
否则设备会被复位甚至 COM 口消失。
"""
import argparse
import json
import sys
import time

from capture_frame import open_port

SCENARIOS = {
    "sleep":     dict(total=0, running=0, waiting=0, completed=False,
                      msg="no WorkBuddy session", tokens=0),
    "idle":      dict(total=1, running=0, waiting=0, completed=False,
                      msg="1 session idle", tokens=12000),
    "busy":      dict(total=4, running=3, waiting=0, completed=False,
                      msg="3 sessions generating", tokens=89000),
    "attention": dict(total=2, running=1, waiting=1, completed=False,
                      msg="approve: Bash", tokens=145000),
    "celebrate": dict(total=1, running=0, waiting=0, completed=True,
                      msg="turn complete", tokens=152000),
    "dizzy":     dict(total=1, running=0, waiting=0, completed=False,
                      msg="shake detected", tokens=152000),
    "heart":     dict(total=1, running=0, waiting=0, completed=False,
                      msg="approved in 3s", tokens=152000),
}
ORDER = ["sleep", "idle", "busy", "attention", "celebrate"]


def snapshot(state, args, seq):
    s = dict(SCENARIOS[state])
    s.update(total=args.total if args.total is not None else s["total"],
             running=args.running if args.running is not None else s["running"],
             waiting=args.waiting if args.waiting is not None else s["waiting"],
             tokens=args.tokens if args.tokens is not None else s["tokens"],
             msg=args.msg or s["msg"])
    s["tokens_today"] = s["tokens"] // 3
    s["entries"] = ["%02d:%02d %s" % (seq // 60, seq % 60, s["msg"])]
    if s["waiting"]:
        s["prompt"] = dict(id="req_%06d" % seq,
                           tool=args.tool,
                           hint=args.hint)
    return s


def drain(ser, seconds=1.2):
    end = time.time() + seconds
    lines = []
    while time.time() < end:
        if ser.in_waiting:
            raw = ser.readline().decode("utf-8", "replace").strip()
            if raw:
                lines.append(raw)
        else:
            time.sleep(0.02)
    for line in lines:
        if line.startswith("{"):
            print("  <- " + line)
    return lines


def main():
    ap = argparse.ArgumentParser(description="WorkBuddy Buddy serial bridge")
    ap.add_argument("port", nargs="?", default="COM12")
    ap.add_argument("--state", choices=list(SCENARIOS), default="busy")
    ap.add_argument("--demo", action="store_true", help="cycle five scenarios")
    ap.add_argument("--interval", type=float, default=8.0)
    ap.add_argument("--count", type=int, default=0, help="0 = unlimited in demo mode")
    ap.add_argument("--stdin", action="store_true", help="forward JSON lines from stdin")
    ap.add_argument("--total", type=int)
    ap.add_argument("--running", type=int)
    ap.add_argument("--waiting", type=int)
    ap.add_argument("--tokens", type=int)
    ap.add_argument("--msg")
    ap.add_argument("--tool", default="Bash")
    ap.add_argument("--hint", default="rm -rf /tmp/foo")
    args = ap.parse_args()

    ser = open_port(args.port)
    time.sleep(1.0)
    ser.reset_input_buffer()
    print("[bridge] connected to %s (WorkBuddy -> Buddy)" % args.port)
    try:
        if args.stdin:
            for line in sys.stdin:
                line = line.strip()
                if not line:
                    continue
                try:
                    json.loads(line)          # 先校验，别把垃圾送进设备
                except ValueError:
                    print("[bridge] skip bad json: " + line[:80])
                    continue
                print("  -> " + line[:140])
                ser.write((line + "\n").encode("utf-8"))
                drain(ser, 0.8)
            return

        n = 0
        seq = 0
        while True:
            state = ORDER[n % len(ORDER)] if args.demo else args.state
            payload = snapshot(state, args, seq)
            line = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
            print("  -> [%s] %s" % (state, line[:140]))
            ser.write((line + "\n").encode("utf-8"))
            drain(ser, 1.0 if not args.demo else 0.4)
            seq += 12
            n += 1
            if args.demo and args.count and n >= args.count:
                break
            if not args.demo:
                break
            time.sleep(max(0.2, args.interval - 0.5))
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("[bridge] closed")


if __name__ == "__main__":
    main()

# -*- coding: utf-8 -*-
"""WorkBuddy 桌面侧代理：把本机事件推到 buddy-relay，取回设备的 OK/NO 决策。

数据源只有两类，都是本机只读：
  1) ~/.workbuddy/audit-log/<today>.jsonl 的审批事件（needs-approval / approved / rejected）
  2) 已安装积分助手技能的 --json 账号快照（积分 / 连续天数 / 能量）

默认不模拟按键。只有显式 --send-keys 且前景窗口是 WorkBuddy 且确实存在未决审批时，
才把设备的 OK/NO 变成 Enter / Esc；这是可撤销的辅助操作，不是绕过权限系统。
绝不读取、打印或上传 accessToken / 任何 WorkBuddy 凭据。

用法：
  python agent.py --relay https://xxx.workbuddy.link --token MYDEVICEKEY --watch
  python agent.py --relay ... --token ... --watch --send-keys
  python agent.py --relay ... --token ... --once --replay 20   # 回放最后 20 条审计日志
"""
import argparse
import datetime
import glob
import json
import os
import subprocess
import sys
import time
import urllib.request

SCORE_SKILL = os.path.expanduser(
    "~/.workbuddy/skills/totorosir-workbuddy-score/scripts/buddy_station.py"
)
AUDIT_DIR = os.path.expanduser("~/.workbuddy/audit-log")

PENDING_TYPES = (
    "needs-approval", "ask-matched", "needs_approval", "pending",
)
RESOLVED_TYPES = (
    "approved", "rejected", "approval-resolved", "approval-timeout",
    "deny-matched", "blocked", "resolved",
)

# 「进行中任务」推断：审计日志没有任务开始/结束事件，
# 用最近 ACTIVE_WINDOW 秒内有工具活动的不同 sessionId 数近似。
ACTIVE_WINDOW = 240.0          # 4 分钟内有写入就算「进行中」：对话文件是攒批写的，太短会闪
SESSION_KEEP = 6 * 3600.0     # 会话表保留 6 小时（跨天清空）

# WorkBuddy 每个会话一个对话文件，随每条消息追加写。mtime 就是「这个会话刚刚在干活」，
# 比审计日志可靠得多：审计日志只在触发安全检查（文件/命令/敏感）时才有记录，
# 普通的读文件、答问题根本不留痕，靠它算「进行中任务」会长期显示 0。
PROJECTS_DIR = os.path.expanduser("~/.workbuddy/projects")


def conversation_activity():
    """返回 (活跃会话数, 今日会话数)。只 stat，不读内容。"""
    now = time.time()
    midnight = datetime.datetime.combine(datetime.date.today(),
                                         datetime.time()).timestamp()
    active = today = 0
    for p in glob.iglob(os.path.join(PROJECTS_DIR, "*", "*.jsonl")):
        try:
            mt = os.path.getmtime(p)
        except OSError:
            continue
        if now - mt <= ACTIVE_WINDOW:
            active += 1
        if mt >= midnight:
            today += 1
    return active, today

TOOL_CN = {
    "command-safety": "命令",
    "file-safety": "文件",
    "network": "网络",
    "sensitive-protection": "敏感",
}


def ascii_safe(s, limit):
    """设备提示行用 FONT12（无中文），只保留 ASCII，避免画成空白。"""
    b = "".join(c if 32 <= ord(c) < 127 else "?" for c in str(s or ""))
    return b[:limit]


SHARECODE = ""


def api(relay, token, path, payload=None, timeout=12):
    url = relay.rstrip("/") + path
    if SHARECODE:
        url += ("&" if "?" in url else "?") + "sharecode=" + SHARECODE
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def account_snapshot():
    """只读账号数据；失败时返回 None，不影响事件推送。"""
    if not os.path.isfile(SCORE_SKILL):
        return None
    try:
        raw = subprocess.check_output(
            [sys.executable, SCORE_SKILL, "--json", "--no-toast", "--no-push"],
            stderr=subprocess.DEVNULL, timeout=90,
        ).decode("utf-8", "replace")
        d = json.loads(raw.strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        return None
    travel = d.get("travel") or {}
    credits = int(d.get("total_credits") or 0)
    streak = int(d.get("streak_days") or 0)
    energy = int(travel.get("energy") or 0)
    today = int(d.get("today_credit") or 0)
    return {
        "total": 1 if d.get("today_checked_in") else 0,
        "credits": credits, "streak": streak, "energy": energy,
        "tokens": credits * 50, "tokens_today": today * 50,
        "completed": bool(d.get("today_checked_in")),
        "msg": "WorkBuddy account",
        "prompt": None,
    }


def audit_events(since_seq=0, replay=0):
    """返回 (事件列表, 最大序号)。默认只读今天新增行。"""
    today = datetime.date.today().isoformat() + ".jsonl"
    path = os.path.join(AUDIT_DIR, today)
    if not os.path.isfile(path):
        return [], since_seq
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    if replay:
        lines = lines[-replay:]
    for line in lines:
        try:
            d = json.loads(line)
        except ValueError:
            continue
        seq = d.get("sequence") or 0
        if not replay and seq <= since_seq:
            continue
        et = str(d.get("eventType") or "")
        out.append((seq, d, et))
    return out, max([s for s, _, _ in out] or [since_seq])


def note_sessions(events, sessions, now):
    """从审计事件记录每个会话最后活跃时间。"""
    for _, d, _ in events:
        sid = str(d.get("sessionId") or "")
        if not sid:
            continue
        try:
            t = int(d.get("timestamp") or 0) / 1000.0
        except (TypeError, ValueError):
            t = now
        if t > now:
            t = now
        sessions[sid] = max(sessions.get(sid, 0), t)


def active_tasks(sessions, now):
    return sum(1 for t in sessions.values() if now - t <= ACTIVE_WINDOW)


def classify(et):
    if any(w in et for w in PENDING_TYPES):
        return "pending"
    if any(w in et for w in RESOLVED_TYPES):
        return "resolved"
    return None


def foreground_is_workbuddy():
    """仅 Windows；检查前景窗口标题，避免把按键送错地方。"""
    if sys.platform != "win32":
        return False
    import ctypes
    u = ctypes.windll.user32
    hwnd = u.GetForegroundWindow()
    n = u.GetWindowTextLengthW(hwnd)
    if not n:
        return False
    buf = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(hwnd, buf, n + 1)
    title = buf.value
    return ("WorkBuddy" in title) or ("CodeBuddy" in title)


def send_key(vk):
    if sys.platform != "win32":
        return False
    import ctypes
    u = ctypes.windll.user32
    u.keybd_event(vk, 0, 0, 0)             # key down
    u.keybd_event(vk, 0, 0x0002, 0)        # key up
    return True


def uia_act(approve):
    """用 UI Automation 精准点审批按钮。

    找不到按钮就什么都不做——这不是失败，是护栏：WorkBuddy 的
    `*.needs-approval` 大多是**事后审计通知**（decision=info），执行时根本没有
    弹窗在等，此时按 Enter/Esc 会把按键送到别处。只有真扫到「允许/拒绝」
    才点，扫不到就只留一行日志。
    """
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui_click.py")
    if not os.path.isfile(exe):
        return "ui_click.py missing"
    flag = "--approve" if approve else "--deny"
    try:
        p = subprocess.run([sys.executable, exe, flag],
                           capture_output=True, text=True, timeout=15)
        out = (p.stdout or "").strip().splitlines()
        return out[-1][:180] if out else "no output"
    except Exception as e:
        return "%s: %s" % (type(e).__name__, e)


def run(args):
    global SHARECODE
    SHARECODE = args.sharecode or ""
    relay = args.relay.rstrip("/")
    state = {"total": 0, "running": 0, "done": 0, "credits": 0, "streak": 0, "energy": 0,
             "tokens": 0, "tokens_today": 0, "completed": False,
             "msg": "WorkBuddy relay", "prompt": None}
    seq = 0
    cursor = 0
    pending = {}
    sessions = {}
    last_account = 0
    last_push = 0
    prev_active = 0
    first = True
    day_key = datetime.date.today()
    print("[agent] start relay=%s watch=%s account=%s push=%ss" %
          (relay, args.watch, args.account, args.push), flush=True)

    # 决策游标从服务端「已确认」处开始：关机期间设备按下的 OK/NO 仍会被处理，
    # 但历史上已经执行过的不会被重放（以前每次重启都会再发一遍 Enter/Esc）。
    try:
        got = api(relay, args.token, "/api/decisions?token=%s&after=0" % args.token)
        cursor = int(got.get("cursor", 0) or 0)
    except Exception:
        cursor = 0

    if args.replay:
        events, seq = audit_events(0, args.replay)
    else:
        # 上线时先用当天日志给「活跃会话」打底：否则刚启动那几秒一律显示 0 个任务，
        # 看起来就像"没打通"。但审批事件不回放，避免把已处理的提示重新亮起来。
        primed, seq = audit_events(0, 0)
        note_sessions(primed, sessions, time.time())
        events = []

    while True:
        try:
            if not events:
                # 只有真的读到了新事件才替换。以前是无条件 events = new：
                # --replay 读进来的那批事件（seq 已推进到末尾）会在第一次刷新时
                # 被空列表覆盖，等于 replay 永远不生效。
                new, seq_new = audit_events(seq, 0)
                if new:
                    events = new
                seq = seq_new
            # 会话表不能无限长：「任务N」会越堆越大，也不是用户理解的任务数。
            # 跨天清空，非当天只保留 6 小时内活跃的会话。
            today_key = datetime.date.today()
            if today_key != day_key:
                sessions.clear(); pending.clear(); day_key = today_key
            now_ts = time.time()
            for sid in [s for s, t in sessions.items() if now_ts - t > SESSION_KEEP]:
                sessions.pop(sid, None)
            note_sessions(events, sessions, now_ts)
            for s, d, et in events:
                kind = classify(et)
                if not kind:
                    continue
                rid = str(d.get("requestId") or "")
                tid = str(d.get("toolCallId") or "")
                key = rid + "|" + tid
                if kind == "pending":
                    cat = str(d.get("category") or "")
                    cmd = d.get("commandPreview") or (d.get("messageParams") or {}).get("command", "")
                    pending[key] = {
                        "id": (rid or tid)[:40],
                        "tool": TOOL_CN.get(cat, "操作"),
                        "hint": ascii_safe(cmd, 56),
                    }
                    print("[audit] pending %s %s" % (cat, ascii_safe(cmd, 60)))
                elif key in pending:
                    pending.pop(key, None)
                    print("[audit] resolved %s" % et)
            events = []

            # 进行中任务数 = 最近 ACTIVE_WINDOW 秒内有写入的会话文件数（真实活动信号）；
            # 项目目录不可用时退回审计日志的会话近似。
            convo_active, convo_today = conversation_activity()
            if convo_today or convo_active:
                active = convo_active
                state["total"] = max(convo_today, convo_active)
            else:
                active = active_tasks(sessions, time.time())
                state["total"] = max(len(sessions), active)
            state["running"] = active
            state["waiting"] = 1 if pending else 0
            if events:
                # 只用于串口 PET 诊断：让"最近在干什么"有个粗略标签
                state["msg"] = str(events[-1][1].get("category") or events[-1][2])[:40]
            if prev_active > 0 and active == 0:
                state["done"] = state["total"]
                state["completed"] = True
            else:
                state["completed"] = False
            prev_active = active
            pr = list(pending.values())[-1] if pending else None
            state["prompt"] = pr

            due = time.time() - last_account >= 300
            if (first or due) and args.account:
                snap = account_snapshot()
                if snap:
                    # 账号只读数据只取这几个字段，别覆盖任务数与 prompt
                    for k in ("credits", "streak", "energy", "tokens", "tokens_today"):
                        state[k] = snap.get(k, 0)
                last_account = time.time()

            if first or time.time() - last_push >= args.push or pr:
                api(relay, args.token, "/api/event", {"token": args.token, "snapshot": state})
                last_push = time.time()

            # 取回设备决策
            got = api(relay, args.token,
                      "/api/decisions?token=%s&after=%d" % (args.token, cursor))
            for dec in got.get("decisions", []):
                print("[device] decision #%d id=%s %s" %
                      (dec.get("n", 0), dec.get("id", ""), dec.get("decision", "")))
                cursor = max(cursor, int(dec.get("n", 0)))
                if dec.get("decision") == "deny":
                    if args.uia:
                        print("[uia] deny -> %s" % uia_act(False))
                    if args.send_keys and foreground_is_workbuddy():
                        send_key(0x1B)          # Esc
                        print("[keys] Esc -> cancel")
                else:
                    if args.uia:
                        print("[uia] approve -> %s" % uia_act(True))
                    if args.send_keys and foreground_is_workbuddy():
                        send_key(0x0D)          # Enter
                        print("[keys] Enter -> confirm")
                # 立刻让宠物退出待授权态，等审计日志确认后再二次刷新
                state["prompt"] = None
                state["running"] = 0
                state["waiting"] = 0
                api(relay, args.token, "/api/event", {"token": args.token, "snapshot": state})
            if got.get("decisions"):
                api(relay, args.token, "/api/ack", {"token": args.token, "seq": cursor})

            first = False
            if not args.watch:
                break
            time.sleep(args.interval)
        except KeyboardInterrupt:
            break
        except Exception as e:
            print("[agent] %s: %s" % (type(e).__name__, e), file=sys.stderr)
            if not args.watch:
                return 1
            time.sleep(3)
    api(relay, args.token, "/api/reset", {"token": args.token})
    return 0


def main():
    ap = argparse.ArgumentParser(description="WorkBuddy desktop agent for Buddy relay")
    ap.add_argument("--config", default="", help="JSON 配置文件（relay/sharecode/token/watch/...）")
    ap.add_argument("--relay", default="", help="部署后的 HTTP 服务根地址")
    ap.add_argument("--token", default="", help="与设备相同的共享密钥（不会落盘）")
    ap.add_argument("--sharecode", default="", help="WorkBuddy 分享链接的访问码")
    ap.add_argument("--watch", action="store_true", help="持续监听")
    ap.add_argument("--once", action="store_true", help="只跑一轮")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--push", type=float, default=8.0, help="无变化时也每隔多少秒补推一次")
    ap.add_argument("--account", action="store_true", help="同步账号积分/连续天数/能量")
    ap.add_argument("--audit", action="store_true", help="读取审计日志审批事件")
    ap.add_argument("--replay", type=int, default=0, help="回放最近 N 条审计日志用于调试")
    ap.add_argument("--send-keys", action="store_true", help="设备决策时模拟 Enter/Esc（需前景是 WorkBuddy）")
    ap.add_argument("--uia", action="store_true",
                    help="设备决策时用 UI Automation 精准点审批按钮；扫不到按钮就不动（推荐）")
    ap.add_argument("--log", default="", help="日志文件（开机自启无控制台时必填）")
    args = ap.parse_args()

    # 开机自启时不给任何参数：默认读同目录的 agent.config.json。
    # （计划任务里的引号很容易被吃掉，argparse 收到就 exit 2，别依赖命令行传路径）
    if not args.config:
        cand = os.path.join(os.path.dirname(os.path.abspath(__file__)), "agent.config.json")
        if os.path.isfile(cand):
            args.config = cand

    cfg = {}
    if args.config and os.path.isfile(args.config):
        try:
            with open(args.config, "r", encoding="utf-8") as f:
                cfg = json.load(f) or {}
        except (OSError, ValueError) as e:
            print("[agent] config %s: %s" % (args.config, e), file=sys.stderr)
            return 2
    for k in ("relay", "token", "sharecode", "log"):
        if k in cfg and not getattr(args, k, None):
            setattr(args, k, cfg[k])
    for k in ("interval", "push", "replay"):
        # 这几个有非零默认值，只有命令行里没显式给才吃配置文件
        if k in cfg and ("--" + k) not in sys.argv:
            setattr(args, k, cfg[k])
    for k in ("watch", "account", "audit", "send_keys", "uia", "once"):
        if cfg.get(k):
            setattr(args, k, True)

    if args.log:
        # pythonw 没有控制台，print 会直接抛异常；重定向到文件并限制体积
        if not os.path.isabs(args.log):
            args.log = os.path.join(os.path.dirname(os.path.abspath(__file__)), args.log)
        try:
            if os.path.isfile(args.log) and os.path.getsize(args.log) > 512 * 1024:
                os.replace(args.log, args.log + ".1")
            f = open(args.log, "a", buffering=1, encoding="utf-8")
            sys.stdout = f
            sys.stderr = f
        except OSError:
            pass

    if not args.relay or not args.token:
        print("[agent] need --relay and --token (or --config)", file=sys.stderr)
        return 2
    if "--once" in sys.argv:            # 命令行显式 --once 优先于配置里的 watch
        args.watch = False
    if not args.watch and not args.once:
        args.once = True
    return run(args)


if __name__ == "__main__":
    sys.exit(main())

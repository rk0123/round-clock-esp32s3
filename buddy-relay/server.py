# -*- coding: utf-8 -*-
"""WorkBuddy Buddy 推送源：桌面侧上报事件，ESP32 通过 WiFi 轮询取状态并回传决策。

单端口 HTTP 服务，仅用 Python 标准库，可直接部署到 WorkBuddy 云（沙箱提供 PORT）。
数据落在 data/state.json，不依赖外部数据库。所有读写按 token 隔离，日志不打印 token。

端点：
  GET  /health                          健康检查
  POST /api/event    {token,snapshot}   桌面侧写入最新快照（覆盖式合并，见 normalize）
  GET  /api/state?token=...             设备轮询：返回 {snapshot, seq, age}
  POST /api/decision {token,id,decision,tool,hint,at}  设备回传 OK/NO
  GET  /api/decisions?token=&after=N    桌面侧取回未读决策（游标经过确认后推进）
  POST /api/ack      {token,seq}        桌面确认已取回决策

安全说明：token 是设备与桌面之间的共享密钥，服务只做隔离与转发，不签发凭据。
不要把真实 WorkBuddy 登录态或 accessToken 送到这里。
"""
import hashlib
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DATA_DIR = os.environ.get("BUDDY_RELAY_DATA", os.path.join(os.path.dirname(__file__), "data"))
STATE_FILE = os.path.join(DATA_DIR, "state.json")
LOCK = threading.Lock()
MAX_BODY = 8192
MAX_DECISIONS = 200
MAX_SNAPSHOT_AGE = 900          # 15 分钟没更新就当离线（秒）

os.makedirs(DATA_DIR, exist_ok=True)


def load_state():
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as f:
            d = json.load(f)
            if isinstance(d, dict):
                return d
    except (OSError, ValueError):
        pass
    return {"rooms": {}, "seq": 0}


def save_state(d):
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, separators=(",", ":"))
    os.replace(tmp, STATE_FILE)


def room_key(token):
    """token 不落盘明文；用摘要做 key，避免路径穿越和日志泄露。"""
    return hashlib.sha256(token.encode("utf-8")).hexdigest()[:32]


def normalize(snap):
    """只保留设备协议认识的字段，控制体积（ESP32 静态 JSON 缓冲 1KB）。"""
    out = {}
    for k in ("total", "running", "waiting", "done", "credits", "streak", "energy",
              "tokens", "tokens_today"):
        try:
            out[k] = max(0, int(snap.get(k, 0)))
        except (TypeError, ValueError):
            out[k] = 0
    out["completed"] = bool(snap.get("completed", False))
    msg = snap.get("msg", "")
    out["msg"] = str(msg)[:40]
    pr = snap.get("prompt")
    if isinstance(pr, dict) and pr.get("id"):
        out["prompt"] = {
            "id": str(pr.get("id", ""))[:40],
            "tool": str(pr.get("tool", ""))[:20],
            "hint": str(pr.get("hint", ""))[:56],
        }
    else:
        out["prompt"] = None
    return out


class Handler(BaseHTTPRequestHandler):
    server_version = "BuddyRelay/1.0"

    def log_message(self, fmt, *args):
        # 默认日志会把 query string（含 token）打到 stdout，必须关掉
        pass

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None
        if n <= 0 or n > MAX_BODY:
            return None
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)

        if u.path == "/health":
            return self._json(200, {"ok": True, "service": "buddy-relay", "ts": int(time.time())})

        token = (q.get("token") or [""])[0]
        if not token:
            return self._json(400, {"error": "missing token"})

        with LOCK:
            st = load_state()
            room = st["rooms"].get(room_key(token))
            if not room:
                room = {"snapshot": normalize({}), "at": 0, "decisions": [], "cursor": 0}
                st["rooms"][room_key(token)] = room

            if u.path == "/api/state":
                age = int(time.time()) - room.get("at", 0)
                # 超过上限：返回一个没连接的空快照，宠物回到睡眠态
                snap = room["snapshot"] if age <= MAX_SNAPSHOT_AGE else normalize({})
                return self._json(200, {
                    "snapshot": snap,
                    "seq": st["seq"],
                    "age": age,
                    "ok": True,
                })

            if u.path == "/api/decisions":
                after = 0
                try:
                    after = int((q.get("after") or ["0"])[0])
                except ValueError:
                    pass
                items = [d for d in room["decisions"] if d.get("n", 0) > after]
                return self._json(200, {
                    "decisions": items[:20],
                    "cursor": room.get("cursor", 0),
                    "count": len(items),
                    "ok": True,
                })

        return self._json(404, {"error": "not found"})

    def do_POST(self):
        u = urlparse(self.path)
        body = self._body()
        if not isinstance(body, dict):
            return self._json(400, {"error": "bad json"})
        token = body.get("token", "")
        if not isinstance(token, str) or not token:
            return self._json(400, {"error": "missing token"})

        with LOCK:
            st = load_state()
            key = room_key(token)
            room = st["rooms"].get(key)
            if not room:
                room = {"snapshot": normalize({}), "at": 0, "decisions": [], "cursor": 0}
                st["rooms"][key] = room

            if u.path == "/api/event":
                snap = body.get("snapshot")
                if not isinstance(snap, dict):
                    return self._json(400, {"error": "bad snapshot"})
                room["snapshot"] = normalize(snap)
                room["at"] = int(time.time())
                save_state(st)
                return self._json(200, {"ok": True, "seq": st["seq"], "age": 0})

            if u.path == "/api/decision":
                st["seq"] += 1
                item = {
                    "n": st["seq"],
                    "id": str(body.get("id", ""))[:40],
                    "decision": "deny" if body.get("decision") == "deny" else "once",
                    "tool": str(body.get("tool", ""))[:20],
                    "hint": str(body.get("hint", ""))[:56],
                    "at": int(time.time()),
                }
                room["decisions"].append(item)
                room["decisions"] = room["decisions"][-MAX_DECISIONS:]
                save_state(st)
                return self._json(200, {"ok": True, "n": item["n"]})

            if u.path == "/api/ack":
                try:
                    seq = int(body.get("seq", 0))
                except (TypeError, ValueError):
                    seq = 0
                room["cursor"] = max(room.get("cursor", 0), seq)
                save_state(st)
                return self._json(200, {"ok": True, "cursor": room["cursor"]})

            if u.path == "/api/reset":
                # 桌面侧收工：清掉待授权提示，避免设备一直亮着 ATTENTION
                room["snapshot"]["prompt"] = None
                room["snapshot"]["running"] = 0
                room["snapshot"]["waiting"] = 0
                room["at"] = int(time.time())
                save_state(st)
                return self._json(200, {"ok": True})

        return self._json(404, {"error": "not found"})


def main():
    port = int(os.environ.get("PORT", "3000"))
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print("buddy-relay listening on port %d" % port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()

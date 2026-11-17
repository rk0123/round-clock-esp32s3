# -*- coding: utf-8 -*-
"""单连接串口探针：发一串命令，把期间所有日志完整收下来。

反复 Serial(port, ...) 会让 ESP32-S3 原生 USB-CDC 复位甚至丢失 COM 口，
所以这里复用 capture_frame.open_port()（DTR/RTS 先置 False 再 open），
并且所有命令都在同一个连接里发完。

用法：
    python serial_probe.py COM12 "STATUS" "AUD" "WAIT:5" "AUD"
    python serial_probe.py COM12 "PLAY" "WAIT:60" "AUD" --grep "AUD|NET|POD|TF"
    python serial_probe.py COM12 "STATUS" --out firmware/tools/probe.log

参数里 "WAIT:n" 表示等待 n 秒（不发送任何东西）。
"""
import sys
import time
import re
from capture_frame import open_port

PY = sys.executable  # 直接用当前解释器，换机器不用改


def main():
    args = sys.argv[1:]
    port = args.pop(0) if args else "COM12"
    grep = None
    out = None
    if "--grep" in args:
        i = args.index("--grep")
        grep = re.compile(args[i + 1], re.I)
        del args[i:i + 2]
    if "--out" in args:
        i = args.index("--out")
        out = args[i + 1]
        del args[i:i + 2]

    s = open_port(port)
    time.sleep(1.2)
    try:
        s.reset_input_buffer()
    except Exception:
        pass
    buf = []

    def drain(seconds):
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                d = s.read(4096)
            except Exception as e:
                buf.append("[PY] read error %r\n" % (e,))
                break
            if d:
                buf.append(d.decode("utf-8", "replace"))
            else:
                time.sleep(0.02)

    drain(0.3)
    for a in args:
        if a.startswith("WAIT:"):
            drain(float(a[5:]))
            continue
        try:
            s.write((a + "\n").encode())
            s.flush()
        except Exception as e:
            buf.append("[PY] write error %r\n" % (e,))
            break
        buf.append("\n>>> %s\n" % a)
        drain(0.6)
    drain(0.5)
    try:
        s.close()
    except Exception:
        pass

    text = "".join(buf)
    if out:
        with open(out, "w", encoding="utf-8", errors="replace") as f:
            f.write(text)
        print("log saved:", out)
    lines = text.splitlines()
    if grep:
        lines = [l for l in lines if grep.search(l) or l.startswith(">>> ") or l.startswith("[PY]")]
    for l in lines:
        print(l)


if __name__ == "__main__":
    main()

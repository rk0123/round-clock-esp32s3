# -*- coding: utf-8 -*-
"""带污染校验的真机抓图。

SHOT 是把整个帧缓冲原样打进串口，如果这期间 netTask / audioTask 正好打印一行日志，
字节流就会多出几十个字节 —— PC 端按 259200 解码后整幅图向右/向左挪，看起来像
"表盘被切成两半 / 图像错位"。固件侧已让 SHOT 期间暂停网络任务，这里再加一道校验：
读完帧数据后必须紧跟 ===FB_END===，否则丢弃重抓。

用法：
    python capture_frame.py COM12 shot.png            # 抓当前页
    python capture_frame.py COM12 shot.png 3          # 先 PAGE 3 再抓
    python capture_frame.py COM12 shot.png 3 "TAP 180 180"
"""
import sys
import time
import serial
from PIL import Image

W = H = 360
FBSZ = W * H * 2


def open_port(port, retries=40):
    """静默打开串口：**必须在 open() 之前**就把 DTR/RTS 设成 False。

    pyserial 的 Serial(port, ...) 是「构造即 open」，open 内部默认把 DTR/RTS 拉高，
    ESP32-S3 原生 USB-CDC 会当场复位；DTR/RTS 某些组合还会把芯片带进下载模式。
    反复开关几次之后 COM12 会直接从系统里消失（只能物理拔插 USB）。
    这里改成先赋值、后 open，并把异常重试做在同一个对象上。
    """
    last = None
    for _ in range(retries):
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.3
        s.dtr = False          # 关键：赋值发生在 open() 之前
        s.rts = False
        try:
            s.open()
            time.sleep(0.8)
            s.reset_input_buffer()
            return s
        except Exception as e:
            last = e
            try:
                s.close()
            except Exception:
                pass
            time.sleep(0.5)
    raise SystemExit("cannot open %s (%s)" % (port, last))


def send(s, line):
    for _ in range(5):
        try:
            s.write((line + "\n").encode())
            s.flush()
            return s
        except Exception:
            s.close()
            time.sleep(1.0)
            s = open_port(s.port)
    raise SystemExit("write failed")


def grab(s, tries=5):
    """返回干净的 RGB565 帧数据，失败返回 None"""
    for attempt in range(1, tries + 1):
        b = b""
        send(s, "SHOT")
        t0 = time.time()
        while b"===FB_BEGIN===" not in b:
            d = s.read(65536)
            if d:
                b += d
                t0 = time.time()
            elif time.time() - t0 > 10:
                break
        if b"===FB_BEGIN===" not in b:
            print("  抓帧超时，重试 %d" % attempt)
            continue
        i = b.index(b"===FB_BEGIN===") + len(b"===FB_BEGIN===\n")
        while len(b) - i < FBSZ:
            d = s.read(65536)
            if d:
                b += d
                t0 = time.time()
            elif time.time() - t0 > 10:
                break
        if len(b) - i < FBSZ:
            print("  数据不足，重试 %d" % attempt)
            continue
        fb = b[i:i + FBSZ]
        # 校验尾部标记：紧跟帧数据的必须是 ===FB_END===
        tail = b[i + FBSZ:]
        t1 = time.time()
        while b"===FB_END===" not in tail and len(tail) < 64 and time.time() - t1 < 3:
            d = s.read(64)
            if d:
                tail += d
        if b"===FB_END===" in tail:
            return fb
        print("  帧流被日志污染（尾部=%r），重试 %d" % (tail[:40], attempt))
    return None


def save(fb, path):
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        r0 = y * W * 2
        for x in range(W):
            v = (fb[r0 + x * 2] << 8) | fb[r0 + x * 2 + 1]   # 大端（LCD_addWindow 已 bswap）
            px[x, y] = (((v >> 11) & 0x1F) * 255 // 31,
                        ((v >> 5) & 0x3F) * 255 // 63,
                        (v & 0x1F) * 255 // 31)
    img.save(path)
    print("saved", path)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM12"
    out = sys.argv[2] if len(sys.argv) > 2 else "preview/frame.png"
    tgt = sys.argv[3] if len(sys.argv) > 3 else None
    extra = sys.argv[4] if len(sys.argv) > 4 else None

    s = open_port(port)
    time.sleep(1.5)
    if tgt is not None:
        send(s, "PAGE " + tgt)
        time.sleep(0.6)
    if extra:
        send(s, extra)
        time.sleep(0.6)
    fb = grab(s)
    s.close()
    if fb is None:
        raise SystemExit("抓帧失败：多次重试仍被污染")
    save(fb, out)


if __name__ == "__main__":
    main()

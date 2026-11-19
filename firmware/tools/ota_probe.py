"""PC 侧探针：用设备的 Device-Id / Client-Id 查 OTA，拿 websocket 配置，再试握手。

用法：
  python ota_probe.py ota <mac>                     # 只查配置
  python ota_probe.py ws opus <url> <token> <mac>   # 用 opus 握手
  python ota_probe.py ws pcm  <url> <token> <mac>

mac 传设备自己的地址（串口 STATUS 或 esptool read_mac 能看到），不要写死在文件里：
它是 xiaozhi 的设备身份，公开出去等于把别人设备的身份牌贴到网上。
"""
import json
import sys
import time
import uuid as uuidlib

import requests

MAC = ""          # 运行时由命令行传入
OTA = "https://api.tenclass.net/xiaozhi/ota/"


def device_body(client_id: str) -> dict:
    return {
        "version": 2,
        "language": "zh-CN",
        "flash_size": 16777216,
        "minimum_free_heap_size": "65536",
        "mac_address": MAC,
        "uuid": client_id,
        "chip_model_name": "esp32s3",
        "chip_info": {"model": 9, "cores": 2, "revision": 2, "features": 18},
        "application": {
            "name": "roundclock",
            "version": "2.7",
            "compile_time": "Nov 18 2026T15:53:00Z",
            "idf_version": "v5.1.4",
            "elf_sha256": "0" * 64,
        },
        "display": {"width": 360, "height": 360, "monochrome": False},
        "board": {
            "type": "esp32-s3-touch-lcd-1.85b",
            "name": "esp32-s3-touch-lcd-1.85b",
            "manufacturer": "waveshare",
            "rssi": -55,
        },
    }


def query_ota(client_id: str):
    headers = {
        "Content-Type": "application/json",
        "Device-Id": MAC,
        "Client-Id": client_id,
        "Accept-Language": "zh-CN",
        "Activation-Version": "1",
        "User-Agent": "esp32-s3-touch-lcd-1.85b/2.7",
    }
    r = requests.post(OTA, headers=headers, data=json.dumps(device_body(client_id)), timeout=15)
    print("HTTP", r.status_code)
    print(json.dumps(r.json(), ensure_ascii=False, indent=2))
    return r.json()


def ws_hello(url: str, token: str, fmt: str, client_id: str):
    import websockets.sync.client as wsc

    headers = {
        "Authorization": f"Bearer {token}",
        "Protocol-Version": "1",
        "Device-Id": MAC,
        "Client-Id": client_id,
    }
    hello = {
        "type": "hello",
        "version": 1,
        "transport": "websocket",
        "audio_params": {"format": fmt, "sample_rate": 16000, "channels": 1, "frame_duration": 60},
    }
    with wsc.connect(url, additional_headers=headers, open_timeout=15, close_timeout=5) as ws:
        ws.send(json.dumps(hello))
        for _ in range(6):
            try:
                msg = ws.recv(timeout=8)
            except Exception as exc:  # noqa: BLE001
                print("recv err:", type(exc).__name__, exc)
                break
            if isinstance(msg, bytes):
                print("BIN", len(msg), msg[:16].hex())
                continue
            print("TXT", msg)
            data = json.loads(msg)
            if data.get("audio_params"):
                print(">>> server audio_params:", data["audio_params"])
                break
        ws.send(json.dumps({"type": "listen", "state": "start", "mode": "manual"}))
        time.sleep(1.0)
        try:
            print("after listen:", ws.recv(timeout=5))
        except Exception as exc:  # noqa: BLE001
            print("after listen err:", type(exc).__name__)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(1)
    if sys.argv[1] == "ota":
        if len(sys.argv) < 3:
            print("用法: python ota_probe.py ota <mac>")
            raise SystemExit(1)
        MAC = sys.argv[2]
        cid = sys.argv[3] if len(sys.argv) > 3 else str(uuidlib.uuid4())
        print("client_id =", cid)
        query_ota(cid)
    elif sys.argv[1] == "ws":
        if len(sys.argv) < 6:
            print("用法: python ota_probe.py ws <opus|pcm> <url> <token> <mac>")
            raise SystemExit(1)
        fmt = sys.argv[2]
        MAC = sys.argv[5]
        ws_hello(sys.argv[3], sys.argv[4], fmt,
                 sys.argv[6] if len(sys.argv) > 6 else str(uuidlib.uuid4()))
    else:
        print(__doc__)
        raise SystemExit(1)

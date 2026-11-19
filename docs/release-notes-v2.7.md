# v2.7 · 小智语音改成真协议

这一版只做一件事，但改动面很大：**把"小智问答"从自己拼的百炼链路换成真正的 xiaozhi 协议**，
并且做成连续对话 + 语音回复。

## 之前是什么

v2.6.x 的"小智"其实是三段拼起来的：录音 → 百炼 Paraformer 转文字 → Qwen 回答文字 → 显示在屏上。
协议、鉴权、设备身份都是我们自己编的，`api.tenclass.net` 那边只是拿到了绑定状态，一句话都没走通。
所以它不叫小智，叫"云端问答"更准确。

## 现在是什么

```
        ┌──────── WebSocket wss://api.tenclass.net/xiaozhi/v1/ ────────┐
设备 ───┤ 上行: 16 kHz Opus, 60 ms/帧, 二进制帧                         │
        │ 下行: 24 kHz Opus 音频 + JSON 文本帧（stt / tts / llm）        │
        └───────────────────────────────────────────────────────────────┘
```

- **握手**：`Authorization: Bearer <token>` + `Protocol-Version` / `Device-Id` / `Client-Id`，
  自己算 nonce、校验 `Sec-WebSocket-Accept`，支持分片重组与 ping/pong。
- **连续对话**：本地能量 VAD 断句（≥4 帧持续超阈值才算开口，静音 900 ms 收尾）→ `listen stop`
  → 等 `tts stop` → **自动重新开麦**。不用每次按键，再点一下是插话。
- **出声**：`opus_decode` → 线插值上采样到 48 k → 立体声交织 → 直写 I²S TX，边收边播。
- **设备绑定**：OTA 接口拿 `websocket.url/token`，eFuse HMAC 证明设备身份。绑定一次就够，
  token 存 NVS。

## Opus 从哪来

xiaozhi 协议两端都是 Opus。板子上原本只有 `ESP32-audioI2S` 自带的一个解码器：

- 只有解码，没有编码 → 上行做不到；
- 它的解码器和上游 libopus 有 **10 个同名的全局查表**，直接引入上游源码会 `multiple definition`。

所以 `tools/vendor_opus.py` 从 libopus 1.3.1 官方 tarball 裁出定点编解码子树到 `firmware/src/opus`，
脚本负责四件事：生成 `config.h`、把 664 处 `#include "x.h"` 改成相对路径、给每个 `.c` 头部插
`config.h`、把那 10 个冲突符号加 `rcz_` 前缀（库那边一个字节都不用改）。

Arduino 会递归编译 `src/`，所以不需要额外装库。许可与重新生成方式见
`firmware/src/opus/README-LICENSE.txt`。

## 顺带修掉的五个硬 bug

这些不是"完善"，是踩到就必须修的：

1. **会话任务永远不启动** —— `xzChatTask` 的启动条件同时判 `chat == OFF`，而 toggle 已经先把状态
   置成 `CONNECTING`，于是条件永不成立。
2. **任务栈放 PSRAM 会随机重启** —— 任务里只要碰 NVS / flash（`Preferences`、`nvs_get_*`、OTA 分区读），
   就会在 `spi_flash_disable_interrupts_caches` 里断言 `esp_task_stack_is_sane_cache_disabled`。
   最终方案是 **PSRAM 32 KB 栈 + 连接参数在主循环里预缓存**，会话任务不读一个字节的 flash。
3. **TLS 握手内存不足** —— mbedtls 要两块 16 KB 连续内部堆，开机时天气 / ADSB 的 TLS 还没退。
   加 4 次带退避的重试，并在失败日志里打 `int/blk/dma/ps` 四项堆数据。
4. **I²S TX 是 16 bit 立体声** —— 单声道数据直写会让每个声道只拿到一半样本，声音变慢、音调低八度。
   必须左右各写一份。
5. **内部 RAM 被曲库表挤爆** —— 三张曲库（3×40×448 B = 52.5 KB）在 `.bss`，语音会话一开
   （TLS + ES7210 RX DMA）就爆，LCD 直接 `spi transmit (queue) color failed` 卡死。
   改 `ps_malloc` 上 PSRAM，内部静态内存 131 KB → 77 KB。

## 实机验收

用 `XZTEST`（合成音代替麦克风，专门验证"服务器下行 TTS 到底有没有出声"）跑通：

```
[XZ] hello sid=eb7b367e rate=24000
[XZ] rx [stt] {"text":"我。"}
[XZ] rx [tts] {"state":"sentence_start","text":"怎么啦，就一个字，我在听喔。"}
[XZ] audio #1 dec=1440 out=5760 wrote=5760
[XZ] answer finished=1 speaking=1 audio pkt=63 frames=181440   ← 3.8 秒语音真的写进 I²S
[XZ] listen start …                                            ← 自动进下一轮
```

连续多轮稳定：`spi transmit` 错误 0，无 assert，无栈溢出，会话中内部堆仍有 80 KB。

## 界面

小智页重做成四段：状态行（连接 / 听你说 / 想一想 / 小智在说）→ `你` 区 → `小智` 区 → 主键。
听的时候状态行下面有一条橙色电平条；会话进行中隐藏「绑定」键，免得压住多行答复；
显示前会剪掉首尾孤立标点（服务端会单独回一个「。」）；服务器发来的 `% tool_call` 文本直接丢弃，
不显示也不念。

## 诚实的保留

`XZTEST` 用合成音，只证明了**链路通、下行有声**，没证明**真人说话的识别准确率**。
VAD 现在是 `noise × 1.5` 阈值、900 ms 断句；如果环境吵、经常抢话或者断得太早，改这两个数就够了。

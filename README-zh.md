# RoundClock v2.6.1 语音和小智绑定修复

目标板卡为 Waveshare ESP32-S3-Touch-LCD-1.85B。构建采用 Arduino-ESP32 3.0.7、ArduinoJson 7.4.2、ESP32-audioI2S 3.4.0、16 MB Flash 和 OPI PSRAM。修复基于原 ZIP 中的 `firmware/`。

## 修改内容

- 小智问答、语音备忘和录音纪要共用同一条百炼 Paraformer 转写链路。固件等待 `task-started`，发送 16 kHz 单声道 PCM 二进制帧，并在 `task-finished` 后提交完整句子。鉴权、连接、超时和无有效人声在串口有不同的诊断。
- 音频播放和麦克风共用 48 kHz I²S 时钟，MCLK 统一为 12.288 MHz。停止录音时会先等待采音任务收尾。录音机在选择「纪要」时校验从 TF 卡载入的 WAV。
- 小智问答页新增「绑定」。固件用板卡 MAC 和保存在 NVS 的 Client-Id 向小智平台请求绑定码，并轮询绑定结果。支持普通板卡的 v1 流程，也可用已经配置了 eFuse 序列号和 HMAC Key0 的板卡走 v2 流程。
- 问答、备忘提取和纪要生成使用设备上的百炼 Key。小智平台账号绑定走独立的设备激活接口。

## 烧录

`flash/` 是本次编译生成的烧录文件，适用于以上板卡及原工程的 16 MB `app5M_fat7M_16MB` 分区。此前使用其他分区方案时，请先核对分区并备份设备数据。

在 Windows 打开终端，进入解压后的 `flash`。把示例的 `COM12` 改成设备实际串口，烧录前关闭串口监视器。

```bat
py -m esptool --chip esp32s3 --port COM12 --baud 460800 write_flash -z --flash_mode keep --flash_freq keep --flash_size keep 0x0 firmware.ino.bootloader.bin 0x8000 firmware.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.ino.bin
```

此命令与工程原烧录地址一致，保留同一分区布局下的 NVS 和 TF 卡录音。若电脑缺少 esptool，可用 `py -m pip install esptool==4.6` 安装。

若需要从源码重编，使用原工程的 `firmware\build.bat` 或 `flash.bat COM12`。Arduino-ESP32 3.0.7 需要安装 `firmware/partitions/` 中的自定义分区文件。使用干净的 ESP32-audioI2S 3.4.0，从 Arduino 库目录按顺序执行两个补丁。

```sh
patch -p1 < /path/to/firmware/patches/ESP32-audioI2S-3.4.0-roundclock.patch
patch -p1 < /path/to/firmware/patches/ESP32-audioI2S-3.4.0-fullduplex.patch
```

## 上板验收

| 操作 | 观察点 |
| --- | --- |
| 联网后运行 `BAILIAN` 和 `QWEN 请回复你好` | `BAILIAN` 回显 `SET`，Qwen 返回文本。需要配置时，在本机串口输入 `BAILIAN <中国内地百炼 API Key>`。 |
| 说中文时运行 `MICTEST 3` | 三秒约 48,000 个样本，`peak` 和 `rms` 有响应。若 `samples=0`，保留 `[MIC]` 日志以定位 I²S 或 codec 问题。 |
| 小智问答页点「说话」，说完点「停止」 | 串口有 `[ASR] task-started` 和 `task-finished`，页面显示转写和问答。串口 `AI` 可查看转写。 |
| 备忘中说「明天下午三点开会」，点「停止」 | 显示转写并添加待办。百炼出错时，页面显示错误状态。 |
| 录音机录一段真人人声，保存后先回放，再点「纪要」 | 回放清楚，串口 `[REC] loaded` 有样本数，随后生成纪要。`RECMIN 1` 可验证列表第一段。 |
| 小智问答页点「绑定」，或发送 `XZBIND` | 屏上显示六位码后，在 [xiaozhi.me](https://xiaozhi.me/) 的账号界面完成绑定。用 `XZSTATUS` 查看 `WAITING` 或 `BOUND`。过期可点「查询」刷新。 |

开机联网后等串口出现 `[TIME] SNTP`，固件使用系统时间校验 HTTPS 证书。遇到问题时，保留从触发到报错的 `[MIC]`、`[WS]`、`[ASR]`、`[Qwen]` 或 `[XZ]` 日志。分享日志前遮盖激活码、密钥和私人转写内容。

## 验证范围

完整固件已通过目标板卡配置的交叉编译。模拟服务端测试覆盖二进制 PCM 掩码、WebSocket 分片及 Ping/Pong、转写成功与失败事件、小智绑定等待及成功状态、绑定码过期和离线。烧录文件已校验。实机录音音质、真实百炼 Key 转写和小智账号绑定有待上板验证。

- 百炼协议参考 [交互流程](https://help.aliyun.com/zh/model-studio/websocket-for-paraformer-real-time-service) 和 [服务端事件](https://help.aliyun.com/zh/model-studio/paraformer-server-events)。
- 小智设备绑定参考 [官方固件 OTA 与设备激活协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/ota.md)。

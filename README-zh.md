# RoundClock v2.6.2 固件及烧录说明

目标板卡为 Waveshare ESP32-S3-Touch-LCD-1.85B。构建采用 Arduino-ESP32 3.0.7、ArduinoJson 7.4.2、ESP32-audioI2S 3.4.0、16 MB Flash 和 OPI PSRAM。此版本基于 v2.6.1。

## 本次修改

- 录音编号来自完整目录，列表显示最新 24 段。保存分批写入临时文件，完成后生成 WAV。纪要在网络任务中分块读取 WAV。
- 采音前由播放任务确认释放共享 I²S。小智问答与备忘直接交接 PCM 缓冲，云端处理期间可点「取消」或输入 `AICANCEL`。
- Wi-Fi 重连由主循环推进。HTTPS 任务优先处理小智绑定和语音，天气与雷达在交互请求间刷新。
- 表盘、待办和 Qwen 使用同一个系统时间。待办按本地日期显示，旧数据在下次成功修改时迁移到单条 NVS 快照。
- 彩云 Token 在 `SHOW` 中只显示配置状态。天气、雷达、百炼和小智绑定使用 SDK 根证书库。

模块关系和问题记录见 [固件模块关系](docs/firmware-integration.md)。

## 烧录

`flash/` 是本次编译生成的烧录文件，适用于以上板卡及原工程的 16 MB `app5M_fat7M_16MB` 分区。此前使用其他分区方案时，请先核对分区并备份设备数据。

在 Windows 打开终端，进入解压后的 `flash`。把示例的 `COM12` 改成设备实际串口，烧录前关闭串口监视器。

```bat
py -m esptool --chip esp32s3 --port COM12 --baud 460800 write_flash -z --flash_mode keep --flash_freq keep --flash_size keep 0x0 firmware.ino.bootloader.bin 0x8000 firmware.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.ino.bin
```

此命令与工程原烧录地址一致，保留同一分区布局下的 NVS 和 TF 卡录音。若电脑缺少 esptool，可用 `py -m pip install esptool==4.6` 安装。

若需要从源码重编，Windows 使用 `firmware\build.bat` 或 `flash.bat COM12`。脚本可从 PATH 选择 Arduino CLI，也接受环境变量 `ARDUINO_CLI` 指定完整可执行文件路径。Linux 和 macOS 可运行 `bash firmware/tools/build.sh`。
Arduino-ESP32 3.0.7 需要安装 `firmware/partitions/` 中的自定义分区文件。使用干净的 ESP32-audioI2S 3.4.0，从 Arduino 库目录按顺序执行两个补丁。

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
| 录音机录一段真人人声，保存后先回放，再点「纪要」 | 回放清楚，串口 `[REC] saved` 显示路径，随后生成纪要。`RECMIN 1` 可验证列表第一段。 |
| 小智问答页点「绑定」，或发送 `XZBIND` | 屏上显示六位码后，在 [xiaozhi.me](https://xiaozhi.me/) 的账号界面完成绑定。用 `XZSTATUS` 查看 `WAITING` 或 `BOUND`。过期可点「查询」刷新。 |

开机后可用 `STATUS` 查看时间来源。RTC、上次保存的时间和编译时间按顺序作为离线来源，联网对时后串口出现 `[TIME] SNTP`。Arduino-ESP32 3.0.7 的默认 SDK 启用了根证书库，证书日期检查配置为关闭。遇到问题时，保留从触发到报错的 `[MIC]`、`[WS]`、`[ASR]`、`[Qwen]` 或 `[XZ]` 日志。分享日志前遮盖激活码、密钥和私人转写内容。

## 验证范围

完整固件已通过目标板卡配置的交叉编译和烧录文件校验。离线回归覆盖 25 段以上录音的编号、临时文件冲突、分批写入失败、跨午夜待办、旧 NVS 迁移和批量保存。实机上建议依次验收录音、回放、纪要、播放切采音，以及断开网络后恢复连接。

- 百炼协议参考 [交互流程](https://help.aliyun.com/zh/model-studio/websocket-for-paraformer-real-time-service) 和 [服务端事件](https://help.aliyun.com/zh/model-studio/paraformer-server-events)。
- 小智设备绑定参考 [官方固件 OTA 与设备激活协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/ota.md)。

# buddy-relay · 桌面伙伴的推送源

把本机 WorkBuddy 的活动推到云端中转，设备（圆屏）用 WiFi 每 5 秒拉一次状态。
桌面侧是一个常驻的 `agent.py`，云端是这个目录里的 `server.py`。

## 数据从哪来（都是本机只读）

| 信号 | 来源 | 用途 |
|---|---|---|
| 进行中任务数 | `~/.workbuddy/projects/*/*.jsonl` 的 **mtime**（每个会话一个对话文件，随消息追加写） | 状态行「进行N」、忙碌/空闲 |
| 今日任务数 | 同一批文件里 mtime 落在今天的 | 空闲时的「任务N」 |
| 待授权 | `~/.workbuddy/audit-log/<今天>.jsonl` 的 `needs-approval` / `approved` 等事件 | 待授权态 + 设备 OK/NO |
| 积分 / 连续天数 / 能量 | 积分助手技能 `--json`（每 5 分钟一次） | 信息栏 |

**为什么不只用审计日志**：审计日志只在触发安全检查（命令/文件/敏感）时才写，
普通的读文件、聊天根本不落盘，靠它算「进行中」会长期显示 0 个任务——那正是
「宠物和 WorkBuddy 状态毫不相干」的根因。

## 为什么必须常驻

`agent.py` 不跑，中转里就只有一份过期的空快照（15 分钟后服务端也会主动返回空），
设备上就永远是「实时 空闲 任务0」。所以它被注册成**登录自启的计划任务**：

```
任务名  WorkBuddyBuddyAgent
动作    pythonw.exe agent.py      （无参数，自动读同目录 agent.config.json）
目录    <本目录>
触发    用户登录时；失败重启 3 次，单次间隔 1 分钟；无执行时长限制
```

管理方式：

```powershell
Start-ScheduledTask  -TaskName WorkBuddyBuddyAgent     # 手动启动
Stop-ScheduledTask   -TaskName WorkBuddyBuddyAgent     # 停止
Unregister-ScheduledTask -TaskName WorkBuddyBuddyAgent -Confirm:$false   # 彻底删掉
```

日志写在 `agent.log`（`pythonw` 没有控制台，所以必须落文件；超过 512KB 自动滚动为 `agent.log.1`）。

## 配置

`agent.config.json`：

```json
{
  "relay": "https://<部署域名>",
  "sharecode": "<分享链接的访问码>",
  "token": "<设备与中转的共享密钥>",
  "watch": true, "account": true, "audit": true,
  "interval": 3.0, "push": 10.0, "log": "agent.log"
}
```

`token` 不是 WorkBuddy 凭据，只是设备和中转之间的房间号；服务端只存它的 sha256。

## 已知边界

- 「进行中任务」是**近似**：用会话文件 4 分钟内有写入来判断，不是 WorkBuddy 自己报的任务数。
- 设备上的「同意/拒绝」会回传到中转。要让桌面**真的点头**，用 `--uia`（推荐）或 `--send-keys`：
  - `--uia`：用 Windows UI Automation 扫描 WorkBuddy 的控件树，**只有真的存在**名字等于
    「允许 / 同意 / 允许一次」这类按钮时才点，扫不到就什么都不做。已验证点击链路有效
    （点「对话内搜索」后 Edit 控件 1→2），并且**永不点「始终允许 / 永久允许」**——
    设备只能代表这一次，授权范围不该被一个屏幕按键放大。
  - `--send-keys`：盲发 Enter / Esc，依赖前台焦点，容易按错地方，默认关。
  - 真正卡点：`*.needs-approval` 事件的 `decision` 字段是 `info`，多数是**事后审计通知**，
    执行时并没有弹窗在等；只有 `sensitive-protection` 那类才会真的等人确认。
    所以「扫不到按钮」是常态，不是故障。
  - 官方没有给外部设备批准权限的 API，以上都是在客户端代你操作。
- 中转只做中转：不存 WorkBuddy 登录态，不读会话内容，只 stat 文件。

## 工具

| 脚本 | 用途 |
|---|---|
| `agent.py` | 常驻代理：推快照、取回 OK/NO |
| `server.py` | 中转服务（部署在云上） |
| `ui_click.py` | 按名字点按钮；`--self-test` 验证点击链路，`--approve/--deny` 供代理调用 |
| `ui_probe.py` | 排障用：dump WorkBuddy 的 UIA 控件树 |


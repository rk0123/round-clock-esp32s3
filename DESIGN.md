# ESP32-S3-Touch-LCD-1.85B 圆形桌面终端 · 设计稿 v0.5

> 功能：机械 / 电子双表盘（含天气）、网络收音机、航班雷达、番茄钟。定位是**演示性桌面终端**，不是纯实用工具。
> 历史版本重心是数据源：框架怎么选都行，但源选错整套东西就是摆设。以下实测结论均为 2026-11-14/15 在本机网络真实请求得到，不是抄文档。
> v0.3：新增天气链路（Open-Meteo 实测 + WMO 码映射），按 1.85" 屏物理毫米重排字号。
> **v0.4（本版）**：天气主源改**彩云天气 v2.6**（用户指定）；刷新全面下调（航班 15 s → **60 s**）；
> 雷达默认半径放大到 **250 nm ≈ 463 km** 并加**演示目标兜底**，保证屏幕永远有飞机；音乐页重做成
> 极简大按键 + 整屏黑胶转动；新增**番茄钟**页。更重要的是：**固件已写完、编译通过、烧到 COM12 实机，
> 五页帧缓冲逐页渲染验收通过**，并接入 ES8311 真实音频播放与 NVS 串口配置。见第 9 节。
> **v0.5（本版，2026-11-15 续）**：把「页面能画出来」推进到「功能真的联网跑起来」——
> 设备端配网 AP（无需电脑串口）、**免 token 的 Open-Meteo 真实天气兜底**、修掉航班距离单位 bug
> 后拿到 10 架实时飞机、SNTP 完整年月日、离线 WAV 音频自测；
> 字体重做为 4-bit 抗锯齿大数字并让冒号精确落在圆心。详见第 10 节。
> 参考项目：[delphicchen/esp32_flight_radar](https://github.com/delphicchen/esp32_flight_radar)（ESPHome + LVGL 8 的大屏航班雷达）。
> 可交互 UI 原型：`round-clock-prototype.html`（浏览器 1:1 模拟）；**可烧录固件**：`firmware/`，用法见 `firmware/README.md`；
> 实机五页截图：`firmware/preview/`。

---

## 0. 结论先行

| 功能 | 主数据源 | 兜底 | 判定 |
|---|---|---|---|
| 航班雷达 | **adsb.lol** `/v2/point`（免密钥、无日配额） | OpenSky（OAuth2）→ 本地缓存 | ✅ 实测可用，国内有数据 |
| 网络收音机 | **内置校验过的 MP3 直链**（蜻蜓 FM CDN 优先） | radio-browser 目录仅用于发现，不作运行时依赖 | ✅ 实测 200 / audio/mpeg |
| 播客 | **配置端预解析的精简 JSON** + CDN `Range` 续播 | 本地 SD 缓存 | ✅ Range 206 已验证；RSS 绝不能让设备解析 |
| 时间 | **板载 PCF85063 RTC** + SNTP 校时 | 断网照走 | ✅ 不需要任何外部 API |
| **天气** | **彩云天气 v2.6**（用户指定，`/v2.6/{token}/{lon},{lat}/weather`） | 内置演示天气 + Open-Meteo 作离线旁证 | ⚠️ 需**自带 token**；官方 demo token 实测 429 |
| **航班演示兜底** | 实时目标 < 5 架时用内置演示目标补足 | 断网全演示 | ✅ 屏幕上永远有飞机 |
| ~~airplanes.live~~ | — | — | ❌ 实测 **403**，需邮件申请授权，已从兜底链剔除 |
| ~~weather.com.cn / wttr.in~~ | — | — | ❌ 前者 301 变 HTML 首页，后者 30 KB + 15 s 超时 |

**一句话**：航班用 adsb.lol（大半径 + 演示兜底），天气用彩云（token 自备），电台用国内 MP3 直链，
播客必须有人在设备外面把 RSS 嚼碎。天气坐标**直接复用雷达中心经纬度**，不需要额外配置。
**v0.4 的定位是演示性终端，不是纯实用工具**——所以「屏幕上永远有内容」优先于「数据 100% 实时」。

---

## 1. 实测记录

### 1.1 航班数据

`GET https://api.adsb.lol/v2/point/{lat}/{lon}/{radius_nm}`，半径单位是**海里**（上限 250 nm ≈ 463 km）。

| 坐标 / 半径 | HTTP | 响应 | 机数 | 样本呼号 |
|---|---|---|---|---|
| 上海 31.23,121.47 / 100 nm | 200 | 101 B | **0** | — |
| 上海 / 250 nm | 200 | 16,981 B | 26 | DKH1210, CSN6917, CSN3666 |
| 北京 39.90,116.40 / 100 nm | 200 | 26,743 B | 41 | CSN8905, CCA1416, CCA1913 |
| 北京 / 250 nm | 200 | — | 76 | — |
| 深圳 / 250 nm | 200 | — | 53 | CSN8080, CSN3449 |
| 台北 / 250 nm | 200 | — | 60 | CES5341, CDG4631 |
| 法兰克福 / 100 nm | 200 | 61,476 B | 96 | HOP52HM … |
| 法兰克福 / 250 nm | 200 | **332,549 B** | 542 | EWG9548, AFR1055 |

响应（readsb v2 格式）：

```json
{"ac":[{"hex":"398565","type":"adsb_icao","flight":"HOP52HM ","r":"F-HBLF","t":"E190",
        "alt_baro":23000,"alt_geom":23225,"gs":395.1,"track":16.92,"baro_rate":0,
        "lat":…,"lon":…,"seen":…}],
 "now":1794640717501,"total":1,"ctime":…,"ptime":0}
```

**它比 OpenSky 好在哪**：免密钥、无每日额度、可 15 s 轮询，而且直接给 `t`（机型代码）和 `r`（注册号），省掉 2.8 MB 离线机型库。

**必须注意的三件事**：
1. 国内近场覆盖稀疏——上海 100 nm 是 0 架，250 nm 才 26 架。**默认半径别设 25/50 km**，否则屏幕经常空白，建议默认 100–250 km 档并允许切档。
2. 单位是英制：`alt_baro` 英尺、`gs` 节、`baro_rate` ft/min。内部统一换算：
   `alt_m = ft × 0.3048`、`vel_ms = kt × 0.514444`、`vr_ms = ft/min × 0.00508`。
3. `alt_baro` 可能是字符串 `"ground"`，必须跳过；`flight` 尾部有空格要 trim；`lc = now/1000 − seen` 用于 stale 判断。密集区 250 nm 能到 332 KB，**不能整包塞进 rx buffer**，要流式解析或裁半径。

对照源：

| 源 | 实测 | 配额 | 结论 |
|---|---|---|---|
| **OpenSky** `/api/states/all?lamin=&lomin=&lamax=&lomax=` | 200，欧洲 bbox 返回 DLH885 等；`X-Rate-Limit-Remaining: 398`；华东 ±2° 有一次返回 `states:null` | 匿名 400 credits/日、认证 4000/日（OAuth2 client credentials，token 30 min）；bbox ≤25 sq° 每次 1 credit | 备用。20 s 刷新一天要 4320 次 → 匿名不够，注册免费账号也只勉强够 21.6 s 一次 |
| **airplanes.live** `/v2/point` | **403** `{"error":"Please contact us at contact@airplanes.live…"}`，文档页同样被挡 | 现在需要邮件申请 | **剔除**。参考项目把它当免费兜底，但那是政策变化前 |

OpenSky 的 `states` 是**二维数组**，按固定索引解析（0 icao24 / 1 callsign / 5 lon / 6 lat / 7 baro_alt / 9 velocity / 10 true_track / 11 vertical_rate / 4 last_contact / 17 category），大量字段可能为 `null`。

### 1.2 网络电台

`radio-browser.info` 实测 200（`de1.api.radio-browser.info/json/stations/search?countrycode=CN&hidebroken=true&limit=15&order=clickcount&reverse=true`，16,874 B）。它是免费开源、可自建镜像的目录，**但只能用来发现，不能当运行时依赖**：

- 返回的中文电台里混着大量 `m3u8` / HLS（CCTV-13、CNR-3、环球资讯），ESP32 上要 HLS demux，不稳。
- 名字有空格、bitrate 常见 0（未校验）、部分境外流不可达。

抽取的 MP3 直链实测（各下载 5 秒）：

| 流 | HTTP | Content-Type | 5 s 下载 |
|---|---|---|---|
| `https://lhttp.qtfm.cn/live/15318317/64k.mp3`（CNR-1 中国之声） | 200 | audio/mpeg | 96,594 B |
| `https://lhttp.qingting.fm/live/4804/64k.mp3`（怀集音乐之声） | 200 | audio/mpeg | 86,794 B |
| `https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3` | 200 | audio/mpeg | 44,797 B |
| `https://stream.zeno.fm/yqawwmweq8mtv` | **000** | — | 连不上 |

**结论**：固件内置 8–12 条人工校验过的 **64–128 kbps MP3 直链**，优先国内 CDN（qtfm.cn / qingting.fm）；Web 配置页可增删改。运行时不查目录。曲名靠 ICY metadata（请求头 `Icy-MetaData: 1`，解析 `StreamTitle=`）。

### 1.3 播客（最大的坑）

实测 NPR Planet Money feed `https://feeds.npr.org/510289/podcast.xml`：

- RSS 正文 **2,053,495 字符（约 2 MB）**，含 **327 个 enclosure**。
- 第一个 enclosure 是追踪链 `https://tracking.swap.fm/.../play.podtrac.com/npr-510289/...`，经 **4 次重定向**才到真实文件 `npr.simplecastaudio.com/.../default.mp3?...&size=39525630`。
- 最终 CDN 测 `Range: bytes=0-1023` → **HTTP 206 + 1024 B**，断点续播可行。
- 另外两个 feed：storyfm.cn 猜测路径 404、BBC podcasts 域名 000 → 中文/境外 feed 可达性不能假设。

**结论：RSS 解析、去重、跟随重定向，全部放到设备外面。** 设备只读这种精简清单：

```json
{"feed":"硅谷101","updated":1794640000,
 "episodes":[{"title":"EP.208 AI 硬件的下一站",
              "url":"https://cdn.example.com/ep208.mp3",
              "bytes":39526038,"dur":2470}]}
```

设备用 `Range: bytes=<offset>-` 续播，NVS 记住每集偏移；CDN 不返回 206 就退化为从头播并在 UI 明确提示。中文播客（小宇宙等）多不公开 RSS，需要你自备 feed / OPML / 直链——**不承诺"随便填个 feed 就能播"**。

所谓「配置端」不是要你买服务器：一个 Python 脚本 + 一个静态 JSON 文件，放手机、家里 NAS、GitHub Pages 都行，手动编辑也行。

### 1.4 时间

板载 **PCF85063**（I2C `0x51`，INT=GPIO6）作主时钟，联网后 SNTP 每天校一次，RTC INT 负责整点报时 / 熄屏唤醒。**不依赖任何外部时间 API**，断网照走。

### 1.5 Open-Meteo 天气（v0.3 主源，v0.4 降为离线旁证）

> v0.4 起主源改为彩云天气（见 1.6）。本节实测数据与 WMO 映射表继续有效：
> 固件把 7 组图标做成了本地查表，彩云 `skycon` 也映射到同一组图标，所以切换源不需要重画 UI。

主源 **Open-Meteo**，免密钥、无需注册，URL：

```
https://api.open-meteo.com/v1/forecast
  ?latitude=31.23&longitude=121.47
  &current=temperature_2m,relative_humidity_2m,weather_code,is_day,wind_speed_10m
  &daily=temperature_2m_max,temperature_2m_min
  &timezone=Asia%2FShanghai&forecast_days=1
```

**实测（2026-11-14 21:00 上海）**：

| 项 | 结果 |
|---|---|
| HTTP / 体积 | **200 / 674 B**（只取 3 个 current 字段时 **402 B**；带完整 daily 819 B） |
| 延迟 | 首次 4.4 s（DNS 1.5 s + TLS 3.8 s），连接复用后 **1.9–2.0 s**（TLS 0.85 s） |
| 实况 | `temperature_2m: 16.0 °C`、`weather_code: 3`、`is_day: 0`、湿度 97%、风 9.9 km/h |
| 当日 | `temperature_2m_max: 18.9`、`min: 15.2`；`current.interval: 900`（15 min 模型步长） |
| 配额 | 非商业 **10 000 次/日**、5 000/小时、600/分钟；**CC-BY 4.0 需署名**；条款明确把「个人家庭自动化」算非商业 |
| 本项目占用 | 30 min 一次 = **48 次/日**，占配额 0.48%，毫无压力 |

响应体小到可以整包塞进 rx buffer，用 `ArduinoJson` 或手写字段提取都行，设备只需 5 个字段：`temperature_2m` / `weather_code` / `is_day` / `temperature_2m_max` / `temperature_2m_min`。`current.time` 已是本地时区 ISO 串，不必解析。

**WMO `weather_code` → 图标 + 中文**（本地查表，7 组 32×32 双色位图，比下载 PNG 省事得多）：

| 图标 | WMO code | 显示 |
|---|---|---|
| 晴 / 夜月 | 0, 1 | 晴、大部晴（`is_day` 决定太阳还是月亮） |
| 少云 | 2 | 多云 |
| 云 | 3 | 阴 |
| 雾 | 45, 48 | 雾 |
| 雨 | 51–57, 61–67, 80–82 | 毛毛雨 / 雨 / 阵雨 / 冻雨 |
| 雪 | 71–77, 85, 86 | 雪 / 阵雪 |
| 雷雨 | 95, 96, 99 | 雷雨 |

未覆盖的码统一走「未知」图标并保留温度，**不要显示空白或 `null`**。`is_day` 只影响晴 / 少云两组的日夜间变体。

**刷新与降级**：30 min 一次（模型 15 min 更新，再快无意义）；熄屏停轮询，亮屏时若距上次 >30 min 补一次；失败指数退避，但**绝不阻塞走时**（RTC 独立）。结果连同时戳写 NVS，断网显示上次值；超过 2 h 未更新，天气图标置灰并在表盘标注陈旧。这条与航班抓取一样放后台任务，共用同一套 ready flag + mutex。

**剔除的备用源**（都实测过，别再试）：

| 源 | 实测 | 结论 |
|---|---|---|
| `www.weather.com.cn/data/sk/101020100.html` | **301** → `https://e.weather.com.cn/`，返回 3 151 B HTML 首页 | 老 sk 接口已废弃，不是 JSON |
| `wttr.in/Shanghai?format=j1` | 200 但 **29 989 B**，15 s 才超时截断 | ESP32 解析不动，直接排除 |
| 和风天气 / 彩云天气 | 需 API key | v0.4 起**彩云升为主源**（用户指定并自备 token），和风仍不用 |

所以天气**没有第二个在线兜底**，靠的是「NVS 缓存 + 明确标注陈旧」。这比挂一个会悄悄失效的备源诚实。

---

### 1.6 彩云天气 v2.6（v0.4 主源，实测）

用户指定改用彩云。格式（注意坐标顺序是**经度在前**）：

```
https://api.caiyunapp.com/v2.6/{TOKEN}/{lon},{lat}/weather?dailysteps=1&hourlysteps=1
```

**2026-11-14/15 本机实测**：

| 项 | 结果 |
|---|---|
| 国内可达性 | ✅ TLS 握手约 **1.5 s**，域名直连正常 |
| 官方 demo token `<CAIYUN_DEMO_TOKEN>` | ❌ **HTTP 429** `API quota is exhausted` → 必须用自己的 token |
| 另试两个网上流传的 token | ❌ `400 token is invalid` |
| 失败响应体 | 66–74 B JSON，设备可直接判 `status != ok` |

接口格式因此**已验证**（能正常返回业务错误而非网络错误），只差一个有效 token。
固件取 `result.realtime.temperature` / `result.realtime.skycon`，以及
`result.daily.temperature[0].max/min`；`skycon` 本地映射到 1.5 节那 7 组图标。
常见值：`CLEAR_DAY/CLEAR_NIGHT`、`PARTLY_CLOUDY_DAY/NIGHT`、`CLOUDY`、`LIGHT_RAIN`、
`RAIN`、`SNOW`、`FOG`、`HAZE`、`THUNDERSTORM` 等，未命中统一走「阴」并保留温度。

**token 不用改代码**：刷机后串口发 `TOKEN <你的token>` + `REBOOT`，存 NVS。
没 token 时显示内置演示天气，点表盘天气区可循环 7 种图标，UI 不会空白。

### 1.7 v0.4 的策略调整（都是按「演示性」重排的）

| 项 | v0.3 | v0.4 | 理由 |
|---|---|---|---|
| 航班刷新 | 15 s | **60 s** | 刷新率下调；演示场景不需要准实时，省流量/省电 |
| 天气刷新 | 30 min | 30 min（不变） | 模型本身 15 min 一更 |
| 雷达默认半径 | 100 km | **250 nm ≈ 463 km** | 实测上海 100 nm 是 **0 架**，250 nm 才 26 架 |
| 半径档位 | 25/50/100/250 km | **100/150/250 nm** | 去掉两个必然空屏的小档 |
| 飞机数 | 真实多少画多少 | **不足 5 架用演示目标补足** | 用户明确「尽量保证有飞机」 |
| 同屏上限 | 40 架（参考项目） | **10 架**，标签做碰撞避让 | 360×360 圆屏放不下更多 |

演示目标在详情卡里标 `DEMO`，颜色比真实目标略浅，不会假装成实时数据。

---

## 2. 数据源架构

```
  [配置端 / 可选中继]  手机浏览器 · NAS · 云服务器 · GitHub Pages
    ├ 解析播客 RSS → 跟随重定向 → 生成精简 JSON
    ├（可选）缓存并裁剪航班 JSON：≤30 架、只留 8 个字段
    └ 维护电台清单（校验可达性、过滤 m3u8）
                    ↓ 固定 URL，设备一次 GET，小 JSON
  [设备 ESP32-S3]
    ├ 航班：adsb.lol（60 s） ──失败──> 演示目标 + 本地缓存
    │        失败后 10 分钟冷却，冷却期内直接用免费源，到期回试
    ├ 天气：彩云 v2.6（30 min，TOKEN 从 NVS） ──失败──> 内置演示天气
    ├ 电台：直连 MP3 CDN（长连接，ICY metadata）
    ├ 播客：直连 MP3 CDN + Range + NVS 进度
    └ 本地：microSD 4-bit SDMMC，断网可用
```

**刷新策略**（v0.4 已下调）：航班 **60 s**、天气 **30 min**，熄屏后一律停轮询（熄屏尚未实装，见第 9 节限制）。天气与航班共用同一份家坐标，但两者刷新周期差两个数量级，**必须拆成独立任务**，别塞进同一个 loop 里互相拖。

**内存纪律**：航班抓取放独立后台任务（ready flag + mutex，**绝不放主循环**），`http_request` 的 `buffer_size_tx` 要设到 8192（OpenSky 的 JWT 有 2–3 KB，默认 512 B 塞不下）；大响应流式解析，只保留最近 N 架。这条是参考项目写进文档的铁律。

**位置投影**（直接照抄参考项目 `common/core.yaml`，小范围精度足够）：

```
e_km  = (lon − lon0) × 111.320 × cos(lat0)
no_km = (lat − lat0) × 110.574
再按 speed × Δt 沿 track 做位置外推，补偿数据陈旧
→ 距离 / 方位 → 圆屏极坐标 (r, θ)
```

**stale 处理**：`now − lc > 60 s` 的呼号加 `*` 并标黄；`alt_baro == "ground"` 的不画。

---

## 3. 从参考项目吸收什么（以及砍掉什么）

参考项目是 **ESPHome + LVGL 8 的桌面航班雷达**，支持 8 种 800×480 / 1024×600 大横屏（要求 ≥8 MB octal PSRAM、GT911 触摸、16 MB Flash）。它的 `PLAN_SOURCES.md`、`PLAN_ATC.md`、`PLAN_MAPTILES.md`、`common/core.yaml` 写得很实在，值得抄的部分：

| 吸收 | 出处 | 在本项目的处理 |
|---|---|---|
| 三源可选 + 失败自动 fallback + 10 min 冷却 | `PLAN_SOURCES.md` | 改成 **adsb.lol 主 / OpenSky 备 / CACHE 兜底**，去掉 403 的 airplanes.live |
| readsb v2 单位换算、`"ground"` 跳过、`lc`、trim | `PLAN_SOURCES.md` | 照抄 |
| 等距投影 + 速度外推 | `core.yaml` L538/585 | 照抄 |
| 后台 job + ready flag + mutex | `PLAN_MAPTILES.md`「项目铁律」 | 照抄 |
| stale>60 s 标黄 + `*` 后缀 | `PLAN_ATC.md` | 圆屏简化为只加 `*` |
| `http_request` tx buffer 8192 | `core.yaml` L419 | 若走 OpenSky 必须 |
| **LVGL 8 而非 9**（实测 LVGL 9 让扫描线变慢） | README | 与微雪官方 **LVGL 8.4** 正好一致，照抄 |
| 分区表改动无法 OTA，需 USB 重烧一次 | README | 一期就把分区定死，别后期改 |

**砍掉**（360×360 圆屏放不下，或者不划算）：

- 40 架同屏 + 标签 → 圆屏安全半径 r≤158，最多 **8–10 架**带标签，超出只画点。
- 2.8 MB `aircraft_db.h` 离线机型库 + adsbdb.com 补查 → adsb.lol 已直接给 `t` 字段，够用。
- 地图瓦片（512 KB 分区、GitHub Pages 图块、写入防呆）→ 圆屏做纯雷达环，不要底图。
- ATC 方块模式、10 槽航迹历史、STCA 冲突检测、气象回波、HA 面板、progress strips → 全部降级或砍掉。
- 顺带：参考项目要求 GT911，本板是 **CST816S（单点 + 手势，无多点）**，所以缩放只能做档位按钮 / 上下滑，**不能捏合**。

---

## 4. 硬件事实与框架（简短）

| 项 | 值 |
|---|---|
| 屏幕 | 1.85" **圆形** 360×360（直径 47 mm，**1 px ≈ 0.13 mm**），ST77916 QSPI，RGB565，450 cd/m² |
| 触摸 | CST816S，I2C `0x15`，单点 + 上下左右手势 |
| 主控 | ESP32-S3R8 240 MHz 双核，8 MB PSRAM，16 MB Flash |
| 音频 | ES8311 codec（`0x30`）+ ES7210 AEC（`0x80`）+ 双麦 + PA=GPIO9 + 扬声器焊盘；I2S MCLK=2 / BCLK=48 / LRCK=38 / DOUT=47 / DIN=39 |
| 其它 | PCF85063 RTC(`0x51`)、QMI8658 IMU(`0x6B`)、BQ27220 电量计(`0x55`)、microSD 4-bit SDMMC、500 mAh 电池、CNC 铝壳 |
| 共用总线 | 触摸 / IMU / codec / 电量计 / RTC **全挤在 GPIO10/11 一条 I2C**，驱动层必须加 mutex（微雪例程 02 已示范 `wire_mutex`） |

**框架主张**：Arduino + **LVGL 8.4** 起步。理由只有一条——微雪官方 8 个例程已经把屏幕、触摸、ES8311+PA、SDMMC MP3、RTC、IMU、电量计全跑通了，音频是三个功能里最容易翻车的，别在这上面重新造轮子。航班抓取层把参考项目的 C++ 思路（后台任务 / fallback / 单位换算 / 投影）移植成 Arduino 任务即可，不依赖 ESPHome 运行时。**这不排斥以后换 ESPHome**：真要 HA 联动和现成 Web 配置页时，参考项目的 YAML 可以直接搬。

音频链路：`HTTP(S) → PSRAM 环形缓冲 → 软解码 MP3/AAC(ESP32-audioI2S v3.4.5) → I2S → ES8311 → PA(GPIO9) → 喇叭`。**双核分工是硬要求**：Core 0 跑音频 loop + 解码，Core 1 跑 LVGL / 触摸 / 其它网络请求。

存储分区（16 MB，一次定死）：双 OTA app 槽 + 素材/字体分区 + NVS 配置；microSD 放本地音乐与播客缓存。

---

## 5. UI（见 `round-clock-prototype.html`）

四个页面，左右滑切换：① 机械表盘 ② 电子表盘 ③ 音乐播放 ④ 航班雷达。

- **圆屏几何**：物理 r=180，CNC 外壳再吃掉约 22 px → 所有交互元素压进 **r≤158**；四角是死区，排版一律走极坐标。原型里勾选「显示圆屏安全区」可看叠加圈。
- **机械表盘**：静态刻度预渲染位图 + 3 个旋转指针精灵（1 Hz），别用 `lv_meter` 逐帧重绘；3 点日期窗（21 px）、**6 点天气副盘**（30 px 图标 + 26 px 温度 + 13 px 中文状况）。**副盘必须画在指针上层**：实测每小时 25 分前后分针正好扫过副盘，放底层会压住温度数字，信息优先于拟真。点副盘可循环 7 种 WMO 天气演示映射。
- **电子表盘**：上半屏 68 px 时间 + 18 px 秒；下半屏 **42 px 天气图标 + 32 px 温度 + 16 px 状况 + 12 px 高低温**；底部一行 12 px 状态。原来的电量 / 音量双副环（标签只有 6.5 px）**已删除**，位置让给天气。
- **音乐**：圆形唱片 + 外圈进度弧 + 三键沿下弧排列；左上源标签点一下在 RADIO / PODCAST / SD 间切换。
- **雷达**：同心圆 + 扫描线（纯视觉动画，位置不插值）+ 按航向旋转的小三角；点顶部源标签切换 ADSB.LOL / OPENSKY / CACHE，点右上切 25/50/100/250 km，点飞机看呼号 / 高度 / 地速 / 距离方位 / 机型注册号（此时暂时收起其它呼号避免半截文字），点空白关闭，stale 的带 `*`。

### 5.1 字号规范（v0.3 新增，硬性）

1.85" 圆屏直径 **47 mm / 360 px → 1 px ≈ 0.1305 mm**；大写字母实际高度约 `0.72 em`。原稿里大量 6.5–9 px 的字只有 0.6–0.8 mm 高，50 cm 桌面视距下根本读不出来。

| 字号 | 实际字高 | 用途 |
|---|---|---|
| 6.5–9 px（旧稿） | 0.6–0.8 mm | ❌ 已全部删除或放大 |
| **10–12 px** | 0.9–1.1 mm | 静态标签下限（页码、状态行、次要标注） |
| **16 px** | 1.5 mm | 次要信息（天气状况、曲名） |
| **24–32 px** | 2.2–3.0 mm | 关键数值（温度、日期、电量） |
| **68 px** | 6.4 mm | 时间主读数 |

**规则**：全屏最小 10 px、表盘最小 12 px；每屏最多 2 组大数字；**字变大必然要砍元素**。v0.3 因此删掉机械表盘的 7 px 品牌副标与底部小字（与刻度、VII/V 打架）、电子表盘的两个 6.5 px 副环，并把音乐 / 雷达页所有 <10 px 的字提到 ≥10 px。字体用 `lv_font_conv` 预生成 **12 / 16 / 24 / 32 / 68 px 等宽数字位图 + 约 200 字中文子集**放 Flash，不要运行时缩放矢量字体。文字统一加同底色描边，避免秒环 / 准线细线穿过笔画。

原型已用无头 Chrome 逐页渲染 + 2× 放大验收：修掉分针压温度、页码点压秒环最低点、雷达详情卡左下角越出 r=158、详情态呼号被切半等 6 处真实问题；静态校验 67 个 DOM ID 无重复 / 无悬空引用、JS 语法通过、**0 个 <10 px 字号**。

---

## 6. 风险清单

| # | 风险 | 对策 |
|---|---|---|
| 1 | adsb.lol / 播客 CDN 都是境外，国内长期可达性不保证 | 先直连（今日实测通）；保留 CACHE 兜底；预留中继 URL 配置项，不稳时再开 |
| 2 | 密集区 250 nm 响应 332 KB，超 rx buffer | 流式解析 + 限制保留架数；或中继裁剪 |
| 3 | 500 mAh 电池 + WiFi 常连 + 解码，续航约 2–4 h，不能当手表戴一天 | 熄屏停轮询 / 降亮度 / 提腕唤醒；当桌面摆件定位，长期插电 |
| 4 | 一条 I2C 上挂 6 个设备，并发读会打架 | 全局 mutex，触摸与音频分离任务 |
| 5 | m3u8 / HLS / 变码率流让软解码卡死 | 清单只收 MP3 直链；连不上按指数退避重试并提示 |
| 6 | 「扬声器焊盘」与「内置扬声器」资料口径不一 | 到手先确认是否已焊喇叭；没有就补焊 |
| 7 | 262K 面板走 RGB565 只有 65K 色 | 避免大面积平滑渐变，用分层色块 |
| 8 | Open-Meteo 也是境外源，国内长期可达性不保证；且**没有在线兜底** | NVS 缓存 + 超 2 h 明确标注陈旧；预留与航班共用的中继 URL 配置项；走时完全不受影响 |
| 9 | Free API 数据按 **CC-BY 4.0** 授权，个人非商业也需署名 | 设备「关于」页 + 源码 README 注明 `Weather data by Open-Meteo.com`；若日后商用须买订阅 |
| 10 | WMO code 覆盖不全 / 新增码 | 统一「未知」图标并保留温度，绝不显示空白或 `null` |
| 11 | 字号放大后一屏放不下原有信息量 | 按 5.1 规范砍元素；天气与时间是最高优先级，电量 / 音量降级到状态行 |

---

## 7. 分期与实装状态（v0.4 更新）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 点亮屏幕 / 触摸 / 音频 | ✅ **已完成**：QSPI ST77916 + CST816S + ES8311/I2S，实机日志全绿 |
| P1 | 双表盘 + 天气区 + 大字号 | ✅ **已完成**：机械/电子双表盘，彩云天气（待 token），24–72 px 字 |
| P2 | adsb.lol + 投影 + 雷达页 | ✅ **已完成**：250 nm 大半径 + 演示兜底 + 标签避让 + 详情卡 |
| P3 | 网络电台播放 | ✅ **已完成**：三条实测 MP3 直链 + 大按键 + 黑胶转动 |
| P4 | 番茄钟 | ✅ **已完成**：25/5/15 循环、进度环、轮次点、三键 |
| P5 | 播客 / SD / RTC / 熄屏 / Web 配置 / OTA | ⬜ 未实装，见 `firmware/README.md` 第 8 节 |

原计划的「Web 配置页」被**串口命令 + NVS**替代：一样不用重编译，但零依赖、已在实机验证。

---

## 8. 现在只差两样配置（都不阻塞演示）

1. **彩云天气 token** —— 官方 demo token 已 429。串口发 `TOKEN <token>` + `REBOOT` 即可，不用重编译。
2. **Wi-Fi 名称与密码** —— 串口发 `WIFI <ssid> <pass>` + `REBOOT`。不配也能跑（全演示模式），
   只是没有实时天气/航班、也不出声。

家坐标默认上海 `31.23 / 121.47`、雷达默认 250 nm。要改城市，改 `config.h` 的 `HOME_LAT/HOME_LON`
后重新 `flash.bat`；天气与雷达共用这个坐标。播客 feed / OPML 如果以后要做播客再给。

---

## 9. 固件实现状态（v0.4，2026-11-15 已上板）

### 9.1 为什么不是 ESPHome / LVGL

参考项目 `delphicchen/esp32_flight_radar` 是 ESPHome + LVGL 8 的 800×480 横屏方案。本机平台调研后定的是：

- **Arduino core 3.0.7 / IDF 5.1**（Arduino IDE 内置 arduino-cli）。微雪官方 QSPI 驱动标注要 core ≥3.2，
  实测 3.0.7 的 `esp_lcd` 已够用，只改了两处头文件路径就编通。
- **PlatformIO 里那份 framework 实为 Arduino 2.0.17 / IDF 4.4**，缺 `quad_mode` / `ledcAttach`，
  第一次编译直接失败，已弃用（旧工程骨架清掉了）。
- **不用 LVGL**，自己写 RGB565 帧缓冲 + 圆/弧/线/三角/点阵字。理由：零外部库依赖、离线可编译、
  360×360 全屏刷一次 259,200 B 可控，且中文字库自己生成子集更省 Flash。

### 9.2 实机验证结果

设备：`VID_303A:PID_1001` 原生 USB-JTAG/Serial，**COM12**；`ESP32-S3 rev0.2 / 8 MB PSRAM / 16 MB Flash`。
烧录 1.71 MB / 约 11 s / Hash 校验通过。启动日志关键行：

```
Flash 16 MB  PSRAM 8388608 bytes  free heap 316536
Register 0x04 data: 00 02 7f 7f        <- ST77916 识别，走 version 2 初始化表
ChipID:0xb5   ProjID:0x71            <- CST816S
[GFX] framebuffer 259200 bytes @ 0x3c1a0e0c
[AUD] ES8311 + I2S ready (44100 Hz / 16 bit)
[NET] no Wi-Fi configured -> full DEMO mode
```

**五页真实帧缓冲逐页抓取验收**（`PAGE n` + `SHOT` 回传 259,200 B，PC 转 PNG，见 `firmware/preview/`）。
这比 HTML 原型严格得多，抓到并修掉了 7 个浏览器模拟看不到的真问题：

| # | 问题 | 修法 |
|---|---|---|
| 1 | 机械表盘底部状态文字压住 VII/V 与斜刻度 | 缩短文案 + 状态栏移到 y=300 的刻度空隙 |
| 2 | 页码点压在底部正中长刻度上 | 机械页 y=314、其余页 y=320，按各页环/刻度分别避让 |
| 3 | 天气副盘中文被盘底圆弧切穿 | 副盘改 r=44@248，三行内容重排进盘内 |
| 4 | 雷达呼号与标题/状态行/档位按钮重叠 | 顶部 92 px、底部 268 px 设保留区，标签不进 |
| 5 | 雷达呼号两两相撞、压住中心本机标记 | 两两碰撞下移 + 中心 r<48 禁区 + 圆边几何裁剪 |
| 6 | 非 ASCII 间隔符 `·` 在点阵字库里渲染成空白 | 全部换成 `/` |
| 7 | `CLEAR` 清 NVS 后 `SHOW` 仍显示旧 token | 内存态同步回落 `config.h` 默认值 |

**字节序坑**（写在这里免得下次再踩）：微雪 `LCD_addWindow()` 第 573 行会
`color[i] = ((color[i]>>8)&0xFF) | ((color[i]<<8)&0xFF00)` **原地交换 FB**，
所以刷屏后帧缓冲是 swap 态。屏幕显示正确，但 `SHOT` 回传的数据要按**大端** `>u2` 解读，
按小端会得到棕红/青绿的错位画面。

### 9.3 新增能力

- **真实音频**：ES8311 codec + `ESP32-audioI2S 3.4.0`，三条实测 MP3 直链，中心键播放/暂停、左右键切台。
  默认不自动连流，不会一上电就出声。
- **NVS 运行时配置**：`WIFI / TOKEN / SHOW / CLEAR / REBOOT`，实机逐条验证过。
- **调试命令**：`PAGE <0..4>` 切页、`SHOT` 回传帧缓冲，配 `tools/capture_frame.py` 出 PNG。
- **字库生成**：`tools/gen_font.py` 用系统字体生成 12 px 等宽 ASCII + 24 px ASCII/中文 201 字形，
  输出 `font_data.h`。

### 9.4 还没做的（诚实清单，v0.4 时点）

播客精简 JSON 与 SD 播放、PCF85063 掉电走时（当前是 SNTP + `millis()` 软时钟）、
熄屏/抬腕唤醒、Web 配置页、OTA。
> 其中「Web 配置页」已在 v0.5 补上（设备端配网 AP），其余仍未实装。
> v0.4 曾把「真实音频播放」写得过于乐观：当时只验证了 codec 初始化，没有验证解码与出声，
> v0.5 已用 `TONE` 离线 WAV 测试补上解码链路证据。

---

## 10. v0.5：从「演示模式」到「真实功能」（2026-11-15 实机验收）

用户连续指出「没有真实实现吧？」「我是需要能实现功能的」「多处字体位置有问题」
「冒号需要在圆的中心视觉上才比较平衡」。本节记录这一轮的根因与修复。

### 10.1 字体：AA 大数字 + 居中算法重写

| 症状 | 根因 | 修法 |
|---|---|---|
| `10:47` / `25:00` 锯齿严重 | 24px **1-bit 点阵放大 3 倍**，每个可见像素块 3px | 生成 44×64 **4-bit 灰度**大数字（`Segoe UI Light`），双线性缩放后 alpha 混合，1:1 绘制不放大 |
| 冒号不在圆心 | `drawText` 用 `x -= total/2` 居中，但字形 canvas(24) 比步进(14) 宽，`scale=3` 时整串系统性**右偏 15px** | `drawBigText(cx,...)` 按**冒号墨迹中心**布局；`drawText` 改用「步进和 − 末字步进 + 完整 canvas」的真实绘制宽度居中 |
| 日期窗数字偏右 | 同上，FONT24 ASCII 右偏 5px | 日期窗改 AA 小数字（22×32），精确居中 |
| 天气中文明显偏大 | 24px 方块字与同字号数字视觉差一圈 | 天气中文改 **16px**，温度/秒改 AA 22×32，高低温 12px，字号依次递减 |
| 日期年份月份写死 `2026-11` | 只同步了 `tm_mday` | SNTP 同步 `tm_year/tm_mon/tm_mday`，软时钟含月/年进位与闰年 |

`gfx.cpp` 新增 `aaPix / blendPixel / aaBlit(8.8 定点双线性) / aaText / drawBigText / drawMidText / drawDegree`，
`tools/gen_font.py` 新增 `FONT16_*` 与 `FONT_BIG_AA`。`FONT_BIG_AA` 每字节两个 4-bit 像素，
高半字节是左像素。5/4 放大得到 55×80 大时间，1/2 缩小得到 22×32 温度与日期。

**验收数字（真机 PNG 逐像素统计）**：电子表盘冒号墨迹质心 `x=180.35`，番茄钟 `x=180.43`；
大数字墨迹高 58–59px、有连续灰度边缘（1-bit 放大时中间灰度级接近 0）；
离线渲染器 `tools/offline_preview.py` 报告 5 页零越界。

### 10.2 真实数据链路

| 链路 | v0.4 状态 | v0.5 实机结果 |
|---|---|---|
| 配置 | 只能串口输 SSID/密码 | 新增 `provision.h`：无 SSID 自动开 AP `RoundClock-Setup`（AP_STA 模式可扫描周边网络）+ DNS 劫持 + 网页表单，PC 实测 `HTTP 200 / 1542 B`、拿到 `192.168.4.2` |
| 时间 | SNTP 只同步时分秒、日期硬编码 | `[TIME] SNTP -> 2026-11-15 08:49:47`，`STATUS` 显示 `SNTP=SYNCED` |
| 天气 | 只有彩云，demo token 实测 429，无 token 就一直是假天气 | 彩云为主、**Open-Meteo 免密钥兜底**：`[WX] OPENMETEO 15C 阴 13/19 wmo=3 (553B)`，`STATUS` 显示 `live=1` |
| 航班 | HTTP 200 / 27KB 但 `live=0` | `distKm` 漏了 `/1000`（米当公里），修后 `[ADSB] live=10 demo=0`，`PLANES` 打印 10 个真实呼号/机型/高度/速度 |
| 音频 | 只验证 codec 初始化 | `TONE 440 1500` 生成 FFat WAV → `connecttoFS OK` → `Channels:1 / SampleRate:44100 / decoding=YES`；网络电台也已 `streaming=YES` |

**航班防回退**：保存 `lastLive[]` 快照；刷新失败或解析出 0 架时恢复快照并标 `STALE`
（目标变黄、标题加 `!`），绝不静默换成假目标。只有从未联网成功才补演示飞机。
切半径按 `r *= oldRadius/newRadius` 重投影而非清空。
`tools/test_adsb_parse.py` + `tools/adsb_sample.json` 是距离换算的回归测试（修复后 10 架 / 旧公式 0 架）。

### 10.3 三个「看起来像功能没实现」的假象

1. **雷达截图一直显示演示呼号**：`capture_frame.py` 用 pyserial 默认参数打开串口，
   `open()` 把 DTR/RTS 拉高，ESP32-S3 USB-Serial/JTAG 当成复位序列，**每次开关串口板子都重启**
   （`reset=0 POWERON`、`uptime=1s`），抓图恰好发生在重启后、首次 60s 拉取完成之前。
   在 `open()` **之前**设 `dtr=False; rts=False` 后，uptime 连续、雷达显示真实呼号。
2. **USB CDC 卡死**：某次 `SetCommState` / `EscapeCommFunction(SETDTR)` 恒返回
   `ERROR_GEN_FAILURE(31)`，数据端点写入也超时，说明芯片 USB 栈没在响应 EP0。
   RTS 脉冲唤醒无效，**物理重新插拔 USB 后恢复**；`tools/serial_recover.py` 保留诊断路径。
3. **首帧仍是演示数据**：TLS 冷启动偶发 `HTTP -11`，启动阶段重试 3 次 + 超时放宽到 15 s。

### 10.4 仍未实装

播客精简 JSON / SD 播放、PCF85063 掉电走时、熄屏与抬腕唤醒、OTA。
电台「可听」需人耳确认：解码/I2S 链路已有板上证据，但本环境无法代替听音。

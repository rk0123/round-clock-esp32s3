#pragma once
// 电台表 v2：精选 33 条，PC 实测日期 2026-11-17
// 排序：国内(仅清单内可直连的蜻蜓 CDN)在前，国外在后；前几条为低延迟国内 CDN
// 实测：curl VLC/3.0.21 UA，Range 0-4095，HTTP 200/206 + 真实 mp3/aac 帧同步；已排除 HLS
static const char *const STREAM_URLS[] = {
  "https://lhttp.qingting.fm/live/2803/64k.mp3",  // 苏州都市音乐广播
  "https://lhttp.qingting.fm/live/4804/64k.mp3",  // 怀集音乐之声
  "https://lhttp.qingting.fm/live/4581/64k.mp3",  // AsiaFM 综合
  "https://lhttp.qtfm.cn/live/15318317/64k.mp3",  // 蜻蜓FM 频道一
  "https://lhttp.qtfm.cn/live/4915/64k.mp3",  // 蜻蜓FM 频道二
  "https://lhttp.qtfm.cn/live/1301/64k.mp3",  // 蜻蜓FM 频道四
  "https://lhttp.qtfm.cn/live/1136/64k.mp3",  // 蜻蜓FM 频道五
  "https://lhttp.qtfm.cn/live/1122/64k.mp3",  // 蜻蜓FM 频道六
  "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service",  // BBC 世界广播
  "https://rfienchinois64k.ice.infomaniak.ch/rfienchinois-64.mp3",  // 法国国际广播 华语
  "https://tunein.cdnstream1.com/3519_96.aac",  // CNN
  "https://tunein.cdnstream1.com/3522_96.aac",  // Bloomberg
  "https://streams.kqed.org/kqedradio",  // KQED 公共广播
  "https://stream.houstonpublicmedia.org/news-mp3",  // HPM 新闻
  "https://icecast.omroep.nl/radio1-bb-mp3",  // NPO 一台
  "https://stream.wqxr.org/wqxr.mp3",  // WQXR 古典
  "https://abc.streamguys1.com/live/classicfmnsw/icecast.audio",  // ABC Classic
  "https://ycradio.stream.publicradio.org/ycradio.mp3",  // Your Classical
  "https://knkx-live-a.edge.audiocdn.com/6285_256k",  // Jazz24
  "https://smoothjazz.cdnstream1.com:443/2585_128.mp3",  // Smooth Jazz
  "https://ais-sa8.cdnstream1.com/3629_128.mp3",  // WBGO Jazz
  "https://ais-sa3.cdnstream1.com/2442_128.aac",  // KMHD Jazz
  "https://stream.houstonpublicmedia.org/classical-mp3?source=npr",  // HPM 古典
  "https://ice1.somafm.com/groovesalad-128-mp3",  // SomaFM 律动
  "https://ice1.somafm.com/dronezone-128-mp3",  // SomaFM 氛围
  "http://stream.radioparadise.com/aac-320",  // Radio Paradise
  "https://virgin.live.stream.broadcasting.news/stream-mobile",  // Virgin Radio UK
  "https://streaming.nrjaudio.fm/oufdfatx4thg",  // NRJ
  "https://fr2.1mix.co.uk:8060/320h",  // 1Mix EDM
  "https://abc.streamguys1.com/live/triplejnsw/icecast.audio",  // ABC Triple J
  "https://stream.revma.ihrhls.com/zc9003",  // iHeart K-Pop
  "https://play.streamafrica.net/jpopradio",  // J-POP Radio
  "http://stream01-ssl.arenastreaming.com:8043/stream",  // 雅加达中文电台
};

static const char *const STREAM_LABELS[] = {
  "苏州都市音乐广播", "怀集音乐之声", "AsiaFM 综合", "蜻蜓FM 频道一",
  "蜻蜓FM 频道二", "蜻蜓FM 频道四", "蜻蜓FM 频道五", "蜻蜓FM 频道六",
  "BBC 世界广播", "法国国际广播 华语", "CNN", "Bloomberg", "KQED 公共广播", "HPM 新闻",
  "NPO 一台", "WQXR 古典", "ABC Classic", "Your Classical", "Jazz24", "Smooth Jazz",
  "WBGO Jazz", "KMHD Jazz", "HPM 古典", "SomaFM 律动", "SomaFM 氛围", "Radio Paradise",
  "Virgin Radio UK", "NRJ", "1Mix EDM", "ABC Triple J", "iHeart K-Pop", "J-POP Radio",
  "雅加达中文电台"
};

#define STREAM_COUNT 33

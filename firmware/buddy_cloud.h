#pragma once
// ============================================================
//  WorkBuddy Buddy · 云端推送源客户端
//  - 设备用 WiFi 轮询 buddy-relay 的 /api/state，把快照喂给 petFeedJson()
//  - 触摸 OK/NO 只置位 wbCloudDecisionPending，POST 在 netTask 里做
//  - 所有 TLS 用已启用的 x509 CA bundle，绝不在触摸/渲染路径里联网
//  - URL / token 存 NVS（wb.url / wb.tok），串口命令 WBCLOUD 配置
// ============================================================

#include "secure_http.h"      // cloudUseCaBundle / BoundedHttpBody
#include <ArduinoJson.h>

static String  wbCloudUrl = "";
static String  wbCloudToken = "";
static String  wbCloudShare = "";     // WorkBuddy 分享链接的访问码（每次请求都要带）
static uint32_t wbCloudLastPoll = 0;
static uint8_t  wbCloudFails = 0;
static uint16_t wbCloudPolls = 0;

static String wbCloudSplitShare(String &u) {
  String share;
  int q = u.indexOf('?');
  if (q < 0) return share;
  String qs = u.substring(q + 1);
  u = u.substring(0, q);
  int p = qs.indexOf("sharecode=");
  if (p >= 0) {
    p += 10;
    int e = qs.indexOf('&', p);
    share = (e < 0) ? qs.substring(p) : qs.substring(p, e);
  }
  return share;
}

static void wbCloudInit() {
  wbCloudUrl = cfgGetStr("wb.url", "");
  wbCloudToken = cfgGetStr("wb.tok", "");
  wbCloudShare = cfgGetStr("wb.share", "");
  wbCloudUrl.trim();
  wbCloudToken.trim();
  wbCloudShare.trim();
  // 只有 URL 里确实还带着 ?sharecode= 才覆盖已存的访问码。
  // 之前这里无条件覆盖：wb.url 存的是去掉 query 的裸域名，拆分结果恒为空串，
  // 开机一次就把 wb.share 抹成 ""，之后每个请求都缺 sharecode → 全站 403，
  // 表现为「重启后宠物永远睡眠、和桌面状态毫不相干」（当时 fails=101 / polls=0）。
  String fromUrl = wbCloudSplitShare(wbCloudUrl);
  if (fromUrl.length()) wbCloudShare = fromUrl;
  while (wbCloudUrl.endsWith("/")) wbCloudUrl.remove(wbCloudUrl.length() - 1);
  // 只在确实被改过时回写，别每次开机都写一遍 NVS
  String stored = cfgGetStr("wb.share", "");
  stored.trim();
  if (stored != wbCloudShare) cfgSet("wb.share", wbCloudShare);
}

static bool wbCloudEnabled() {
  return wbCloudUrl.length() > 8 && wbCloudToken.length() >= 4;
}

static void wbCloudSet(const String &url, const String &tok) {
  String u = url; u.trim();
  wbCloudShare = wbCloudSplitShare(u);
  while (u.endsWith("/")) u.remove(u.length() - 1);
  wbCloudUrl = u;
  wbCloudToken = tok;
  wbCloudToken.trim();
  cfgSet("wb.url", wbCloudUrl);
  cfgSet("wb.tok", wbCloudToken);
  cfgSet("wb.share", wbCloudShare);
  wbCloudLastPoll = 0;
  Serial.printf("[WBC] cloud=%s token_len=%d share_len=%d\n",
                wbCloudUrl.c_str(), wbCloudToken.length(), wbCloudShare.length());
}

static void wbCloudOff() {
  wbCloudUrl = "";
  wbCloudToken = "";
  wbCloudShare = "";
  cfgSet("wb.url", "");
  cfgSet("wb.tok", "");
  cfgSet("wb.share", "");
  Serial.println("[WBC] cloud disabled");
}

// 回传决策。返回 true 表示成功或没有待回传项。
static bool wbCloudFlushDecision() {
  if (!wbCloudDecisionPending) return true;
  if (!wbCloudEnabled() || WiFi.status() != WL_CONNECTED) return false;

  char body[320];
  StaticJsonDocument<384> doc;
  doc["token"] = wbCloudToken;
  doc["id"] = wbCloudDecisionId;
  doc["decision"] = wbCloudDecisionApprove ? "once" : "deny";
  doc["tool"] = wbCloudDecisionTool;
  doc["hint"] = wbCloudDecisionHint;
  serializeJson(doc, body, sizeof(body));

  String url = wbCloudUrl + "/api/decision";
  if (wbCloudShare.length()) url += "?sharecode=" + wbCloudShare;
  WiFiClientSecure cli;
  cloudUseCaBundle(cli);
  HTTPClient http;
  if (!http.begin(cli, url)) return false;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  http.end();
  if (code == 200) {
    Serial.printf("[WBC] decision sent id=%s %s\n", wbCloudDecisionId,
                  wbCloudDecisionApprove ? "once" : "deny");
    wbCloudDecisionPending = false;
    return true;
  }
  Serial.printf("[WBC] decision POST failed code=%d\n", code);
  return false;
}

// 轮询云端快照。必须在 netTask 里调用（16KB 栈 + TLS 内存已预留）。
static void wbCloudPoll(bool force = false) {
  if (!wbCloudEnabled()) return;
  const uint32_t now = millis();
  if (!force && (uint32_t)(now - wbCloudLastPoll) < 5000UL) return;
  wbCloudLastPoll = now;

  if (!wbCloudFlushDecision()) return;           // 决策优先，失败则本轮不拉新状态
  if (WiFi.status() != WL_CONNECTED) {
    wbSnap.connected = false;                    // 掉网：宠物回睡眠；不清 prompt
    return;
  }

  WiFiClientSecure cli;
  cloudUseCaBundle(cli);
  HTTPClient http;
  String url = wbCloudUrl + "/api/state?";
  if (wbCloudShare.length()) url += "sharecode=" + wbCloudShare + "&";
  url += "token=" + wbCloudToken;
  if (!http.begin(cli, url)) {
    wbSnap.connected = false;
    return;
  }
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    if (++wbCloudFails == 3)
      Serial.printf("[WBC] poll failed code=%d%s\n", code,
                    code == 403 ? " (403 = sharecode missing, re-send WBCLOUD)" : "");
    wbSnap.connected = false;
    return;
  }
  BoundedHttpBody body(1200);
  const bool got = body.receive(http);
  http.end();
  if (!got) return;

  StaticJsonDocument<1536> doc;
  // StreamString 同时是 Stream 和 String，ArduinoJson 会歧义；按现有代码显式转 String
  if (deserializeJson(doc, static_cast<const String &>(body))) {
    Serial.println("[WBC] bad state json");
    return;
  }
  JsonObject snap = doc["snapshot"];
  if (snap.isNull()) return;
  char line[1024];
  serializeJson(snap, line, sizeof(line));
  petFeedJson(line);                             // 复用串口同款协议
  wbCloudFails = 0;
  wbCloudPolls++;
}

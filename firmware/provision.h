#pragma once
// ============================================================================
//  设备端 Wi-Fi 配网（Captive Portal）
//  没有保存 SSID 时自动开 AP "RoundClock-Setup"（192.168.4.1）：
//  手机/电脑连上后会自动弹出配置页，选 Wi-Fi、填密码，可选填彩云 token，
//  提交后写入 NVS 并重启，重启后即走 STA 联网，天气/航班/电台全部变真实数据。
//  也可随时用串口命令 AP 手工打开配网 AP。
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "runtime_config.h"

static WebServer  *gProvWeb = nullptr;
static DNSServer  *gProvDns = nullptr;
static bool        gProvActive = false;
static uint32_t    gProvRebootAt = 0;

static void provSendRoot() {
  // AP 模式下扫描周边 2.4G 网络，生成下拉框；扫描约 1~2 秒，只在打开页面时做
  String opts = "";
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n && i < 24; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    ssid.replace("&", "&amp;"); ssid.replace("<", "&lt;"); ssid.replace("\"", "&quot;");
    opts += "<option value=\"" + ssid + "\">" + ssid +
            " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
  }
  WiFi.scanDelete();
  String html;
  html.reserve(2600 + opts.length());
  html += F("<!doctype html><html lang='zh'><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>RoundClock 配网</title><style>"
            "body{margin:0;background:#0b1014;color:#e8eef5;font:16px/1.6 system-ui;padding:24px}"
            ".c{max-width:420px;margin:auto}h1{font-size:20px;color:#e2622e}"
            "label{display:block;margin:14px 0 4px;color:#8b97a9;font-size:13px}"
            "input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
            "border:1px solid #2b353d;background:#101820;color:#e8eef5;font-size:16px}"
            "button{width:100%;margin-top:22px;padding:12px;border:0;border-radius:8px;"
            "background:#e2622e;color:#fff;font-size:16px;font-weight:600}"
            "p{color:#8b97a9;font-size:12px;margin-top:18px}</style></head><body><div class='c'>"
            "<h1>RoundClock · 圆屏终端配网</h1>"
            "<form method='POST' action='/save'>");
  html += F("<label>Wi-Fi 名称</label><select name='ssid' id='ssid'>");
  html += opts;
  if (opts.length() == 0) html += F("<option value=''>未扫描到网络</option>");
  html += F("</select><label>或手动输入 SSID（优先使用）</label>"
            "<input name='ssid_manual' placeholder='留空则使用上面的下拉框'>"
            "<label>Wi-Fi 密码</label><input name='pass' type='password' placeholder='开放网络可留空'>"
            "<label>彩云天气 Token（可留空）</label>"
            "<input name='token' placeholder='留空时自动用 Open-Meteo 真实天气'>"
            "<button type='submit'>保存并重启</button></form>"
            "<p>留空彩云 Token 也能用：设备会自动改用 Open-Meteo 获取真实天气。"
            "航班数据来自 adsb.lol，无需密钥。保存后设备自动重启联网。</p>"
            "</div></body></html>");
  gProvWeb->send(200, "text/html; charset=utf-8", html);
}

static void provSendSave() {
  String ssid  = gProvWeb->hasArg("ssid_manual") ? gProvWeb->arg("ssid_manual") : String("");
  ssid.trim();
  if (ssid.length() == 0) ssid = gProvWeb->arg("ssid");
  String pass  = gProvWeb->arg("pass");
  String token = gProvWeb->arg("token");
  token.trim();
  if (ssid.length() == 0) {
    gProvWeb->send(400, "text/html; charset=utf-8",
                   "<h3>Wi-Fi 名称不能为空</h3><a href='/'>返回</a>");
    return;
  }
  cfgSet("ssid", ssid);
  cfgSet("pass", pass);
  cfgSet("token", token);
  gCfg.ssid = ssid; gCfg.pass = pass; gCfg.token = token;
  Serial.printf("[PROV] saved ssid='%s' caiyun=%s, rebooting...\n",
                ssid.c_str(), token.length() ? "SET" : "EMPTY (Open-Meteo)");
  gProvWeb->send(200, "text/html; charset=utf-8",
                 String(F("<!doctype html><meta charset='utf-8'><h2>已保存，设备正在重启并连接 Wi-Fi…</h2>"
                          "<p>可以关闭本页。约 10 秒后表盘天气/航班会切换到真实数据。</p>")));
  gProvRebootAt = millis() + 1200;
}

static bool provisionStart() {
  if (gProvActive) return true;
  // AP_STA 而不是纯 AP：这样页面里的 WiFi.scanNetworks() 能真正列出周边网络，
  // 用户在下拉框里就能直接点选，而不必手动敲 SSID
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  if (!WiFi.softAP("RoundClock-Setup")) {
    Serial.println("[PROV] softAP failed");
    return false;
  }
  delay(120);
  gProvDns = new DNSServer();
  gProvDns->setTTL(300);
  gProvDns->start(53, "*", WiFi.softAPIP());
  gProvWeb = new WebServer(80);
  gProvWeb->on("/",        HTTP_GET,  provSendRoot);
  gProvWeb->on("/save",    HTTP_POST, provSendSave);
  gProvWeb->onNotFound([]() { gProvWeb->sendHeader("Location", "/", true); gProvWeb->send(302, "text/plain", ""); });
  gProvWeb->begin();
  gProvActive = true;
  Serial.printf("[PROV] AP 'RoundClock-Setup' @ %s  (portal http://192.168.4.1/)\n",
                WiFi.softAPIP().toString().c_str());
  return true;
}

static void provisionLoop() {
  if (!gProvActive) return;
  gProvDns->processNextRequest();
  gProvWeb->handleClient();
  if (gProvRebootAt && millis() >= gProvRebootAt) {
    Serial.println("[PROV] restarting into STA mode");
    delay(200);
    ESP.restart();
  }
}

// sketch entry; setup()/loop() live in main.cpp
// 显式列出配网页所需库，确保 Arduino 的库发现能链上 WebServer / DNSServer
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

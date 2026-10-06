#pragma once
#include <Arduino.h>

/* WiFi 凭据与配网 —— 自包含, 不碰屏幕(tft), 屏幕提示由 main.cpp 通过回调负责。
 *
 * 凭据存 NVS 的 "wifi" namespace, 不写死在代码里。
 * 配网时 ESP32 自己开热点跑一个极简 WebServer, 手机连上去填 SSID/密码。 */

/* NVS 里有没有存过凭据 */
bool wifiHasStored();

/* 用 NVS 里的凭据连 WiFi, timeoutMs 内没连上就返回 false */
bool wifiConnectStored(uint32_t timeoutMs = 10000);

/* 阻塞式配网: 开 AP + WebServer, 直到用户保存或超时。
 * statusCb(l1, l2) 用来在屏幕上显示两行状态, 传 nullptr 也行。
 *
 * 注意: 本函数不会正常返回 —— 保存成功和超时两条路都会 ESP.restart()。
 * (重启后新凭据才会生效, 也避免把已连上 AP 的手机晾在半路) */
void wifiRunSetupPortal(void (*statusCb)(const char *l1, const char *l2) = nullptr,
                        uint32_t timeoutMs = 300000);

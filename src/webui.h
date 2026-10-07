#pragma once
#include <Arduino.h>

/* 常驻网页 —— 用药记录的查看与编辑入口。
 *
 * 为什么做成常驻的: 麦克风到货之前, 这是唯一能录入用药方案的办法;
 * 到货之后它仍然是「降级路径」—— 不方便说话时、或者要批量整理记录时用。
 *
 * 和 vision.cpp 里那个照片预览网页的区别: 那个是「开一会儿就关」的阻塞式,
 * 这个跟着 loop() 一直跑, 每次只处理一个请求就返回, 不打断取景。 */

/* setup() 里 WiFi 连上之后调一次。
 *
 * onTftReset: 网页上「重置屏幕」按钮的处理函数, 传 nullptr 就没有这个按钮。
 *   用途: 手指碰杜邦线把面板抖白之后(线没断, 但面板收到假的 DISPOFF 把显示关了),
 *   手机点一下就能整屏重初始化, 不用跑到设备跟前断电重启。 */
void webuiBegin(void (*onTftReset)() = nullptr);

/* loop() 里每帧调一次。内部只做一次 handleClient(), 开销极小 */
void webuiLoop();

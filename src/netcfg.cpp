#include "logcfg.h"   /* 必须第一行: 日志配置要在 <Arduino.h> 之前生效 */
#include "strutil.h"  // std::string 桥接

static const char *TAG = "netcfg";    // 日志标签

#include "netcfg.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

/* ==================== 可改参数 ==================== */
#define AP_SSID        "ESP32-CAM-Setup"   // 配网热点的名字
#define AP_PASS        nullptr             // 想给热点加密就填 >=8 位的字符串
#define SETUP_TIMEOUT  300000              // 配网多久没人操作就自动重启 (ms)
/* ================================================= */

static Preferences prefs;

/* ==================== 凭据读写 ==================== */
bool wifiHasStored()
{
    prefs.begin("wifi", true);
    const bool has = prefs.getString("ssid", "").length() > 0;
    prefs.end();
    return has;
}

bool wifiConnectStored(uint32_t timeoutMs)
{
    prefs.begin("wifi", true);
    const std::string ssid = toStd(prefs.getString("ssid", ""));   // NVS 返回 Arduino String
    const std::string pass = toStd(prefs.getString("pass", ""));
    prefs.end();

    if (ssid.empty()) return false;

    /* persistent(false): 不让 SDK 把凭据再往它自己的 NVS 区存一份,
       免得改了配网页面的密码后还有一份旧的在暗处生效 */
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    ESP_LOGI(TAG, "[WIFI] 连接 \"%s\"", ssid.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
        delay(200);     /* 原来这里每 200ms 打一个 '.' 当进度条,
                           但日志系统每条自成一行, 打点会变成几十条日志 —— 改成最后报耗时 */
    }

    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGE(TAG, "[WIFI] 连接失败 (status=%d, 等了 %u ms)",
                 (int)WiFi.status(), (unsigned)(millis() - t0));
        return false;
    }
    ESP_LOGI(TAG, "[WIFI] 已连接, IP=%s (耗时 %u ms)",
             WiFi.localIP().toString().c_str(), (unsigned)(millis() - t0));
    return true;
}

/* ==================== 配网页面 ====================
 * ESP32 上 PROGMEM 是空宏、flash 可以直接寻址, 这些 const char[] 不会占 RAM,
 * 所以 server.send(200, "...", 数组) 直传即可, 不需要 send_P。
 * HTML 属性一律用单引号, 免去 C 字符串里的转义。 */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM</title><style>"
    "body{font-family:sans-serif;margin:16px;max-width:420px;line-height:1.5}"
    "input,select,button{width:100%;padding:10px;margin:6px 0;"
    "box-sizing:border-box;font-size:16px}"
    "button{background:#0a7;color:#fff;border:0;border-radius:4px}"
    "a{color:#0a7}</style></head><body>";

static const char PAGE_FORM[] =
    "<h3>ESP32-CAM 配网</h3>"
    "<form method=POST action=/save>"
    "<input name=s placeholder='WiFi 名称 (SSID)' required>"
    "<input name=p type=password placeholder='WiFi 密码'>"
    "<button>保存并重启</button></form>"
    "<p><a href=/scan>扫描附近 WiFi</a></p></body></html>";

static const char PAGE_SAVED[] =
    "<h3>已保存</h3><p>设备正在重启并连接你的 WiFi。</p>"
    "<p>请把手机的网络切回原来的 WiFi, 屏幕会恢复取景。</p></body></html>";

/* SSID 里可能混进 < > & " ' 这类字符, 直接拼进 HTML 会把页面搞坏 */
static std::string htmlEscape(const std::string &in)
{
    std::string out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        switch (in[i]) {
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '&':  out += "&amp;";  break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&#39;";  break;
        default:   out += in[i];
        }
    }
    return out;
}

/* ==================== 配网主流程 ==================== */
void wifiRunSetupPortal(void (*statusCb)(const char *l1, const char *l2), uint32_t timeoutMs)
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);

    const IPAddress ip  = WiFi.softAPIP();
    const std::string    url = std::string("http://") + toStd(ip.toString());

    ESP_LOGI(TAG, "\n[WIFI] 进入配网模式 —— 热点 \"%s\", 浏览器打开 %s",
                  AP_SSID, url.c_str());
    if (statusCb) statusCb("WiFi Setup", url.c_str());

    /* captive portal: 把所有域名都解析到本机, 手机连上热点后一般会自动弹配网页 */
    DNSServer dns;
    dns.start(53, "*", ip);

    WebServer server(80);
    bool saved = false;

    server.on("/", HTTP_GET, [&server]() {
        std::string page = PAGE_HEAD;
        page += PAGE_FORM;
        server.send(200, "text/html; charset=utf-8", page.c_str());
    });

    server.on("/scan", HTTP_GET, [&server]() {
        const int n = WiFi.scanNetworks();        // 同步扫描, 约 2 秒
        std::string page = PAGE_HEAD;
        page += "<h3>选择 WiFi</h3><form method=POST action=/save>";
        if (n > 0) {
            page += "<select name=s>";
            for (int i = 0; i < n; i++) {
                const std::string ssid = toStd(WiFi.SSID(i));
                if (ssid.empty()) continue;
                page += "<option>" + htmlEscape(ssid) + "</option>";
            }
            page += "</select>";
        } else {
            page += "<p>没扫到网络, 手动输入吧:</p>";
            page += "<input name=s placeholder='WiFi 名称 (SSID)' required>";
        }
        WiFi.scanDelete();                        // 扫完立刻释放结果占的内存
        page += "<input name=p type=password placeholder='WiFi 密码'>";
        page += "<button>保存并重启</button></form>";
        page += "<p><a href=/>手动输入 SSID</a></p></body></html>";
        server.send(200, "text/html; charset=utf-8", page.c_str());
    });

    server.on("/save", HTTP_POST, [&server, &saved]() {
        std::string ssid = toStd(server.arg("s"));
        const std::string pass = toStd(server.arg("p"));
        strTrim(ssid);
        if (ssid.empty()) {
            server.send(400, "text/html; charset=utf-8",
                        "SSID 不能为空, <a href=/>返回</a>");
            return;
        }
        prefs.begin("wifi", false);
        prefs.putString("ssid", ssid.c_str());
        prefs.putString("pass", pass.c_str());
        prefs.end();

        /* 只打印 SSID, 密码不回显到串口 */
        ESP_LOGI(TAG, "[WIFI] 已保存凭据: ssid=\"%s\" 密码%s",
                      ssid.c_str(), pass.empty() ? "(空)" : "(已设置)");

        std::string page = PAGE_HEAD;
        page += PAGE_SAVED;
        server.send(200, "text/html; charset=utf-8", page.c_str());
        saved = true;
    });

    server.onNotFound([&server]() {               // 其它路径一律回首页
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });

    server.begin();

    const uint32_t t0 = millis();
    while (!saved && millis() - t0 < timeoutMs) {
        dns.processNextRequest();
        server.handleClient();
        delay(2);
    }

    server.stop();
    dns.stop();

    /* 两条路都重启: 保存后要靠重启让新凭据生效;
       超时重启是为了避免设备永远停在 AP 模式 —— 用户可能改主意了 */
    ESP_LOGI(TAG, "%s", saved ? "[WIFI] 凭据已保存, 重启中..."
                              : "[WIFI] 配网超时, 重启后重试");
    delay(600);
    ESP.restart();
}

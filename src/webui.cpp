#include "logcfg.h"   /* 必须第一行: 日志配置要在 <Arduino.h> 之前生效 */
#include "strutil.h"  // std::string 桥接

static const char *TAG = "webui";    // 日志标签

#include "webui.h"
#include "med.h"
#include "voice.h"
#include "vision.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer *s_server = nullptr;

/* ESP32 上 PROGMEM 是空宏, 这些常量本来就在 flash 里, 不占 RAM */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>用药记录</title><style>"
    "body{font-family:sans-serif;margin:16px;max-width:560px;line-height:1.6;color:#222}"
    "table{border-collapse:collapse;width:100%}"
    "td,th{border-bottom:1px solid #ddd;padding:8px 4px;text-align:left;font-size:15px}"
    "th{color:#666;font-weight:600}"
    "input,select,button{font-size:16px;box-sizing:border-box}"
    "input,select{width:100%;padding:9px;margin:4px 0;border:1px solid #bbb;border-radius:5px}"
    "button{background:#0a7;color:#fff;border:0;border-radius:5px;padding:11px 18px;margin-top:6px}"
    ".act a{display:inline-block;padding:5px 10px;margin-right:6px;border-radius:4px;"
    "text-decoration:none;font-size:14px;background:#eee;color:#036}"
    ".act a.d{background:#fdd;color:#900}"
    "h3{margin:0 0 12px}h4{margin:24px 0 8px;color:#444}"
    ".hint{color:#888;font-size:13px}</style></head><body>";

static std::string htmlEsc(const std::string &in)
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

/* ==================== 主页: 说话 + 列表 + 添加 ==================== */
static void handleRoot()
{
    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    std::string page = PAGE_HEAD;
    page += "<h3>用药记录</h3>";

    /* 说话框 —— 按 BOOT 说话是主入口, 这里打字是麦克风没接/坏了时的降级入口,
       两者走的是同一条处理链路(medHandleUtterance) */
    page += "<form method=POST action=/say>"
            "<input name=t placeholder='说一句: 「阿莫西林吃了」「阿莫西林一天两次」' required>"
            "<button>发送</button></form>"
            "<p class='hint'>主入口是按住 BOOT 说话。这里打字走同一条处理链路, "
            "结果同样会用喇叭读出来。</p>";

    /* 最近一次语音的结果 —— 屏幕上只显示最新一条, 这里能看到完整文本 */
    const VoiceResult vr = voiceLastResult();
    if (vr.stamp) {
        page += "<h4>最近一次语音</h4>";
        if (vr.ok && vr.text[0])
            page += "<p class='hint'>听到：「" + htmlEsc(std::string(vr.text)) + "」</p>";
        page += "<p>" + htmlEsc(std::string(vr.reply)) + "</p>";
    }

    if (n == 0) {
        page += "<p>还没有记录。在下面添加一条, 或者对着药盒短按 BOOT 拍照识别。</p>";
    } else {
        page += "<table><tr><th>药品</th><th>方案</th><th>上次服用</th><th></th></tr>";
        for (int i = 0; i < n; i++) {
            const std::string nm = htmlEsc(std::string(items[i].name));
            page += "<tr><td><b>" + nm + "</b></td><td>";
            if (items[i].perDay) {
                page += "一天 " + std::to_string((unsigned)items[i].perDay) + " 次";
                if (items[i].slots[0]) page += "<br><span class='hint'>" +
                                               htmlEsc(std::string(items[i].slots)) + "</span>";
            } else {
                page += "<span class='hint'>未设方案</span>";
            }
            page += "</td><td>";
            if (!medTimeReady() && items[i].lastTaken == 0)
                page += "<span class='hint'>时间未同步</span>";
            else
                page += medFmtTime(items[i].lastTaken);

            /* 确认弹窗里刻意不写药名 —— 药名里若含引号会把 JS 字符串截断 */
            page += "</td><td class='act'>"
                    "<a href='/taken?n=" + nm + "'>吃了</a>"
                    "<a class='d' href='/del?n=" + nm +
                    "' onclick=\"return confirm('确定删掉这条用药记录?')\">删除</a>"
                    "</td></tr>";
        }
        page += "</table>";
        page += "<p class='act'><a class='d' href='/clear' "
                "onclick=\"return confirm('清空全部用药记录? 这个操作不可撤销。')\">"
                "清空全部</a></p>";
    }

    page += "<h4>添加 / 修改方案</h4>"
            "<p class='hint'>药名写已存在的会更新它的方案, 不会动服用记录。</p>"
            "<form method=POST action=/add>"
            "<input name=n placeholder='药品名称, 如 阿莫西林' required>"
            "<input name=pd type=number min=1 max=10 value=2 placeholder='每天几次'>"
            "<input name=s placeholder='时段(可留空), 如 morning,evening'>"
            "<button>保存</button></form>"
            "<p class='hint'>提醒按「每天几次」算间隔, 时段只是备注。</p>";

    /* 有照片就给个入口 —— 调识别精度时要能亲眼看模型收到的是什么 */
    {
        size_t jlen = 0;
        if (visionLastJpeg(jlen) && jlen) {
            page += "<p class='act'><a href='/photo'>看最近拍的那张照片</a>"
                    "<span class='hint'>（模型认药名用的就是它）</span></p>";
        }
    }
    page += "</body></html>";

    s_server->send(200, "text/html; charset=utf-8", page.c_str());
}

/* ==================== 说一句 ==================== */
static void handleSay()
{
    const std::string t = toStd(s_server->arg("t"));
    const std::string result = medHandleUtterance(t.c_str());

    ESP_LOGI(TAG, "[MED] 输入「%s」-> %s", t.c_str(), result.c_str());

    /* 打字的也读出来 —— 这条路径本来就是给麦克风坏掉的情况兜底的,
       既然设备有喇叭, 结果就该能听见 */
    voiceSpeakAsync(result.c_str(), true);          // 用药提醒, 带免责

    std::string page = PAGE_HEAD;
    page += "<h3>结果</h3>";
    page += "<p>" + htmlEsc(result) + "</p>";
    page += "<p class='hint'>用药决策请以医嘱和说明书为准。</p>";
    page += "<p><a href='/'>← 返回用药记录</a></p></body></html>";
    s_server->send(200, "text/html; charset=utf-8", page.c_str());
}

/* ==================== 清空全部 ==================== */
static void handleClear()
{
    medClear();
    ESP_LOGI(TAG, "[MED] 已清空全部用药记录");
    s_server->sendHeader("Location", "/", true);
    s_server->send(303, "text/plain", "");
}

/* ==================== 添加 / 修改 ==================== */
static void handleAdd()
{
    std::string nm = toStd(s_server->arg("n"));
    strTrim(nm);
    if (nm.empty() || nm.length() >= MED_NAME_LEN) {
        s_server->send(400, "text/html; charset=utf-8",
                       "<meta charset=utf-8><p>药名不能为空或过长。<a href=/>返回</a></p>");
        return;
    }

    int pd = s_server->arg("pd").toInt();
    if (pd < 1)  pd = 1;
    if (pd > 10) pd = 10;

    std::string slots = toStd(s_server->arg("s"));
    strTrim(slots);

    const bool ok = medAdd(nm.c_str(), (uint8_t)pd, slots.c_str());
    ESP_LOGI(TAG, "[MED] %s 方案: 一天 %d 次 %s -> %s",
                  nm.c_str(), pd, slots.c_str(), ok ? "已保存" : "保存失败");

    s_server->sendHeader("Location", "/", true);
    s_server->send(303, "text/plain", "");
}

/* ==================== 标记已服用 ==================== */
static void handleTaken()
{
    const std::string nm = toStd(s_server->arg("n"));
    const bool ok = medMarkTaken(nm.c_str());
    ESP_LOGI(TAG, "[MED] 标记服用: %s -> %s (时间 %s)",
                  nm.c_str(), ok ? "已记录" : "没有这条记录",
                  medTimeReady() ? medFmtTime(medNow()).c_str() : "未同步");

    s_server->sendHeader("Location", "/", true);
    s_server->send(303, "text/plain", "");
}

/* ==================== 删除 ==================== */
static void handleDel()
{
    const std::string nm = toStd(s_server->arg("n"));
    const bool ok = medRemove(nm.c_str());
    ESP_LOGI(TAG, "[MED] 删除: %s -> %s", nm.c_str(), ok ? "已删" : "没有这条记录");

    s_server->sendHeader("Location", "/", true);
    s_server->send(303, "text/plain", "");
}

/* ==================== 生命周期 ==================== */
void webuiBegin()
{
    static WebServer server(80);        // 静态: 得活得比这个函数长
    s_server = &server;

    server.on("/",      HTTP_GET,  handleRoot);
    server.on("/add",   HTTP_POST, handleAdd);
    server.on("/taken", HTTP_GET,  handleTaken);
    server.on("/del",   HTTP_GET,  handleDel);
    server.on("/say",   HTTP_POST, handleSay);
    server.on("/clear", HTTP_GET,  handleClear);

    /* 最近一次拍的照片。正常模式下**不能**像 TEST 模式那样临时开
       另一个 80 端口服务器(两个 WebServer 抢同一端口会静默 bind 失败),
       所以走这条路由直接复用 vision 保留的那张 JPEG —— 零拷贝直发 PSRAM。 */
    server.on("/photo", HTTP_GET, []() {
        size_t len = 0;
        const uint8_t *jpg = visionLastJpeg(len);
        if (!jpg || !len) {
            s_server->send(404, "text/plain; charset=utf-8", "还没拍过照片");
            return;
        }
        s_server->send_P(200, "image/jpeg", (PGM_P)jpg, len);
    });

    /* 浏览器会自动来要 favicon, 不给它就在串口刷一条 "request handler not found" */
    server.on("/favicon.ico", HTTP_GET, []() { s_server->send(204, "text/plain", ""); });
    server.onNotFound([]() {
        s_server->sendHeader("Location", "/", true);
        s_server->send(303, "text/plain", "");
    });

    server.begin();
    ESP_LOGI(TAG, "[WEB] 用药记录页: http://%s", WiFi.localIP().toString().c_str());
}

void webuiLoop()
{
    if (s_server) s_server->handleClient();
}

#include "logcfg.h"     /* 必须第一行: 日志配置要在 <Arduino.h> 之前生效 */
#include "strutil.h"    /* std::string 与 Arduino String 的桥接 */
#include <errno.h>      /* errno: 写失败诊断用 */
#include <lwip/sockets.h>   /* 提速用的 setsockopt */

static const char *TAG = "httpio";    // 日志标签

#include "httpio.h"

/* ==================== 写 ==================== */
/* 单次 write() 喂给 WiFiClientSecure 的最大字节数。
 * 不要一次把剩下的大块整个喂进去: arduino-esp32 的 TLS 写实现
 * (send_ssl_data) 在连续大块写时容易卡住/半途丢连 —— issue #7356、
 * PR #11865 一类问题。社区验证过的稳妥做法是应用层自己分块
 * (1.3KB~4KB), 这里取 4KB; 如果仍然卡住, 先把这个值往小改(比如 1350)。 */
#define HTTP_WRITE_CHUNK 4096

bool httpWriteAllChunk(WiFiClientSecure &c, const uint8_t *data, size_t len, size_t chunk)
{
    size_t   sent = 0;
    uint32_t lastProgress = millis();
    const uint32_t t0 = millis();
    size_t   nextLogAt = 65536;
    bool     stallLogged = false;

    while (sent < len) {
        const size_t want = (len - sent > chunk) ? chunk : (len - sent);
        const size_t n = c.write(data + sent, want);
        if (n > 0) {
            sent += n;
            lastProgress = millis();
            if (sent >= nextLogAt) {
                ESP_LOGI(TAG, "[写] 已发 %u/%u 字节 (距开始 %u ms)",
                              (unsigned)sent, (unsigned)len, (unsigned)(millis() - t0));
                nextLogAt += 65536;
            }
            continue;
        }
        /* 卡住(n==0)时先看看对端发来了什么 —— 可能是 TLS 层记录(会话票据/
           告警), 也可能是服务端提前回的 HTTP 错误响应; 一直没人读的话,
           既看不到真正原因, mbedtls 也可能卡在等这些记录被处理上。
           读出来按可打印字符打进日志(纯诊断用) */
        if (c.available()) {
            uint8_t tmp[128];
            const size_t got = c.read(tmp, sizeof(tmp));
            char txt[129];
            for (size_t i = 0; i < got; i++) {
                const uint8_t ch = tmp[i];
                txt[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
            }
            txt[got] = 0;
            ESP_LOGW(TAG, "[写] 卡住期间读到对端 %u 字节: %s", (unsigned)got, txt);
        }
        if (!stallLogged) {
            stallLogged = true;
            ESP_LOGW(TAG, "[写] 首次受阻: 已发 %u/%u 字节, connected=%d, errno=%d",
                          (unsigned)sent, (unsigned)len, (int)c.connected(), errno);
        }
        if (!c.connected()) {
            ESP_LOGW(TAG, "[写] 连接断开, 已发 %u/%u 字节", (unsigned)sent, (unsigned)len);
            return false;                                      // 连接断了
        }
        const uint32_t stall = millis() - lastProgress;
        if (stall > 10000) {
            ESP_LOGW(TAG, "[写] 放弃: 距上次进展 %u ms, 已发 %u/%u 字节",
                          (unsigned)stall, (unsigned)sent, (unsigned)len);
            return false;                                      // 卡住不动了
        }
        delay(10);
    }
    return true;
}

bool httpWriteAll(WiFiClientSecure &c, const uint8_t *data, size_t len)
{
    return httpWriteAllChunk(c, data, len, HTTP_WRITE_CHUNK);
}

/* ==================== 连 ==================== */
bool httpConnect(WiFiClientSecure &c, const char *host, uint16_t port, uint32_t timeoutSec)
{
    /* setInsecure 必须在 connect 之前 —— 否则底层 start_ssl_client 会因为
       既没有 CA 证书又没声明 insecure 而直接失败, 连接根本建不起来 */
    c.setInsecure();
    c.setTimeout(timeoutSec);        // 单位是秒
    if (!c.connect(host, port)) return false;

    /* ---- 连上之后的提速设置 ----
       实测设备上行吞吐被卡在 ~12KB/s (窗口/RTT 限制): 默认 TCP 发送缓冲
       只有 ~5.7KB, 把发送缓冲调大让它多压些数据在途; 顺便关掉 Nagle。
       调不上去也只是维持原速, 不影响功能、不影响连接语义。 */
    c.setNoDelay(true);
    int sndBuf = 32768;
    lwip_setsockopt(c.fd(), SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
    return true;
}

/* ==================== 读头 ==================== */
int httpReadHeader(WiFiClientSecure &c, bool &chunked)
{
    int code = 0;
    chunked = false;

    for (int n = 0; n < 40; n++) {          // 防呆: header 不会超过 40 行
        /* 首字节要等模型推理完 —— 非流式接口是「先算完再发响应头」,
           模型思考 + 生成花 10~60 秒都算正常, 所以第一行给 90 秒。
           后面的 header 行是连着来的, 短超时足够 */
        const uint32_t waitMs = (n == 0) ? 90000 : 5000;
        const uint32_t t0 = millis();
        while (!c.available() && c.connected() && millis() - t0 < waitMs) delay(5);
        if (!c.available()) {
            if (n == 0) ESP_LOGE(TAG, "[HTTP] 等响应头超时 (对端没在 90 秒内回话)");
            break;
        }

        /* readStringUntil 是 Stream 的方法, 返回的就是 Arduino String ——
           这是必须过桥的边界, 拿到立刻转成 std::string, 后面全用它 */
        std::string line = toStd(c.readStringUntil('\n'));
        strTrim(line);
        if (line.empty()) break;                    // 空行 = header 结束

        /* ⚠️ HTTP 头名**大小写不敏感**, 服务端发 "transfer-encoding: chunked"
           也得认。这里原来按原样比 "Transfer-Encoding:", 一旦对端换个小写写法,
           chunked 就会被当成普通体 —— 解码全错, 而且**不报错**(只会看到 JSON 解析失败)。
           所以统一转小写再比。 */
        std::string low = line;
        for (size_t i = 0; i < low.size(); i++)
            low[i] = (char)tolower((unsigned char)low[i]);

        if (strStartsWith(low, "http/")) {
            const size_t sp = line.find(' ');
            if (sp != std::string::npos)
                code = strToInt(strSub(line, sp + 1, sp + 4));
        } else if (strStartsWith(low, "transfer-encoding:") &&
                   low.find("chunked") != std::string::npos) {
            chunked = true;
        }
    }
    return code;
}

/* ==================== 读体 ==================== */
std::string httpReadBody(WiFiClientSecure &c, uint32_t timeoutMs)
{
    std::string raw;
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        while (c.available()) raw += (char)c.read();
        if (!c.connected() && !c.available()) break;
        delay(5);
    }
    return raw;
}

std::string httpDecodeChunked(const std::string &raw)
{
    std::string out;
    size_t pos = 0;
    while (pos < raw.size()) {
        const int eol = strIndex(raw, "\r\n", pos);
        if (eol < 0) break;
        const long sz = strtol(strSub(raw, pos, (size_t)eol).c_str(), nullptr, 16);
        if (sz <= 0) break;
        pos = (size_t)eol + 2;
        if (pos + (size_t)sz > raw.size()) { out += strSub(raw, pos); break; }
        out += strSub(raw, pos, pos + (size_t)sz);
        pos += (size_t)sz + 2;    // 跳过数据块尾部的 \r\n
    }
    return out;
}

/* ==================== 取字段 ==================== */
std::string httpExtractStringField(const std::string &json, const char *key)
{
    /* 拼成 "key" 形式再找 —— 带引号是为了避开 reasoning_content
       这类「包含同名子串的别的键」被先匹配上 */
    std::string pat = "\"";
    pat += key;
    pat += "\"";

    const size_t k = json.find(pat);
    if (k == std::string::npos) return "";

    size_t i = json.find(':', k + pat.size());
    if (i == std::string::npos) return "";
    i++;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;
    if (i >= json.size() || json[i] != '"') return "";
    i++;

    std::string out;
    while (i < json.size()) {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            const char n = json[++i];
            switch (n) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case 'r':  out += '\r'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'u':
                /* \uXXXX -> UTF-8。智谱返回的中文一般直接就是 UTF-8,
                   但转义形式也得能处理, 否则会冒出乱码 */
                if (i + 4 < json.size()) {
                    const uint32_t cp =
                        strtoul(strSub(json, i + 1, i + 5).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) {
                        out += (char)cp;
                    } else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            default: out += n;   // \" \\ \/ 等
            }
            i++;
        } else if (c == '"') {
            break;               // 字符串结束
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

std::string httpExtractContent(const std::string &json)
{
    return httpExtractStringField(json, "content");
}

std::string httpJsonEscape(const char *s)
{
    std::string out;
    for (const char *p = s; p && *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        }
        /* 控制字符必须转义 —— JSON 里裸着换行/制表符就是语法错误。
           (文本来源是 ASR 转写和模型回答, 平时干净, 但不能指望它永远干净) */
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else if (c < 0x20)  {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
            out += b;
        }
        else { out += (char)c; }
    }
    return out;
}

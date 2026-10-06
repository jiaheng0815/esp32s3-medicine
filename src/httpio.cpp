#include "httpio.h"

/* ==================== 写 ==================== */
bool httpWriteAll(WiFiClientSecure &c, const uint8_t *data, size_t len)
{
    size_t   sent = 0;
    uint32_t lastProgress = millis();

    while (sent < len) {
        const size_t n = c.write(data + sent, len - sent);
        if (n > 0) {
            sent += n;
            lastProgress = millis();
            continue;
        }
        if (!c.connected()) return false;                      // 连接断了
        if (millis() - lastProgress > 10000) return false;      // 卡住不动了
        delay(10);
    }
    return true;
}

/* ==================== 连 ==================== */
bool httpConnect(WiFiClientSecure &c, const char *host, uint16_t port, uint32_t timeoutSec)
{
    /* setInsecure 必须在 connect 之前 —— 否则底层 start_ssl_client 会因为
       既没有 CA 证书又没声明 insecure 而直接失败, 连接根本建不起来 */
    c.setInsecure();
    c.setTimeout(timeoutSec);        // 单位是秒
    return c.connect(host, port);
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
            if (n == 0) Serial.println("[HTTP] 等响应头超时 (对端没在 90 秒内回话)");
            break;
        }

        String line = c.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) break;          // 空行 = header 结束
        if (line.startsWith("HTTP/")) {
            const int sp = line.indexOf(' ');
            if (sp > 0) code = line.substring(sp + 1, sp + 4).toInt();
        } else if (line.startsWith("Transfer-Encoding:") && line.indexOf("chunked") > 0) {
            chunked = true;
        }
    }
    return code;
}

/* ==================== 读体 ==================== */
String httpReadBody(WiFiClientSecure &c, uint32_t timeoutMs)
{
    String raw;
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        while (c.available()) raw += (char)c.read();
        if (!c.connected() && !c.available()) break;
        delay(5);
    }
    return raw;
}

String httpDecodeChunked(const String &raw)
{
    String out;
    int pos = 0;
    while (pos < (int)raw.length()) {
        const int eol = raw.indexOf("\r\n", pos);
        if (eol < 0) break;
        const long sz = strtol(raw.substring(pos, eol).c_str(), nullptr, 16);
        if (sz <= 0) break;
        pos = eol + 2;
        if (pos + sz > (int)raw.length()) { out += raw.substring(pos); break; }
        out += raw.substring(pos, pos + sz);
        pos += sz + 2;            // 跳过数据块尾部的 \r\n
    }
    return out;
}

/* ==================== 取字段 ==================== */
String httpExtractStringField(const String &json, const char *key)
{
    /* 拼成 "key" 形式再找 —— 带引号是为了避开 reasoning_content
       这类「包含同名子串的别的键」被先匹配上 */
    String pat = "\"";
    pat += key;
    pat += "\"";

    const int k = json.indexOf(pat);
    if (k < 0) return "";

    int i = json.indexOf(':', k + pat.length());
    if (i < 0) return "";
    i++;
    while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
    if (i >= (int)json.length() || json[i] != '"') return "";
    i++;

    String out;
    while (i < (int)json.length()) {
        const char c = json[i];
        if (c == '\\' && i + 1 < (int)json.length()) {
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
                if (i + 4 < (int)json.length()) {
                    const uint32_t cp =
                        strtoul(json.substring(i + 1, i + 5).c_str(), nullptr, 16);
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

String httpExtractContent(const String &json)
{
    return httpExtractStringField(json, "content");
}

String httpJsonEscape(const char *s)
{
    String out;
    for (const char *p = s; p && *p; p++) {
        if (*p == '"' || *p == '\\') out += '\\';
        out += *p;
    }
    return out;
}

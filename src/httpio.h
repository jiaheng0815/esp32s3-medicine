#pragma once
#include <Arduino.h>            /* 仍要 String: readStringUntil 返回的是它 */
#include <WiFiClientSecure.h>
#include <string>

/* 共享的 HTTPS 传输层。
 *
 * 这一层原本全在 vision.cpp 里以 static 形式躺着, 抽出来是因为语音识别(ASR)
 * 要用同一套东西 —— 它也是「拼 body → 循环写 → 收响应 → 取 content」的流程,
 * 只不过 body 从 JSON 换成了 multipart。 */

/* 循环把数据写满。
 *
 * WiFiClient::write() 单次最多只走一个 TLS 记录就返回 —— 记录大小受
 * CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN 限制(16384 字节)。所以发 30KB 的 base64
 * 调一次只会发出 16384 字节, 剩下的必须自己循环补上。
 * 内部带断连检测和「10 秒无进展」超时。 */
bool httpWriteAll(WiFiClientSecure &c, const uint8_t *data, size_t len);

/* 同上, 但可指定单次 write() 的块大小 (诊断/调参用) */
bool httpWriteAllChunk(WiFiClientSecure &c, const uint8_t *data, size_t len, size_t chunk);

/* 起一个跳过证书校验的 TLS 连接。
 * 注意 WiFiClientSecure::setTimeout 的单位是【秒】, 且必须在 connect 之前调用
 * 才会写进底层 socket 超时。 */
bool httpConnect(WiFiClientSecure &c, const char *host, uint16_t port, uint32_t timeoutSec);

/* 读 HTTP 响应头直到空行, 返回状态码; 是不是 chunked 用引用带出。
 * 首行给 90 秒 —— 非流式接口是「模型推理完才发响应头」, 想 10~60 秒都正常。 */
int httpReadHeader(WiFiClientSecure &c, bool &chunked);

/* 把剩余响应体读成字符串, 带总超时 */
std::string httpReadBody(WiFiClientSecure &c, uint32_t timeoutMs);

/* 解 Transfer-Encoding: chunked */
std::string httpDecodeChunked(const std::string &raw);

/* 从 JSON 里取一个字符串字段 (含 \uXXXX → UTF-8 转换)。
 *
 * 语音识别(ASR)用的是这个而不是下面的 httpExtractContent ——
 * ASR 的响应 schema 是 {id, created, request_id, model, text},
 * 转写文字在**顶层 text**, 不是 choices[0].message.content。
 * 拿错了不会报错, 只会永远得到空串, 特别难查。 */
std::string httpExtractStringField(const std::string &json, const char *key);

/* 从 OpenAI 风格响应里取 choices[0].message.content。
 * 等价于 httpExtractStringField(json, "content")。
 * 不引 ArduinoJson: 只需要一个字段, 手写反而更省 flash。 */
std::string httpExtractContent(const std::string &json);

/* 把字符串转义成能安全放进 JSON 字面量的形式:
 *   " \ -> 反斜杠转义;  \n \r \t -> 转义序列;  其余控制字符 -> \u00xx
 * (原来只处理 " 和 \, 文本里混进换行就会生成非法 JSON —— 文本来源是 ASR 转写和
 *  模型回答, 平时干净, 但不能指望它永远干净) */
std::string httpJsonEscape(const char *s);

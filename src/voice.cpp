#include "logcfg.h"   /* 必须第一行: 日志配置要在 <Arduino.h> 之前生效 */
#include "strutil.h"  /* std::string 与 Arduino String 的桥接 */

static const char *TAG = "voice";    // 日志标签

#include "voice.h"
#include "httpio.h"
#include "med.h"
#include "secrets.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ==================== 智谱接口 ==================== */
#define ZHIPU_HOST       "open.bigmodel.cn"
#define ASR_PATH         "/api/paas/v4/audio/transcriptions"
#define ASR_MODEL        "glm-asr-2512"
#define TTS_PATH         "/api/paas/v4/audio/speech"
#define TTS_MODEL        "glm-tts"
#define TTS_VOICE        "tongtong"     // 女声, 念提醒比男声柔和
#define TLS_TIMEOUT_S    20

/* multipart 分隔符: 含 '-' 而 base64 字母表(A-Za-z0-9+/)里没有 '-',
   所以哪怕把音频 base64 后拼进来也不会撞上 */
#define ASR_BOUNDARY     "----esp32s3camASR7f3a91"

/* ==================== ASR 热词 ====================
 * 提高药名这种专有名词的识别率。
 *
 * ⚠️ 2026-10-07 实测: 「苯磺酸氨氯地平片」被识别成「酸氯环利晴片」, 而**内置表里
 *    本来就有「氨氯地平」** —— 所以要么热词被服务端静默忽略了, 要么力度不够。
 *    下面这三档就是用来把这件事测清楚的:
 *
 *      0 = 不发 hotwords 字段   ← **对照实验**: 同一句话录两遍, 看转写有没有变化。
 *                                 没变化 = 服务端压根没理这个参数, 别再指望它
 *      1 = JSON 数组  ["阿莫西林"]  ← 接口文档(2026-10-07 核实)写的是 array, maxItems 100
 *      2 = 逗号分隔   阿莫西林,      ← 社区工具(zhipu-asr)用的写法
 *
 * 另外, 热词表 = **设备里自己存的药名** + 内置常用词。
 * 这里以前刻意不用用户药单, 理由是"别把健康信息随每次请求发给智谱"——
 * 那个理由其实早就不成立了: 药名已经有三条路进到智谱(药盒照片走 glm-4.6v、
 * 播报文本走 GLM-TTS、还有**用户嘴上说的那一句**, 就在同一个 ASR 请求的音频里)。
 * 而药名恰恰是热词最该起作用的地方 —— 用户会说的, 基本就是他表里存的那些。
 * (不想发就把 ASR_HOTWORDS_FMT 设成 0, 一并不发。) */
#define ASR_HOTWORDS_FMT  1

#if ASR_HOTWORDS_FMT
/* 常见说法(药名本身从 med 的 medCommonDrugs() 取, 一处定义两处用) */
static const char *const kAsrPhraseWords[] = {
    "一天两次", "一天三次", "早晚各一次", "饭前", "饭后",
    "吃了", "吃过了", "忘了",
};

/* 拼热词表: **用户存过的药名排前面**(最可能被说出口的), 再补内置药名和常见说法。
 * ⚠️ 要去重 —— 表里存了「阿莫西林」时它会和内置表撞, 实测日志里就出现过
 *    ["阿莫西林","阿莫西林",...]。重复热词纯属浪费额度。 */
static std::string buildHotwords()
{
    /* ⚠️ 只取 16 条, 不用 MED_MAX_ITEMS(40): 那是 2240 字节的栈数组, 而这条路径
       跑在语音 task 里(栈 12KB, 底下调用链也不浅)。16 条够覆盖家庭用药了。 */
    MedItem items[16];
    const int n = medLoad(items, 16);

    std::string out;
    const char *seen[100];
    int cnt = 0;

    auto add = [&](const char *w) {
        if (!w || !*w) return;
        for (int i = 0; i < cnt; i++) {
            if (strcmp(seen[i], w) == 0) return;      // 去重
        }
        if (cnt >= 100) return;                       // 接口上限 maxItems: 100
        if (cnt) out += ',';
#if ASR_HOTWORDS_FMT == 1
        out += '"';
        out += w;
        out += '"';
#else
        out += w;
#endif
        seen[cnt++] = w;
    };

    for (int i = 0; i < n; i++) add(items[i].name);

    int cdN = 0;
    const char *const *cd = medCommonDrugs(&cdN);
    for (int i = 0; i < cdN; i++) add(cd[i]);

    for (size_t i = 0; i < sizeof(kAsrPhraseWords) / sizeof(kAsrPhraseWords[0]); i++)
        add(kAsrPhraseWords[i]);

#if ASR_HOTWORDS_FMT == 1
    return "[" + out + "]";
#else
    return out;                                   // 逗号分隔, 本身就是拼好的
#endif
}
#endif  /* ASR_HOTWORDS_FMT */

/* TTS 播放缓冲。
 *
 * ⚠️ 512KB **不够** —— 2026-10-06 实测撑满还没完(见 CLAUDE.md 坑 #14;
 *    后来查明是**单声道** 48KB/s, 是文本比当年预想的长, 不是立体声)。
 *
 * 2MB -> **3MB**(2026-10-11): 识药问答改版后播报变长, 最长一档(药名+用途+
 * 添加引导+免责 ≈ 150 字)按实测语速约 3 字/秒 ≈ 50 秒 ≈ 2.3MB —— 正好压在 2MB 上,
 * 尾部会被 readChunkedToBuffer 截掉,**症状就是"吐字不完全"**。
 * 3MB = 62 秒, 留 20% 余量。
 * ⚠️ 别再加太大: 它是开机常驻的, 和拍照链路的 framebuffer/jpg/base64
 *    (峰值约 3.3MB) 同时存在于 8MB PSRAM 里。
 *
 * 多分配 1 字节是为了出错时能塞个 '\0' 把响应体当字符串打出来。 */
#define TTS_BUF_BYTES    (3 * 1024 * 1024)

/* 播报文本上限。100 -> 130(2026-10-11): 最长的一档是"药名+用途+添加引导"
 * (药名≤10字 + 用途~50字 + 引导~35字) + 免责 20 字 —— 100 会把引导语尾巴砍掉。
 * ⚠️ 三层截断点要一起看, 任何一层小了都表现为"播出的声音缺尾巴":
 *    ① voiceSpeakAsync 的 s_pending(VOICE_REPLY_MAX, voice.h)
 *    ② 这里(文本层)
 *    ③ TTS_BUF_BYTES(音频层)
 * 130 字 × 3 字节 = 390 字节, 所以 VOICE_REPLY_MAX 也同步加到了 512。 */
#define TTS_MAX_CHARS    130

/* 录音电平每块更新一次; 2048 字节 = 1024 采样 @16k = 64ms */
#define MIC_BLOCK_BYTES  2048

/* ==================== 缓冲与状态 ==================== */

/* 录音缓冲: [0..43] WAV 头, [44..63] 填充(对齐用), [64..] PCM。
 * 开机一次性分配、长期持有 —— 不能录一次申请一次: 摄像头的 deinit/init
 * 反复申请释放 960KB framebuffer, PSRAM 本来就在碎片化边缘。 */
static uint8_t *s_recBuf    = nullptr;
static size_t   s_recPcmLen = 0;        // 已录到的 PCM 字节数
static uint32_t s_peak      = 0;        // 整段峰值, 判断是不是全程静音
static int16_t  s_minVal    = 0, s_maxVal = 0;   // 整段极值, 判断数据线是不是死了
static uint32_t s_zeroCnt   = 0, s_nonZeroCnt = 0;

static uint8_t *s_ttsBuf = nullptr;     // TTS 下来的 WAV

enum VoiceState { VS_IDLE, VS_RECORDING, VS_UPLOAD, VS_SPEAK };
static volatile VoiceState s_state = VS_IDLE;

static SemaphoreHandle_t s_sem      = nullptr;
static VoiceUiCb         s_ui       = nullptr;
static VoiceResultCb     s_resultCb = nullptr;

/* 跨任务传递: 主线程投递播报文本 / 语音 task 回写结果 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char         s_pending[VOICE_REPLY_MAX];
static volatile bool s_pendingReady = false;
static volatile bool s_pendingDisclaimer = false;   // 本次播报要不要追加免责声明

static VoiceResult   s_last = {};

static void ui(const char *l1, const char *l2 = nullptr, int level = -1)
{
    if (s_ui) s_ui(l1, l2, level);
}

/* ==================== 小端读写 ==================== */
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ==================== 功放使能 ==================== */

/* MAX98357A 的 SD/EN: 拉高才工作。
 * 录音期间必须拉低 —— 见 voice.h 顶部关于自激的说明。 */
static void ampOn(bool on)
{
    gpio_set_level((gpio_num_t)I2S_PIN_AMP_SD, on ? 1 : 0);
}

/* ==================== I2S ====================
 * 录音和播报共用 BCLK/WS, 所以任何时刻只允许一个 I2S 存在:
 * 切换 = 先 uninstall 再按新方向 install。 */

static bool i2sInstallRx()
{
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = VOICE_RATE_HZ;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    /* INMP441 的 L/R 接 GND = 数据在左声道, 所以只收左声道 */
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 320;         // 320 帧 @16k = 20ms/块, 8 块 = 160ms 缓冲
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = false;
    cfg.fixed_mclk           = 0;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pin = {};
    pin.mck_io_num   = I2S_PIN_NO_CHANGE;
    pin.bck_io_num   = I2S_PIN_BCLK;
    pin.ws_io_num    = I2S_PIN_WS;
    pin.data_out_num = I2S_PIN_NO_CHANGE;   // RX 不驱动数据脚
    pin.data_in_num  = I2S_PIN_MIC_SD;

    if (i2s_set_pin(I2S_NUM_0, &pin) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    return true;
}

static bool i2sInstallTx(uint32_t rate)
{
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = rate;        // GLM-TTS 返回 24kHz
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    /* 立体声 + 手动把 mono 展开成 L/R 交错(见 playPcm)。
     *
     * MAX98357A 的 SD 拉高时工作在 (L+R)/2 模式, 只发单声道而右声道为 0
     * 的话, 混音后音量直接减半, 症状是"能响但明显偏小" —— 很容易误判成
     * 功放模块本身的问题。
     *
     * (SDK 里 I2S_CHANNEL_FMT_ALL_LEFT 会打开硬件的 tx_mono + tx_chan_equal,
     *  让硬件自己复制到两个 slot, 省掉这次拷贝。但那个行为依赖 HAL 内部实现,
     *  这里是"任何版本都成立"的写法, 24kHz 下多一次 memcpy 可以忽略。) */
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;        // 欠载自动补 0, 避免末尾爆音
    cfg.fixed_mclk           = 0;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pin = {};
    pin.mck_io_num   = I2S_PIN_NO_CHANGE;
    pin.bck_io_num   = I2S_PIN_BCLK;
    pin.ws_io_num    = I2S_PIN_WS;
    pin.data_out_num = I2S_PIN_SPK_DIN;
    pin.data_in_num  = I2S_PIN_NO_CHANGE;   // TX 不读数据脚

    if (i2s_set_pin(I2S_NUM_0, &pin) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    return true;
}

static void i2sUninstall()
{
    i2s_driver_uninstall(I2S_NUM_0);
}

static void i2sWriteAll(const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        size_t n = 0;
        if (i2s_write(I2S_NUM_0, data + sent, len - sent, &n, pdMS_TO_TICKS(1000)) != ESP_OK) break;
        if (!n) break;
        sent += n;
    }
}

/* ==================== WAV ==================== */

/* 填 64 字节 WAV 头: 标准 44 字节 + 一个 20 字节的 JUNK 对齐块。
 *
 * JUNK 是 RIFF 里合法存在的填充块, 标准解析器会跳过它。用它(而不是随便
 * 塞 20 字节垃圾)的原因是: data chunk 必须**如实**声明自己在 56 偏移,
 * 否则解析方会算错 PCM 起点 —— 表现成开头几声咔哒 + 末尾丢一截,
 * 而且看起来像"编码有问题", 完全指不到对齐上去。
 *
 * 布局:  0..11 RIFF | 12..35 fmt | 36..55 JUNK | 56..63 data 头 | 64.. PCM
 * PCM 落在 64 = 16 字节对齐, 满足 GDMA 访问 PSRAM 的要求。 */
static void wavWriteHeader(uint8_t *buf, size_t pcmBytes)
{
    const uint16_t ch   = 1;
    const uint16_t bits = 16;
    const uint32_t rate = VOICE_RATE_HZ;

    memcpy(buf + 0,  "RIFF", 4);
    put32(buf + 4,  56 + (uint32_t)pcmBytes);   // 4(WAVE)+24(fmt)+20(JUNK)+8(data)+pcm
    memcpy(buf + 8,  "WAVEfmt ", 8);
    put32(buf + 16, 16);                        // fmt chunk 长度
    put16(buf + 20, 1);                         // 1 = PCM
    put16(buf + 22, ch);
    put32(buf + 24, rate);
    put32(buf + 28, rate * ch * (bits / 8));    // 字节率
    put16(buf + 32, ch * (bits / 8));           // 块对齐
    put16(buf + 34, bits);

    memcpy(buf + 36, "JUNK", 4);                // 对齐块 —— 让 data 的数据正好落在 64
    put32(buf + 40, 12);                        // 长度 12(chunk 按偶数字节对齐)
    memset(buf + 44, 0, 12);

    memcpy(buf + 56, "data", 4);
    put32(buf + 60, (uint32_t)pcmBytes);
    /* 64 起就是 PCM —— 录音时直接写在这个偏移上 */
}

struct WavInfo {
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    size_t   dataOff;
    size_t   dataLen;
};

/* 扫 chunk 链表找 fmt / data。
 * ⚠️ 不能假设 data 就在偏移 44 —— 中间可能夹着 LIST/fact 之类的 chunk,
 * 那样会把 chunk 头当音频喂进功放(变成噪音), 或者漏掉真正的数据。 */
static bool wavParse(const uint8_t *p, size_t len, WavInfo &info)
{
    info.rate = 0; info.channels = 0; info.bits = 0;
    info.dataOff = 0; info.dataLen = 0;

    if (len < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0)
        return false;

    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = p + pos;
        const uint32_t sz = get32(p + pos + 4);

        if (memcmp(id, "fmt ", 4) == 0 && pos + 8 + 16 <= len) {
            info.channels = get16(p + pos + 8 + 2);
            info.rate     = get32(p + pos + 8 + 4);
            info.bits     = get16(p + pos + 8 + 14);
        } else if (memcmp(id, "data", 4) == 0) {
            info.dataOff = pos + 8;
            info.dataLen = sz;
            if (info.dataOff + info.dataLen > len) info.dataLen = len - info.dataOff;
        }
        pos += 8 + sz + (sz & 1);       // chunk 之间按偶数字节对齐
    }
    return info.rate > 0 && info.dataLen > 0;
}

/* ==================== 电平与统计 ==================== */

static void updateStats(const uint8_t *pcm, size_t bytes, uint32_t &rmsOut)
{
    const int16_t *s = (const int16_t *)pcm;
    const size_t   n = bytes / 2;
    if (!n) { rmsOut = 0; return; }

    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = s[i];
        acc += (uint32_t)(v * v);
        const uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
        if (a > s_peak) s_peak = a;
        if (s[i] < s_minVal) s_minVal = s[i];
        if (s[i] > s_maxVal) s_maxVal = s[i];
        if (s[i] == 0) s_zeroCnt++; else s_nonZeroCnt++;
    }
    uint32_t rms = (uint32_t)sqrt((double)acc / (double)n);
    rmsOut = rms > 32767 ? 32767 : rms;
}

/* 0..100, 对数刻度: -60dBFS -> 0, 0dBFS -> 100。人耳是对数的, 线性刻度看着不动 */
static int levelPercent(uint32_t rms)
{
    if (rms < 4) return 0;
    const float db = 20.0f * log10f((float)rms / 32768.0f);
    int v = (int)((db + 60.0f) * 100.0f / 60.0f);
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

/* ASCII 波形: 每列取一段的峰值, 映射成字符高度。自检时用 */
static void printWaveform(const int16_t *s, size_t n)
{
    static const char bar[] = " .:-=+*#%@";
    const int cols = 60;
    if (n < (size_t)cols) return;

    const size_t step = n / cols;

    /* 整行拼好再一次性输出 —— ESP_LOG 每条自成一行, 不能像 Serial.print
       那样分几段"接着打"(那样会变成好几条独立日志, 波形就散了)。 */
    char line[80];
    int  p = 0;
    line[p++] = '[';
    for (int c = 0; c < cols; c++) {
        uint32_t peak = 0;
        for (size_t i = 0; i < step; i++) {
            const int32_t v = s[(size_t)c * step + i];
            const uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
            if (a > peak) peak = a;
        }
        int idx = (int)((uint64_t)peak * 9 / 32768);
        if (idx > 9) idx = 9;
        line[p++] = bar[idx];
    }
    line[p++] = ']';
    line[p]   = '\0';

    ESP_LOGI(TAG, "%s", line);
}

/* ==================== 播放 ==================== */

static bool playWavBuffer(const uint8_t *wav, size_t len, bool interruptible)
{
    WavInfo wi;
    if (!wavParse(wav, len, wi)) {
        ESP_LOGI(TAG, "[VOICE] WAV 解析失败");
        return false;
    }

    const uint32_t bytesPerFrame = (wi.bits ? wi.bits / 8 : 2) * (wi.channels ? wi.channels : 1);
    ESP_LOGI(TAG, "[TTS] WAV: %u Hz, %u 声道, %u bit, 音频 %.1f 秒 (总 %u 字节)",
                  (unsigned)wi.rate, (unsigned)wi.channels, (unsigned)wi.bits,
                  (double)wi.dataLen / ((double)wi.rate * bytesPerFrame),
                  (unsigned)len);

    /* ★ 装 I2S 之前就先确保功放关断 —— 不能只指望"上次播报结束时会关":
       万一上次是异常退出(中途 return false), 功放还开着, I2S 一上电的
       那批 DMA 垃圾就会被直接放出来。 */
    ampOn(false);
    delay(10);
    ESP_LOGI(TAG, "[TTS] [%u ms] 功放已关断, 装 I2S", millis());

    if (!i2sInstallTx(wi.rate)) {
        ESP_LOGI(TAG, "[VOICE] I2S 播放模式安装失败");
        return false;
    }

    /* ⚠️ 顺序不能反: 先把 I2S 环形缓冲整个灌满静音, **再**开功放。
     *
     * 三个因素叠在一起会放出一阵"嘟嘟"声(像摩斯电码):
     *   ① i2s_driver_install() 之后 DMA 环形缓冲是**未初始化的随机内存**,
     *      而 I2S 一启动就立刻开始"播放"它
     *   ② 环形缓冲是 dma_buf_count × dma_buf_len = 8 × 256 帧 ≈ 8KB,
     *      只灌一小段(原来的 20ms=1.9KB)根本盖不住, 剩下的垃圾照样漏出来
     *   ③ 功放要是先使能, 这些垃圾就被直接放大了
     * 所以: 灌满(20KB 远超 8KB) -> 再 ampOn。 */
    {
        static int16_t silence[256 * 2] = {0};      // 256 帧立体声 = 1KB
        size_t filled = 0;
        for (int i = 0; i < 20; i++) {              // 20KB
            i2sWriteAll((const uint8_t *)silence, sizeof(silence));
            filled += sizeof(silence);
        }
        ESP_LOGI(TAG, "[TTS] [%u ms] 静音已灌 %u 字节 (环形缓冲只有 8KB)",
                      millis(), (unsigned)filled);
    }

    ampOn(true);            // ★ 缓冲填满静音之后才使能功放
    delay(10);
    ESP_LOGI(TAG, "[TTS] [%u ms] 功放已开, 开始播真实音频", millis());

    const uint8_t *pcm = wav + wi.dataOff;
    const size_t   n   = wi.dataLen;

    if (wi.channels == 1 && wi.bits == 16) {
        /* mono -> L/R 交错展开 */
        static uint8_t chunk[4 * 1024];
        const int16_t *src      = (const int16_t *)pcm;
        const size_t   samples  = n / 2;
        const size_t   perChunk = sizeof(chunk) / 4;

        size_t done = 0;
        while (done < samples) {
            size_t m = samples - done;
            if (m > perChunk) m = perChunk;

            int16_t *dst = (int16_t *)chunk;
            for (size_t i = 0; i < m; i++) {
                dst[i * 2]     = src[done + i];
                dst[i * 2 + 1] = src[done + i];
            }
            i2sWriteAll(chunk, m * 4);
            done += m;

            /* 播报中途按一下 BOOT 就能打断 —— 4~8 秒听着不能停会让人烦 */
            if (interruptible && buttonDown()) {
                ESP_LOGI(TAG, "[VOICE] 用户按键, 打断播报");
                break;
            }
        }
    } else {
        i2sWriteAll(pcm, n);        // 立体声/非 16bit: 原样透传
    }

    /* 等 DMA 排空再拆驱动, 否则尾巴被切掉。
       环形缓冲 = dma_buf_count × dma_buf_len 帧 */
    const uint32_t ringMs = (uint32_t)(8 * 256) * 1000UL / wi.rate;
    delay(ringMs + 60);

    ampOn(false);           // 先关功放, 再拆 I2S
    i2sUninstall();
    return true;
}

/* 生成正弦波直接喂功放 —— 单独验证功放链路, 不牵扯麦克风。
 * 加了 10ms 淡入淡出: 突然起停会有"啪"的爆音, 容易被误当成故障。 */
static bool testTone(uint32_t freq, uint32_t ms)
{
    const uint32_t rate = 24000;
    if (!i2sInstallTx(rate)) return false;

    /* 同 playWavBuffer: 先灌满静音再开功放, 否则 DMA 环形缓冲里的
       随机内存会先响一阵"嘟嘟"声, 然后才是正音 */
    {
        static int16_t silence[256 * 2] = {0};
        for (int i = 0; i < 10; i++)
            i2sWriteAll((const uint8_t *)silence, sizeof(silence));
    }

    ampOn(true);
    delay(5);

    static int16_t buf[512 * 2];            // 512 帧立体声交错
    const size_t   frames      = 512;
    const size_t   totalFrames = (size_t)rate * ms / 1000;
    const size_t   ramp        = rate / 100;    // 10ms

    for (size_t done = 0; done < totalFrames; done += frames) {
        size_t n = totalFrames - done;
        if (n > frames) n = frames;

        for (size_t i = 0; i < n; i++) {
            const size_t k = done + i;
            double gain = 1.0;
            if (k < ramp)                     gain = (double)k / ramp;
            else if (k + ramp > totalFrames)  gain = (double)(totalFrames - k) / ramp;

            const int16_t v = (int16_t)(sin(2.0 * M_PI * freq * k / rate) * 8000.0 * gain);
            buf[i * 2]     = v;
            buf[i * 2 + 1] = v;
        }
        i2sWriteAll((const uint8_t *)buf, n * 4);
    }

    delay((uint32_t)(8 * 256) * 1000UL / rate + 60);
    ampOn(false);
    i2sUninstall();
    return true;
}

/* ==================== ASR 上传 (multipart/form-data) ==================== */

static std::string mpTextField(const char *name, const char *value)
{
    std::string s;
    s.reserve(strlen(name) + strlen(value) + 96);
    s += "--"; s += ASR_BOUNDARY; s += "\r\n";
    s += "Content-Disposition: form-data; name=\""; s += name; s += "\"\r\n\r\n";
    s += value;
    s += "\r\n";
    return s;
}

/* file part 的头。真正的数据(64 字节 WAV 头 + PCM)后面分两次发 */
static std::string mpFileHead()
{
    std::string s;
    s.reserve(160);
    s += "--"; s += ASR_BOUNDARY; s += "\r\n";
    s += "Content-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n";
    s += "Content-Type: audio/wav\r\n\r\n";
    return s;
}

static std::string mpTail()
{
    std::string s;
    s.reserve(64);
    s += "\r\n--"; s += ASR_BOUNDARY; s += "--\r\n";
    return s;
}

/* ⚠️ 上传分块**必须小** —— 见 CLAUDE.md 坑 #16。
 * 本 core 的 mbedtls 出方向只要单个 TLS 记录"大"到一定程度(实测 ≥1350 字节)
 * 就会被对端静默掐死, 而 httpWriteAll() 的默认块是 HTTP_WRITE_CHUNK=4096,
 * 直接拿它传录音**必挂**, 日志长这样(2026-10-07 实测):
 *   [写] 首次受阻: 已发 4096/143360 字节, connected=0, errno=11
 *   [ASR] 请求体写到一半断了 (已发 818 / 144211 字节)
 * 64 是实测最小可用值, 和 vision.cpp 的 uploadChunkGuess 保持一致。
 * 代价是慢: 设备上行天花板约 12KB/s, 144KB 的录音要约 12 秒。 */
#define ASR_UPLOAD_CHUNK 64

/* 上传录音拿转写结果。
 * 用 file 分片而不是 file_base64: 省掉一份 427KB 的 base64 缓冲。 */
static bool asrTranscribe(const uint8_t *recBuf, size_t pcmLen, std::string &outText)
{
    outText.clear();

    std::string prefix;
    prefix += mpTextField("model", ASR_MODEL);
    prefix += mpTextField("stream", "false");
#if ASR_HOTWORDS_FMT
    const std::string hw = buildHotwords();
    /* 把实际发出去的热词原样打出来 —— 上机时一眼就能确认"到底发没发、格式对不对",
       不用靠猜(这条就是被"表里有氨氯地平却还是听错"逼出来的) */
    ESP_LOGI(TAG, "[ASR] hotwords(%u 字节): %s", (unsigned)hw.size(), hw.c_str());
    prefix += mpTextField("hotwords", hw.c_str());
#else
    /* 对照档: 完全不发 hotwords 字段, 用来判断服务端到底认不认这个参数 */
    ESP_LOGI(TAG, "[ASR] hotwords: 关闭(对照实验)");
#endif

    const std::string fileHead = mpFileHead();
    const std::string tail     = mpTail();

    /* file part 的内容 = 64 字节 WAV 头(含 JUNK 对齐块) + PCM。
       头本身就占满到 64, 中间没有要跳过的填充, 所以直接相加 */
    const size_t filePayload = VOICE_WAV_HDR + pcmLen;

    /* Content-Length 不手数 —— 差 1 字节服务端就挂起或回 400,
       而设备上极难查是长度问题。这里算出来, 发完再核对实际写出量 */
    const size_t bodyLen = prefix.length() + fileHead.length() + filePayload
                         + tail.length() + 2 /* file part 结尾的 \r\n */;

    WiFiClientSecure client;
    ui("ASR upload...", nullptr, -1);
    if (!httpConnect(client, ZHIPU_HOST, 443, TLS_TIMEOUT_S)) {
        ESP_LOGI(TAG, "[ASR] TLS 连接失败");
        return false;
    }

    client.printf("POST %s HTTP/1.1\r\n", ASR_PATH);
    client.printf("Host: %s\r\n", ZHIPU_HOST);
    client.printf("Authorization: Bearer %s\r\n", ZHIPU_API_KEY);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", ASR_BOUNDARY);
    client.printf("Content-Length: %u\r\n", (unsigned)bodyLen);
    client.print("Connection: close\r\n\r\n");

    /* 分多次发, 不拼成一个大 buffer: 录音就是那块 PSRAM 缓冲,
       拼的话要多占 320KB 还多一次整段拷贝 */
    size_t sent = 0;
    bool   ok   = true;

    auto wr = [&](const uint8_t *p, size_t n) {
        if (!ok) return;
        if (!httpWriteAllChunk(client, p, n, ASR_UPLOAD_CHUNK)) { ok = false; return; }
        sent += n;
    };

    wr((const uint8_t *)prefix.c_str(),   prefix.length());
    wr((const uint8_t *)fileHead.c_str(), fileHead.length());
    wr(recBuf, VOICE_WAV_HDR);                          // 64 字节 WAV 头(含 JUNK 对齐块)
    wr(recBuf + VOICE_PCM_OFF, pcmLen);                 // PCM —— 零拷贝, 直接从 PSRAM 发
    wr((const uint8_t *)"\r\n", 2);
    wr((const uint8_t *)tail.c_str(),     tail.length());

    if (!ok) {
        ESP_LOGI(TAG, "[ASR] 请求体写到一半断了 (已发 %u / %u 字节)",
                      (unsigned)sent, (unsigned)bodyLen);
        return false;
    }
    ESP_LOGI(TAG, "[ASR] 请求体 %u 字节 (预期 %u) %s",
                  (unsigned)sent, (unsigned)bodyLen,
                  sent == bodyLen ? "一致" : "!! 不一致, 服务端多半会挂起");

    bool chunked = false;
    const int code = httpReadHeader(client, chunked);
    std::string body = httpReadBody(client, 30000);
    if (chunked) body = httpDecodeChunked(body);

    if (code != 200) {
        ESP_LOGE(TAG, "[ASR] HTTP %d", code);
        ESP_LOGE(TAG, "%s", strSub(body, 0, 512).c_str());
        return false;
    }

    /* ⚠️ 转写文字在**顶层 text**, 不是 choices[0].message.content。
       用 httpExtractContent() 取的话会永远得到空串, 而且不报任何错。 */
    outText = httpExtractStringField(body, "text");
    ESP_LOGI(TAG, "[ASR] 转写: 「%s」", outText.c_str());
    if (outText.empty())
        ESP_LOGW(TAG, "[ASR] 没取到 text 字段, 原始响应: %s",
                 strSub(body, 0, 512).c_str());
    return !outText.empty();
}

/* ==================== TTS ==================== */

/* 把响应体读进缓冲, 返回字节数。
 * 不走 String: 二进制数据(含 0x00)过 String 又慢又容易出问题。 */
static size_t readRawToBuffer(WiFiClientSecure &c, uint8_t *dst, size_t cap, uint32_t idleMs)
{
    size_t   out  = 0;
    uint32_t last = millis();

    while (out < cap) {
        if (c.available()) {
            size_t want = cap - out;
            if (want > 4096) want = 4096;
            const int r = c.read(dst + out, want);
            if (r > 0) { out += r; last = millis(); continue; }
        }
        if (!c.connected()) break;
        if (millis() - last > idleMs) break;
        delay(5);
    }
    return out;
}

/* 解 chunked 直接解进缓冲 —— httpDecodeChunked 吃 String, 不适合二进制。
 *
 * ⚠️ 除了"无进展超时"还加了**总时长保底**。之前只靠前者: 一旦 read() 返回 0
 *    而 available() 又一直为真, 内层循环会空转, 永远出不来(实测卡死过,
 *    而且症状是"设备不重启但彻底没反应", 极难查)。 */
static size_t readChunkedToBuffer(WiFiClientSecure &c, uint8_t *dst, size_t cap, uint32_t idleMs)
{
    size_t   out  = 0;
    uint32_t last = millis();
    const uint32_t hardLimit = millis() + 90000;         // 总时长保底
    int      chunks  = 0;
    bool     timedOut = false;

    while (out < cap) {
        if (millis() >= hardLimit) { timedOut = true; break; }

        if (!c.available()) {
            if (!c.connected()) break;
            if (millis() - last > idleMs) { timedOut = true; break; }
            delay(5);
            continue;
        }

        /* Stream::readStringUntil 返回的就是 Arduino String —— 边界处过桥 */
        std::string line = toStd(c.readStringUntil('\n'));   // chunk 长度行, 纯 ASCII
        strTrim(line);
        const long sz = strtol(line.c_str(), nullptr, 16);
        if (sz <= 0) break;                              // 0 = 结束块

        chunks++;
        if (chunks <= 4)                                 // 只报前几个, 免得刷屏
            ESP_LOGI(TAG, "[TTS] chunk#%d %ld 字节 (累计 %u)",
                          chunks, sz, (unsigned)out);

        size_t got = 0;
        while (got < (size_t)sz) {
            /* 内层也要有出口 —— 就是这里以前会空转 */
            if (millis() >= hardLimit || millis() - last > idleMs) { timedOut = true; break; }
            if (!c.available()) {
                if (!c.connected()) break;
                delay(2);
                continue;
            }
            size_t want = (size_t)sz - got;
            if (want > cap - out) want = cap - out;
            if (want == 0) break;                        // 缓冲满了 —— 不加这句 read(ptr,0) 恒返回 0, 内层空转
            const int r = c.read(dst + out, want);
            if (r > 0) { out += r; got += r; last = millis(); }
        }
        if (got < (size_t)sz) break;                     // 这一段没读全, 别再往下走
        c.readStringUntil('\n');                         // 块尾的 \r\n
    }

    /* ⚠️ 撞到缓冲上限原来是**静默** break 的 —— 症状是"播出来缺尾巴",
       但日志上看不出原因。现在明确报警, 上机一看就知道该加大 TTS_BUF_BYTES。 */
    const bool bufFull = (out >= cap);
    ESP_LOGI(TAG, "[TTS] chunked 解出 %u 字节, 共 %d 块%s%s",
                  (unsigned)out, chunks,
                  bufFull ? " (!! 缓冲满, 音频被截断 —— 加大 TTS_BUF_BYTES)" : "",
                  timedOut ? " (超时退出!)" : "");
    return out;
}

/* 播报文本的加工: 截断 (+ 可选免责)。
 * 免责是药品场景的安全边界 —— 输出要是"陈述 + 免责", 不是指令。
 * 但它只对用药提醒有意义: 测试语句、纯记录类("已记下这次服用")跟一句
 * "请以医嘱为准"听着莫名其妙。所以由调用方决定, 不在这里无差别追加。 */
static std::string speakTextFrom(const char *reply, bool addDisclaimer)
{
    std::string s(reply ? reply : "");
    strTrim(s);
    if (s.empty()) return s;

    /* 太长截断。UTF-8 中文一字 3 字节, 按字节截可能截出半个汉字, 往前退到完整字符 */
    const size_t maxBytes = (size_t)TTS_MAX_CHARS * 3;
    if (s.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && ((uint8_t)s[cut] & 0xC0) == 0x80) cut--;
        s = s.substr(0, cut);
    }

    if (addDisclaimer) s += "。以上仅供参考，请以医嘱和说明书为准。";
    return s;
}

static bool ttsSpeak(const char *reply, bool addDisclaimer)
{
    if (!reply || !*reply) return true;
    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGI(TAG, "[TTS] WiFi 没连, 跳过播报");
        return false;
    }
    if (!s_ttsBuf) return false;

    const std::string say  = speakTextFrom(reply, addDisclaimer);
    std::string body = "{\"model\":\"" TTS_MODEL "\",\"input\":\"";
    body += httpJsonEscape(say.c_str());
    body += "\",\"voice\":\"" TTS_VOICE "\",\"response_format\":\"wav\",\"speed\":1.0}";

    WiFiClientSecure client;
    ESP_LOGI(TAG, "[TTS] 文本 %u 字节, 请求体 %u 字节",
                  (unsigned)say.size(), (unsigned)body.size());
    ui("Speaking...", nullptr, -1);

    ESP_LOGI(TAG, "[TTS] TLS 连接中...");
    if (!httpConnect(client, ZHIPU_HOST, 443, TLS_TIMEOUT_S)) {
        ESP_LOGI(TAG, "[TTS] TLS 连接失败");
        return false;
    }
    ESP_LOGI(TAG, "[TTS] TLS 已建立");

    client.printf("POST %s HTTP/1.1\r\n", TTS_PATH);
    client.printf("Host: %s\r\n", ZHIPU_HOST);
    client.printf("Authorization: Bearer %s\r\n", ZHIPU_API_KEY);
    client.print("Content-Type: application/json\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)body.size());
    client.print("Connection: close\r\n\r\n");

    if (!httpWriteAll(client, (const uint8_t *)body.c_str(), body.size())) {
        ESP_LOGI(TAG, "[TTS] 请求体发送失败");
        return false;
    }
    ESP_LOGI(TAG, "[TTS] 请求已发出, 等响应头 (最长 90 秒)...");

    bool chunked = false;
    const int code = httpReadHeader(client, chunked);
    ESP_LOGI(TAG, "[TTS] 响应码 %d, chunked=%d", code, (int)chunked);

    if (code != 200) {
        std::string err = httpReadBody(client, 10000);
        if (chunked) err = httpDecodeChunked(err);
        ESP_LOGE(TAG, "[TTS] HTTP %d: %s", code, strSub(err, 0, 256).c_str());
        return false;
    }

    const size_t total = chunked
        ? readChunkedToBuffer(client, s_ttsBuf, TTS_BUF_BYTES, 30000)
        : readRawToBuffer(client, s_ttsBuf, TTS_BUF_BYTES, 30000);

    if (total < 44) {
        ESP_LOGI(TAG, "[TTS] 响应只有 %u 字节", (unsigned)total);
        return false;
    }

    /* ⚠️ 用 RIFF 魔数判断而不是 Content-Type: 服务端出错时回的是 JSON 错误体,
       而 Content-Type 不一定跟着变 */
    if (memcmp(s_ttsBuf, "RIFF", 4) != 0) {
        const size_t dbg = total < 200 ? total : 200;
        s_ttsBuf[dbg] = 0;
        ESP_LOGI(TAG, "[TTS] 不是 WAV, 开头是: %s", (const char *)s_ttsBuf);
        return false;
    }

    return playWavBuffer(s_ttsBuf, total, true);
}

/* ==================== 一次完整的语音会话 ==================== */

static void storeResult(bool ok, const char *text, const char *reply)
{
    VoiceResult r = {};
    r.ok    = ok;
    r.stamp = millis() ? millis() : 1;
    if (text)  strlcpy(r.text,  text,  sizeof(r.text));
    if (reply) strlcpy(r.reply, reply, sizeof(r.reply));

    portENTER_CRITICAL(&s_mux);
    s_last = r;
    portEXIT_CRITICAL(&s_mux);
}

/* 存结果 + 上屏。屏幕有中文点阵, 结果应该留在屏上 ——
 * 这个功能面向的正是"不方便掏手机"的场合。 */
static void publishResult(bool ok, const char *heard, const char *reply)
{
    storeResult(ok, heard, reply);
    if (s_resultCb) s_resultCb(heard ? heard : "", reply ? reply : "");
}

/* 录音 -> ASR -> 本地判断 -> 播报。全程在语音 task 里跑。 */
static void recordAndHandle()
{
    s_recPcmLen  = 0;
    s_peak       = 0;
    s_minVal     = 32767;
    s_maxVal     = -32768;
    s_zeroCnt    = 0;
    s_nonZeroCnt = 0;
    s_state      = VS_RECORDING;

    /* ★ 录音前先关功放。
       RX 模式下 BCLK/WS 一直在翻转, MAX98357A 看到时钟就工作, 而 DIN
       此刻是悬空的 —— 它会把噪声放大出来, 再被 INMP441 录回去(自激)。 */
    ampOn(false);

    if (!i2sInstallRx()) {
        ESP_LOGI(TAG, "[VOICE] I2S 录音模式安装失败");
        ui("Mic FAIL", "i2s install", -1);
        s_state = VS_IDLE;
        return;
    }

    ui("Recording", "hold to talk", 0);

    const size_t maxPcm = (size_t)VOICE_RATE_HZ * 2 * VOICE_MAX_SEC;
    uint32_t lastUiMs = 0;

    while (s_recPcmLen < maxPcm) {
        size_t want = maxPcm - s_recPcmLen;
        if (want > MIC_BLOCK_BYTES) want = MIC_BLOCK_BYTES;

        size_t got = 0;
        if (i2s_read(I2S_NUM_0, s_recBuf + VOICE_PCM_OFF + s_recPcmLen, want, &got,
                     pdMS_TO_TICKS(500)) != ESP_OK) {
            ESP_LOGI(TAG, "[VOICE] i2s_read 出错");
            break;
        }
        if (got) {
            uint32_t rms = 0;
            updateStats(s_recBuf + VOICE_PCM_OFF + s_recPcmLen, got, rms);
            s_recPcmLen += got;

            /* 电平条每块都更新, 但屏幕刷新限到 ~10Hz, 别让 SPI 拖累 I2S */
            const uint32_t now = millis();
            if (now - lastUiMs >= 100) {
                lastUiMs = now;
                ui(nullptr, nullptr, levelPercent(rms));
            }
        }

        /* 松手就停 —— 这就是必须有独立 task 的原因:
           主循环的按键检测只有 ~15Hz, 而且拍照/上传期间干脆不跑 */
        if (!buttonDown()) break;
    }

    i2sUninstall();

    const double secs = (double)s_recPcmLen / (VOICE_RATE_HZ * 2);
    ESP_LOGI(TAG, "[VOICE] 录到 %u 字节 (%.1f 秒), 峰值 %u",
                  (unsigned)s_recPcmLen, secs, (unsigned)s_peak);

    if (s_recPcmLen < (size_t)VOICE_RATE_HZ) {       // < 0.5 秒 = 误触
        ESP_LOGI(TAG, "[VOICE] 太短, 忽略");
        ui("Too short", nullptr, -1);
        s_state = VS_IDLE;
        return;
    }

    /* ---- 静音 / 断线判定: 这一层挡掉的是「接线有问题」而不是「识别不准」 ---- */

    /* 整段一个值都不跳 = 数据线根本没动 */
    const bool lineDead = (s_maxVal == s_minVal) ||
                          (s_nonZeroCnt * 100 < (s_nonZeroCnt + s_zeroCnt));

    if (lineDead) {
        ESP_LOGI(TAG, "[VOICE] 麦克风数据线没有输出(整段恒定或全 0)");
        ESP_LOGI(TAG, "        查: L/R 是否接了 GND / VDD 是否 3V3 / SD 是否在 GPIO40");
        ui("Mic DEAD", "check wiring", -1);
        publishResult(false, "", "麦克风没有输出。检查接线：L/R 接 GND，VDD 接 3V3，SD 接 GPIO40。");
        s_state = VS_IDLE;
        return;
    }

    if (s_peak < VOICE_SILENT_PEAK) {
        /* 有数据但全是底噪。拦下来: 不浪费一次 API 调用,
           也免得人以为是"识别不准"而去调 prompt */
        ESP_LOGI(TAG, "[VOICE] 整段几乎没有声音 (峰值 %u)", (unsigned)s_peak);
        ui("No sound", "speak louder", -1);
        publishResult(false, "", "没听到声音，请靠近麦克风再说一次。");
        s_state = VS_IDLE;
        return;
    }

    wavWriteHeader(s_recBuf, s_recPcmLen);

    /* ---- ASR ---- */
    s_state = VS_UPLOAD;
    std::string text;
    if (!asrTranscribe(s_recBuf, s_recPcmLen, text)) {
        ESP_LOGI(TAG, "[VOICE] ASR 失败");
        ui("ASR failed", "see serial", -1);
        publishResult(false, "", "语音识别失败，看串口日志。");
        s_state = VS_IDLE;
        return;
    }

    /* ---- 本地判断 ----
       复用已有的处理链路: 和网页文本框 POST /say 是同一个函数,
       所以「记录服药 / 记方案 / 查询该怎么吃」三种说法在这里全都能用 */
    const std::string reply = medHandleUtterance(text.c_str());
    ESP_LOGI(TAG, "[VOICE] 「%s」-> %s", text.c_str(), reply.c_str());

    /* 先存(网页可能正在读), 播报完再上屏 —— 否则结果页会被
       ttsSpeak 里的 "Speaking..." 提示覆盖掉, 最后停在提示页上没意义 */
    storeResult(true, text.c_str(), reply.c_str());

    s_state = VS_SPEAK;
    ttsSpeak(reply.c_str(), true);          // 用药提醒: 带免责

    if (s_resultCb) s_resultCb(text.c_str(), reply.c_str());
    s_state = VS_IDLE;
}

/* 只播报不录音 —— 拍照 / 网页打字那条路走这里 */
static void speakOnly(const char *text, bool addDisclaimer)
{
    s_state = VS_SPEAK;
    ttsSpeak(text, addDisclaimer);
    s_state = VS_IDLE;
}

/* ==================== 语音 task ====================
 * 开机创建一次, 之后永远挂在信号量上, 不退出。
 * 会话全程(录音/上传/播报)都在这里跑, 主循环只管"看到 voiceBusy()
 * 就不取景、不刷屏、不碰按键"。 */
static void voiceTask(void *)
{
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;

        char pending[VOICE_REPLY_MAX];
        bool hasPending = false;
        bool disclaimer = false;
        portENTER_CRITICAL(&s_mux);
        if (s_pendingReady) {
            memcpy(pending, s_pending, sizeof(pending));
            disclaimer     = s_pendingDisclaimer;
            s_pendingReady = false;
            hasPending     = true;
        }
        portEXIT_CRITICAL(&s_mux);

        if (hasPending) speakOnly(pending, disclaimer);
        else            recordAndHandle();
    }
}

/* ==================== 对外接口 ==================== */

bool voiceBegin(VoiceUiCb ui)
{
    s_ui = ui;

    const size_t recBytes = VOICE_PCM_OFF + (size_t)VOICE_RATE_HZ * 2 * VOICE_MAX_SEC;

    /* 开机一次性分配、长期持有 —— 防 PSRAM 碎片化。
       16 字节对齐是给 I2S 的 DMA 用的(虽然老 API 走 ringbuffer 中转,
       但对齐本身零成本, 没必要赌)。 */
    s_recBuf = (uint8_t *)heap_caps_aligned_alloc(16, recBytes, MALLOC_CAP_SPIRAM);
    s_ttsBuf = (uint8_t *)heap_caps_aligned_alloc(16, TTS_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!s_recBuf || !s_ttsBuf) {
        ESP_LOGI(TAG, "[VOICE] PSRAM 缓冲分配失败 (录音 %u KB / 播放 %u KB)",
                      (unsigned)(recBytes / 1024), (unsigned)(TTS_BUF_BYTES / 1024));
        return false;
    }

    /* 功放使能脚: 默认拉低(关), 只有播报时才开 */
    gpio_config_t amp = {};
    amp.pin_bit_mask = 1ULL << I2S_PIN_AMP_SD;
    amp.mode         = GPIO_MODE_OUTPUT;
    amp.pull_up_en   = GPIO_PULLUP_DISABLE;
    amp.pull_down_en = GPIO_PULLDOWN_ENABLE;    // 悬空/复位期间也保持关断
    amp.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&amp);
    ampOn(false);

    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) {
        ESP_LOGI(TAG, "[VOICE] 信号量创建失败");
        return false;
    }

    /* core 1、优先级 3: core 0 上已经有 cam_task 和 lwIP; 优先级高于 loopTask(1),
       远低于 WiFi 的 ~23, 只在语音会话那几秒真正占用。
       栈给 8192: TLS 握手的调用链很深。 */
    /* ⚠️ 栈给到 12KB。这条 task 里最深的一条路是 medHandleUtterance/adviceFor,
       那里有一个 2240 字节的 MedItem 数组(见 med.cpp), 再加上 std::string 和
       一层层 ESP_LOG —— 8KB 时余量已经不多, 栈溢出又只表现为莫名其妙的崩溃。 */
    if (xTaskCreatePinnedToCore(voiceTask, "voice", 12288, nullptr, 3, nullptr, 1) != pdPASS) {
        ESP_LOGI(TAG, "[VOICE] 语音 task 创建失败");
        return false;
    }

    ESP_LOGI(TAG, "[VOICE] 就绪: 录音缓冲 %u KB, 播放缓冲 %u KB (PSRAM)",
                  (unsigned)(recBytes / 1024), (unsigned)(TTS_BUF_BYTES / 1024));
    return true;
}

bool voiceStartRec()
{
    if (s_state != VS_IDLE || !s_sem) return false;
    xSemaphoreGive(s_sem);
    return true;
}

bool voiceBusy()
{
    return s_state != VS_IDLE;
}

bool voiceSpeakAsync(const char *text, bool addDisclaimer)
{
    if (!text || !*text) return true;
    if (!s_sem) return false;

    portENTER_CRITICAL(&s_mux);
    strlcpy(s_pending, text, sizeof(s_pending));
    s_pendingDisclaimer = addDisclaimer;
    s_pendingReady = true;
    portEXIT_CRITICAL(&s_mux);

    xSemaphoreGive(s_sem);
    return true;
}

VoiceResult voiceLastResult()
{
    VoiceResult r;
    portENTER_CRITICAL(&s_mux);
    r = s_last;
    portEXIT_CRITICAL(&s_mux);
    return r;
}

void voiceSetResultCb(VoiceResultCb cb) { s_resultCb = cb; }

/* ==================== 硬件自检 ==================== */

void voiceSelfTest()
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========== 麦克风 / 功放 自检 ==========");
    ESP_LOGI(TAG, "接线: INMP441   VDD=3V3 GND=GND SCK=41 WS=42 SD=40 L/R=GND");
    ESP_LOGI(TAG, "      MAX98357A VIN=5V  BCLK=41 LRC=42 DIN=39 SD=38 SPK±=喇叭");
    ESP_LOGI(TAG, "");

    if (!s_recBuf) {
        ESP_LOGI(TAG, "[自检] 录音缓冲没分配 —— 要先调 voiceBegin()");
        for (;;) delay(1000);
    }

    /* ---- ① 先验功放 ----
       麦克风和功放是两个独立模块, 分开验才定位得了问题。先验功放是因为
       它只有一条路(生成波形 -> I2S -> 喇叭), 变量最少。 */
    ESP_LOGI(TAG, "[自检 ①] 放 1kHz 测试音 1 秒 —— 应该听到「嘀」的一声");
    if (testTone(1000, 1000)) {
        ESP_LOGI(TAG, "        已播放。没声音就是**功放侧**的问题, 检查:");
        ESP_LOGI(TAG, "          VIN 是不是 5V / SD(GPIO38) 有没有接到模块的 SD/EN");
        ESP_LOGI(TAG, "          SPK± 是不是接在喇叭两端(都不能接 GND —— 会烧功放)");
    } else {
        ESP_LOGI(TAG, "        !! I2S 播放模式安装失败");
    }

    delay(1200);

    /* ---- ② 录音 + ③ 回放, 循环 ---- */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[自检 ②] 按住 BOOT 说话, 松手结束。会打波形, 然后用喇叭回放出来。");
    ESP_LOGI(TAG, "        退出自检: 把 voice.h 的 MIC_SELFTEST 改成 0 重新烧录");
    ESP_LOGI(TAG, "");

    for (;;) {
        while (!buttonDown()) delay(20);                   // 等按下
        delay(30);                                                          // 消抖

        ESP_LOGI(TAG, "[自检] 录音中... 松手结束");
        ampOn(false);                       // 录音期间关功放(自激)
        if (!i2sInstallRx()) {
            ESP_LOGI(TAG, "[自检] !! I2S 录音模式安装失败");
            while (buttonDown()) delay(20);
            continue;
        }

        s_recPcmLen = 0;
        s_peak = 0; s_minVal = 32767; s_maxVal = -32768;
        s_zeroCnt = 0; s_nonZeroCnt = 0;

        const size_t maxPcm = (size_t)VOICE_RATE_HZ * 2 * VOICE_MAX_SEC;
        while (s_recPcmLen < maxPcm) {
            size_t want = maxPcm - s_recPcmLen;
            if (want > MIC_BLOCK_BYTES) want = MIC_BLOCK_BYTES;

            size_t got = 0;
            if (i2s_read(I2S_NUM_0, s_recBuf + VOICE_PCM_OFF + s_recPcmLen, want, &got,
                         pdMS_TO_TICKS(500)) != ESP_OK) break;
            if (got) {
                uint32_t rms = 0;
                updateStats(s_recBuf + VOICE_PCM_OFF + s_recPcmLen, got, rms);
                s_recPcmLen += got;
            }
            if (!buttonDown()) break;
        }
        i2sUninstall();

        /* 等松手 —— 否则回放期间还按着的手会被当成下一次录音 */
        while (buttonDown()) delay(20);

        ESP_LOGI(TAG, "[自检] 录到 %u 字节 (%.1f 秒), 峰值 %u, 范围 [%d, %d], 非零 %u/%u",
                      (unsigned)s_recPcmLen,
                      (double)s_recPcmLen / (VOICE_RATE_HZ * 2),
                      (unsigned)s_peak, (int)s_minVal, (int)s_maxVal,
                      (unsigned)s_nonZeroCnt, (unsigned)(s_nonZeroCnt + s_zeroCnt));
        ESP_LOGI(TAG, "        (整段范围不跳 / 非零占比极低 = 数据线没输出)");

        if (s_recPcmLen < (size_t)VOICE_RATE_HZ) {
            ESP_LOGI(TAG, "        太短, 忽略");
            continue;
        }

        printWaveform((const int16_t *)(s_recBuf + VOICE_PCM_OFF), s_recPcmLen / 2);

        /* 录完立刻回放 —— 有声音就说明录音和播放两条路都通了。
           听音高: 明显低沉/时长翻倍 = 采样率配置不对(16k 录的按别的速率放的)。 */
        wavWriteHeader(s_recBuf, s_recPcmLen);
        ESP_LOGI(TAG, "[自检] 回放:");
        if (!playWavBuffer(s_recBuf, VOICE_WAV_HDR + s_recPcmLen, false)) {
            ESP_LOGI(TAG, "        !! 回放失败");
        }

        ESP_LOGI(TAG, "\n------ 准备好就再按一次 BOOT ------");
    }
}

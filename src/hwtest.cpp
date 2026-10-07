#include "logcfg.h"   /* 必须第一行: 日志配置要在 <Arduino.h> 之前生效 */

static const char *TAG = "hwtest";    // 日志标签

#include "pins.h"
#include "hwtest.h"
#include "cnfont.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 引脚用 src/pins.h 里的 I2S_PIN_* —— 和 voice.cpp 是同一套硬件,
   以前这里各自抄一份, 换脚改漏一处就变成「屏幕正常但麦克风没数据」。
   ⚠️ 屏幕上的提示文字别手写脚号, 用 PIN_STR(...) 自动跟宏走。 */

#define HT_RATE_HZ      16000   // 麦克风采样率
#define HT_BLOCK_BYTES  2048    // 一次读 64ms 音频(1024 采样)
#define HT_COLS         16      // 一次读画多少列 -> 扫满全屏要 10 次读 = 640ms

/* 颜色直接用 RGB565 数值, 免得为了几个宏去 include Adafruit 的显示屏头 */
static const uint16_t C_BG    = 0x0000;
static const uint16_t C_GRID  = 0x4208;     // 暗灰
static const uint16_t C_WAVE  = 0x07E0;     // 绿
static const uint16_t C_LOUD  = 0xF800;     // 红(接近削顶时)
static const uint16_t C_TEXT  = 0xFFFF;
static const uint16_t C_DIM   = 0x8410;
static const uint16_t C_BLACK = 0x0000;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_GREEN = 0x07E0;
static const uint16_t C_BLUE  = 0x001F;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_CYAN  = 0x07FF;
static const uint16_t C_YELLOW = 0xFFE0;

/* ==================== 功放使能 ====================
 * 录音时 BCLK/WS 一直在翻转, 而 DIN 悬空 —— 功放会把噪声放大出来
 * 再被麦克风录回去(自激)。所以测试音放完必须立刻关掉。 */
static void ampOn(bool on)
{
    gpio_set_level((gpio_num_t)I2S_PIN_AMP_SD, on ? 1 : 0);
}

static void ampInit()
{
    gpio_config_t g = {};
    g.pin_bit_mask = 1ULL << I2S_PIN_AMP_SD;
    g.mode         = GPIO_MODE_OUTPUT;
    g.pull_down_en = GPIO_PULLDOWN_ENABLE;      // 复位期间也保持关断
    gpio_config(&g);
    ampOn(false);
}

/* ==================== ⓪ 引脚电平探测 ====================
 * 在装 I2S 之前, 先把几个信号脚切成输入、分别开内部上拉和下拉各读一次。
 *
 *   两次读数不同 → 引脚悬空/高阻 = 正常(没接东西, 或者接的是输入脚)
 *   两次都读到同一个值 → 引脚被外部钉死: 拉低多半是短路到 GND 或线接错,
 *                        拉高多半是接到了 VCC
 *
 * 这一条能抓到「线插错排针」「模块内部短路」这类靠看代码永远看不出的问题。 */
static void probePins()
{
    struct { int pin; const char *name; } list[] = {
        { I2S_PIN_MIC_SD,  "MIC_SD  (" PIN_STR(I2S_PIN_MIC_SD)  ")" },
        { I2S_PIN_BCLK,    "BCLK    (" PIN_STR(I2S_PIN_BCLK)    ")" },
        { I2S_PIN_WS,      "WS      (" PIN_STR(I2S_PIN_WS)      ")" },
        { I2S_PIN_SPK_DIN, "SPK_DIN (" PIN_STR(I2S_PIN_SPK_DIN) ")" },
        { I2S_PIN_AMP_SD,  "AMP_SD  (" PIN_STR(I2S_PIN_AMP_SD)  ")" },
    };

    ESP_LOGI(TAG, "\n[HW ⓪] 引脚静态电平探测");
    ESP_LOGI(TAG, "        (上拉/下拉各读一次; 两次不同=悬空正常, 相同=被外部钉死)");

    for (auto &e : list) {
        gpio_config_t c = {};

        c.pin_bit_mask = 1ULL << e.pin;
        c.mode         = GPIO_MODE_INPUT;
        c.pull_up_en   = GPIO_PULLUP_ENABLE;
        c.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&c);
        delay(3);
        const int up = gpio_get_level((gpio_num_t)e.pin);

        c.pull_up_en   = GPIO_PULLUP_DISABLE;
        c.pull_down_en = GPIO_PULLDOWN_ENABLE;
        gpio_config(&c);
        delay(3);
        const int dn = gpio_get_level((gpio_num_t)e.pin);

        const char *verdict;
        if (up != dn)          verdict = "悬空/高阻 (正常)";
        else if (up == 0)      verdict = "!! 被钉在低电平 —— 短路到 GND 或接错脚了";
        else                   verdict = "!! 被钉在高电平 —— 接到 VCC 或接错脚了";

        ESP_LOGI(TAG, "        %-12s 上拉=%d 下拉=%d  ->  %s", e.name, up, dn, verdict);
    }

    /* 探测完把脚恢复成输入, 后面装 I2S 时会重新配置 */
    for (auto &e : list) {
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << e.pin;
        c.mode         = GPIO_MODE_INPUT;
        gpio_config(&c);
    }
    ampInit();      // AMP_SD 要恢复成输出, 否则功放使能脚悬空
    ESP_LOGI(TAG, "");
}

/* ==================== I2S 开/关 ==================== */

static bool i2sRxOn(i2s_channel_fmt_t fmt)
{
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = HT_RATE_HZ;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = fmt;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 320;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = false;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pin = {};
    pin.mck_io_num   = I2S_PIN_NO_CHANGE;
    pin.bck_io_num   = I2S_PIN_BCLK;
    pin.ws_io_num    = I2S_PIN_WS;
    pin.data_out_num = I2S_PIN_NO_CHANGE;    // RX 不驱动数据脚
    pin.data_in_num  = I2S_PIN_MIC_SD;

    if (i2s_set_pin(I2S_NUM_0, &pin) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    return true;
}

static bool i2sTxOn(uint32_t rate)
{
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = rate;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    /* 立体声: MAX98357A 按 (L+R)/2 混音, 只发单声道的话音量会减半 */
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pin = {};
    pin.mck_io_num   = I2S_PIN_NO_CHANGE;
    pin.bck_io_num   = I2S_PIN_BCLK;
    pin.ws_io_num    = I2S_PIN_WS;
    pin.data_out_num = I2S_PIN_SPK_DIN;
    pin.data_in_num  = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(I2S_NUM_0, &pin) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    return true;
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

/* ==================== ① 屏幕 ==================== */

static void testScreen(Adafruit_GFX &g)
{
    const int W = g.width();
    const int H = g.height();

    ESP_LOGI(TAG, "\n[HW ①] 屏幕 —— 依次画 5 张图, 每张停几秒");
    ESP_LOGI(TAG, "        尺寸 %dx%d", W, H);

    /* [a] 纯色慢闪 —— 每色 500ms 走两轮。
         屏幕有故障时这几帧是唯一的"活体证据", 给慢了才看得清。 */
    ESP_LOGI(TAG, "        [a] 纯色闪光: 红/绿/蓝/白 各 0.5 秒, 走 2 轮");
    const uint16_t sweep[] = { C_RED, C_GREEN, C_BLUE, C_WHITE };
    for (int round = 0; round < 2; round++) {
        for (uint16_t c : sweep) {
            g.fillScreen(c);
            delay(500);
        }
    }

    /* [b] 彩条 —— 看颜色顺序对不对(红绿蓝白黄青品黑) */
    ESP_LOGI(TAG, "        [b] 彩条: 红绿蓝白黄青品黑");
    {
        const uint16_t bars[] = { C_RED, C_GREEN, C_BLUE, C_WHITE, C_YELLOW, C_CYAN, 0xF81F, C_BLACK };
        for (int i = 0; i < 8; i++)
            g.fillRect(i * W / 8, 0, W / 8 + 1, H, bars[i]);
    }
    delay(3000);

    /* [c] 棋盘格 —— 看有没有缺行/缺列(线松的典型症状) */
    ESP_LOGI(TAG, "        [c] 棋盘格: 看有没有缺行/缺列");
    {
        const int cell = 16;
        for (int y = 0; y < H; y += cell)
            for (int x = 0; x < W; x += cell)
                g.fillRect(x, y, cell, cell, (((x / cell) + (y / cell)) & 1) ? C_WHITE : C_BLACK);
    }
    delay(3000);

    /* [d] 边框 + 十字 + 四角色块 —— 看整体有没有偏移/缩放 */
    ESP_LOGI(TAG, "        [d] 边框+十字+四角: 红左上/绿右上/蓝左下/白右下");
    g.fillScreen(C_BLACK);
    g.drawRect(0, 0, W, H, C_WHITE);
    g.drawFastHLine(0, H / 2, W, C_GREEN);
    g.drawFastVLine(W / 2, 0, H, C_GREEN);
    g.fillRect(0, 0, 10, 10, C_RED);
    g.fillRect(W - 10, 0, 10, 10, C_GREEN);
    g.fillRect(0, H - 10, 10, 10, C_BLUE);
    g.fillRect(W - 10, H - 10, 10, 10, C_WHITE);
    delay(3000);

    /* [e] 文字页 —— 中文走点阵字库, 能画出来说明 cnfont 也正常 */
    ESP_LOGI(TAG, "        [e] 文字页: 中英文");
    g.fillScreen(C_BG);
    g.setTextColor(C_TEXT, C_BG);
    g.setTextSize(1);
    g.setCursor(4, 4);   g.print("SCREEN TEST");
    g.setCursor(4, 22);  g.print("Colors OK?");
    cnDraw(g, 4, 44, "中文显示测试", C_CYAN);
    cnDraw(g, 4, 64, "能看到这行字", C_YELLOW);
    cnDraw(g, 4, 84, "屏幕就是好的", C_YELLOW);
    delay(3000);

    ESP_LOGI(TAG, "        判读:");
    ESP_LOGI(TAG, "          五张里任何一张看得到  -> 屏幕和 SPI 线是通的");
    ESP_LOGI(TAG, "          全白/全黑/毫无变化      -> 屏幕侧没通");
    ESP_LOGI(TAG, "            查 VCC=5V(不是3V3!) / GND / SCLK=21 / SDA=14 / CS=47 / DC=48 / RES=1");
    ESP_LOGI(TAG, "          有图案但缺行缺列/偏移  -> 某根信号线接触不良");
    ESP_LOGI(TAG, "          颜色顺序不对            -> 面板 tab 不对(TFT_TAB)");
}

/* ==================== ② 功放 ==================== */

static bool testSpeaker(Adafruit_GFX &g)
{
    const uint32_t rate = 24000;

    ESP_LOGI(TAG, "\n[HW ②] 功放 —— 放 1kHz 测试音 1 秒");

    if (!i2sTxOn(rate)) {
        ESP_LOGI(TAG, "       !! I2S 播放模式装不上");
        return false;
    }

    /* 先灌满静音再开功放 —— 原因见 voice.cpp 的 playWavBuffer():
       i2s_driver_install 之后 DMA 环形缓冲是未初始化的随机内存,
       直接开功放会先放出一阵"嘟嘟"声(像摩斯电码), 然后才是正音。 */
    {
        static int16_t silence[256 * 2] = {0};
        for (int i = 0; i < 10; i++)
            i2sWriteAll((const uint8_t *)silence, sizeof(silence));
    }

    ampOn(true);
    delay(5);

    static int16_t buf[512 * 2];            // 512 帧立体声交错
    const size_t frames = 512;
    const size_t total  = (size_t)rate * 1000 / 1000;   // 1000ms
    const size_t ramp   = rate / 100;                   // 10ms 淡入淡出, 免得"啪"一声

    for (size_t done = 0; done < total; done += frames) {
        size_t n = total - done;
        if (n > frames) n = frames;

        for (size_t i = 0; i < n; i++) {
            const size_t k = done + i;
            double gain = 1.0;
            if (k < ramp)                    gain = (double)k / ramp;
            else if (k + ramp > total)       gain = (double)(total - k) / ramp;

            const int16_t v = (int16_t)(sin(2.0 * M_PI * 1000.0 * k / rate) * 8000.0 * gain);
            buf[i * 2]     = v;
            buf[i * 2 + 1] = v;
        }
        i2sWriteAll((const uint8_t *)buf, n * 4);
    }

    delay((uint32_t)(8 * 256) * 1000UL / rate + 60);    // 等 DMA 排空, 否则尾巴被切
    ampOn(false);
    i2s_driver_uninstall(I2S_NUM_0);

    ESP_LOGI(TAG, "       听到「嘀」= 功放通过");
    ESP_LOGI(TAG, "       没声音查: VIN 是不是 5V / SD(GPIO38) 有没有接到模块的 SD|EN");
    ESP_LOGI(TAG, "                 SPK± 是不是接在喇叭两端(都不能接 GND)");
    return true;
}

/* ==================== ③ 麦克风: 屏幕实时波形 ==================== */

static void testMicWave(Adafruit_GFX &g)
{
    const int W = g.width();
    const int H = g.height();

    ESP_LOGI(TAG, "\n[HW ③] 麦克风 —— 屏幕上实时画声波");
    ESP_LOGI(TAG, "        对着麦克风说话, 波形应该跟着动。不用按键。");
    ESP_LOGI(TAG, "        串口每秒一行, 判读方法:");
    ESP_LOGI(TAG, "          全0 占绝大多数   -> 数据线没信号(没接 / SD 接错脚 / VDD 没供电)");
    ESP_LOGI(TAG, "          全FF 占绝大多数  -> 数据线被拉高(接错到 VCC 或 SD 短路到 3V3)");
    ESP_LOGI(TAG, "          其他值占多数     -> 有数据流; 再看说话时 rms 会不会变大");
    ESP_LOGI(TAG, "          rms 随说话变大   -> 麦克风正常");
    ESP_LOGI(TAG, "        ⚠ 读数是恒定的 全0 或 全FF 时, 先怀疑接线(接触不良/插错排针),");
    ESP_LOGI(TAG, "          再怀疑硬件 —— 两者症状完全一样。重插一遍是最快的验证。");

    uint8_t *buf = (uint8_t *)heap_caps_malloc(HT_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGI(TAG, "       !! PSRAM 分配失败");
        g.fillScreen(C_BG);
        g.setTextColor(C_RED, C_BG);
        g.setCursor(4, 4);
        g.print("No PSRAM!");
        for (;;) delay(1000);
    }

    ampOn(false);                   // ★ 录音期间必须关功放, 否则自激
    if (!i2sRxOn(I2S_CHANNEL_FMT_ONLY_LEFT)) {
        ESP_LOGI(TAG, "       !! I2S 录音模式装不上");
        g.fillScreen(C_BG);
        g.setTextColor(C_RED, C_BG);
        g.setCursor(4, 4);
        g.print("i2s install FAIL");
        for (;;) delay(1000);
    }

    /* ---- 布局 ---- */
    const int topY  = 14;                       // 顶部状态行让出来
    const int waveH = H - topY - 34;
    const int midY  = topY + waveH / 2;
    const int halfH = waveH / 2 - 2;
    const int barY  = topY + waveH + 4;
    const int barH  = 10;

    g.fillScreen(C_BG);
    g.drawRect(0, topY, W, waveH, C_GRID);
    g.drawFastHLine(0, midY, W, C_GRID);        // 中线 = 静音时的位置
    g.drawRect(0, barY, W, barH, C_DIM);

    g.setTextColor(C_TEXT, C_BG);
    g.setCursor(2, 2);
    g.print("MIC TEST  speak now");

    g.setTextColor(C_DIM, C_BG);
    g.setCursor(2, barY + barH + 4);
    g.print("no key needed - just talk");

    /* 声道格式: ONLY_LEFT —— L/R 接 GND 时数据在左声道。
     *
     * ⚠️ 这一条是实测定下来的(2026-10-06): 社区帖子里有人说 L/R 接 GND 要用
     * ONLY_RIGHT, 本机实测**用 ONLY_LEFT 才有数据** —— ONLY_RIGHT 下全是 0xFFFE。
     * 别照抄网上的说法, 两边都试一次才算数。 */
    int      colX      = 0;             // 扫描位置(示波器式)
    uint32_t peakHold  = 0;             // 最近 1.5 秒的峰值(会衰减)
    uint32_t peakEver  = 0;             // 开机以来收到过的最大峰值(不衰减)
    uint32_t peakAt    = millis();
    uint32_t lastBar    = 0;
    uint32_t lastSerial = 0;
    int      lastPct    = -1;
    uint32_t peakShown  = 0xFFFFFFFF;
    uint32_t lines      = 0;
    bool     warned     = false;

    for (;;) {
        size_t got = 0;
        if (i2s_read(I2S_NUM_0, buf, HT_BLOCK_BYTES, &got, pdMS_TO_TICKS(500)) != ESP_OK) continue;
        if (got < 4) continue;

        const int16_t *src = (const int16_t *)buf;
        const int      n   = got / 2;

        /* ---- 峰值保持: 显示最近 1.5 秒内最大的那个, 不然一闪就没了 ---- */
        for (int i = 0; i < n; i++) {
            const int32_t v = src[i];
            const uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
            if (a > peakHold) { peakHold = a; peakAt = millis(); }
            if (a > peakEver) peakEver = a;
        }
        const uint32_t now = millis();
        if (now - peakAt > 1500) { peakHold = 0; peakAt = now; }

        /* ---- 画 HT_COLS 列: 每列画该时间段内的 min~max 竖线(声波包络) ---- */
        const int perCol = n / HT_COLS;
        for (int c = 0; c < HT_COLS && perCol > 0; c++) {
            int16_t mn = 32767, mx = -32768;
            for (int i = 0; i < perCol; i++) {
                const int16_t s = src[c * perCol + i];
                if (s < mn) mn = s;
                if (s > mx) mx = s;
            }

            int y1 = midY - (int)((int32_t)mx * halfH / 32768);
            int y2 = midY - (int)((int32_t)mn * halfH / 32768);
            if (y1 > y2) { const int t = y1; y1 = y2; y2 = t; }
            if (y1 < topY + 1)          y1 = topY + 1;
            if (y2 > topY + waveH - 2)  y2 = topY + waveH - 2;

            const uint32_t amp   = (uint32_t)((int32_t)mx - mn);
            const uint16_t color = (amp > 20000) ? C_LOUD : C_WAVE;

            g.drawFastVLine(colX, topY + 1, waveH - 2, C_BG);   // 先清掉这一列的旧波形
            g.drawFastVLine(colX, y1, y2 - y1 + 1, color);

            colX = (colX + 1) % W;
        }
        lines++;

        /* ---- 电平条 + 峰值数字: 10Hz 够了, 不用每块都刷 ---- */
        if (now - lastBar >= 100) {
            lastBar = now;

            uint64_t acc = 0;
            for (int i = 0; i < n; i++) {
                const int32_t v = src[i];
                acc += (uint32_t)(v * v);
            }
            const uint32_t rms = (uint32_t)sqrt((double)acc / (double)n);

            int pct = 0;
            if (rms >= 4) {
                const float db = 20.0f * log10f((float)rms / 32768.0f);
                pct = (int)((db + 60.0f) * 100.0f / 60.0f);
                if (pct < 0)   pct = 0;
                if (pct > 100) pct = 100;
            }
            if (pct != lastPct) {
                lastPct = pct;
                g.fillRect(1, barY + 1, W - 2, barH - 2, C_BG);
                if (pct > 0)
                    g.fillRect(1, barY + 1, (W - 2) * pct / 100, barH - 2,
                               (pct < 60) ? C_GREEN : (pct < 85) ? C_YELLOW : C_LOUD);
            }

            if (peakHold != peakShown) {
                peakShown = peakHold;
                g.fillRect(0, 2, W, 11, C_BG);
                g.setTextColor(C_TEXT, C_BG);
                g.setCursor(2, 2);
                g.printf("MIC TEST   peak %5u", (unsigned)peakHold);
            }

            /* 每秒往串口报一次 —— 不盯着屏幕也能判断麦克风状态。
               用时间判断而不是 `lines % 30`: 后者嵌在 100ms 刷屏门控里,
               和 64ms 的读取周期相位很容易错开, 结果一次都不打印。 */
            if (now - lastSerial >= 1000) {
                lastSerial = now;

                /* 采样分布 + 原始值 —— 这是区分「没接」「接死」「有噪声」的关键。
                   只看 rms 是分不出这几种情况的, 原始字节骗不了人。 */
                int cntZero = 0, cntFF = 0, cntOther = 0;
                for (int i = 0; i < n; i++) {
                    if      (src[i] == 0)   cntZero++;
                    else if (src[i] == -1)  cntFF++;      // 0xFFFF
                    else                    cntOther++;
                }

                ESP_LOGI(TAG, "[HW] rms=%5u peak=%5u | 全0=%4d 全FF=%4d 其他=%4d | 开头:",
                              (unsigned)rms, (unsigned)peakHold, cntZero, cntFF, cntOther);
                for (int i = 0; i < 6 && i < n; i++) ESP_LOGI(TAG, " %04X", (uint16_t)src[i]);
                ESP_LOGI(TAG, "");
            }
        }

        /* 录了 8 秒一个信号都没收到过 —— 基本就是接线问题, 直接提示到屏幕上。
           用 peakEver(不衰减) 而不是 peakHold, 否则用户还没开口就会误报。 */
        if (!warned && lines >= 120 && peakEver == 0) {
            warned = true;
            g.fillRect(0, barY + barH + 1, W, H - (barY + barH + 1), C_BG);
            g.setTextColor(C_LOUD, C_BG);
            g.setCursor(2, barY + barH + 4);
            g.print("NO SIGNAL - check wiring");

            ESP_LOGI(TAG, "[HW] ⚠ 录了 8 秒没收到任何信号 —— 查:");
            ESP_LOGI(TAG, "        L/R 有没有接 GND / VDD 是不是 3V3 / SD 是不是在 GPIO40");
            ESP_LOGI(TAG, "        (INMP441 悬空 L/R 时读数会乱跳, 完全没接则恒定不变)");
        }
    }
}

/* ==================== 入口 ==================== */

void hwTestRun(Adafruit_GFX &g)
{
    ESP_LOGI(TAG, "\n\n==================================================");
    ESP_LOGI(TAG, "  硬件诊断 (HW_TEST)");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  接线核对表:");
    ESP_LOGI(TAG, "    INMP441   VDD=3V3(!) GND=GND SCK=41 WS=42 SD=40 L/R=GND");
    ESP_LOGI(TAG, "    MAX98357A VIN=5V     GND=GND BCLK=41 LRC=42 DIN=39 SD=38 SPK±=喇叭");
    ESP_LOGI(TAG, "    ST7735S   VCC=5V(!)  GND=GND SCL=21 SDA=14 RES=1 DC=48 CS=47 BLK=2");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  ⚠ 两个模块的电源别接混: 屏幕要 5V(板载LDO), 麦克风只能 3V3(接5V会烧)");
    ESP_LOGI(TAG, "==================================================");

    ampInit();
    probePins();        // ⓪ 引脚静态电平 —— 先看有没有接错/短路
    testScreen(g);

    g.fillScreen(C_BG);
    g.setTextColor(C_TEXT, C_BG);
    g.setCursor(4, 4);
    g.print("SPEAKER TEST");
    g.setCursor(4, 20);
    g.print("1kHz beep...");
    testSpeaker(g);

    delay(1200);
    testMicWave(g);        // 不会返回
}

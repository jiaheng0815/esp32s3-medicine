/*
 * ESP32-S3-CAM (OV3660/R8N16) -> ST7735S 128x160 TFT 实时取景
 * PlatformIO + Arduino-ESP32
 *
 * 数据流: OV3660 --(RGB565, QVGA 320x240)--> PSRAM 帧缓冲
 *         --(2x2 平均降采样)--> 160x120 --> ST7735S (SPI, 批量32bit发送)
 *
 * 按键 (板上 BOOT = GPIO0):
 *   短按        -> 拍照发给智谱认药名, 本地算该不该吃, 喇叭读出来
 *   按住 1.2 秒 -> 对着麦克风说话, 松手后上传智谱做语音转文字,
 *                  走「和网页文本框同一条」本地处理链路, 结果也读出来
 *   双击        -> 进配网模式 (开热点 ESP32-CAM-Setup), 随时可重配
 *   开机时按住  -> 纯屏幕测试模式, 排障用 (见下)
 *
 * ⚠️ 配网原来在「长按 3 秒」上, 现在挪到双击了 —— 因为 1.2 秒的录音阈值
 *    一定先触发, 而且「一天两次早晚各一次」这种话要说 4~8 秒, 会把 3 秒
 *    阈值撞上、说到一半跳去配网。
 *
 * 排障模式:
 *   1) 开机自动闪 红/绿/蓝/白 自检 -> 屏幕一点变化都没有 = 屏幕侧没通
 *   2) 按住板上 BOOT(GPIO0) 再复位 -> 纯屏幕测试模式(循环花屏/彩条/黑白), 不启摄像头
 *   3) 没配过网 / 连不上 -> 自动进配网模式, 屏幕上会显示热点名和网址
 *   4) voice.h 的 MIC_SELFTEST = 1 -> 麦克风/功放自检(波形 + 回放), 不启 WiFi
 *
 * 文件分工:
 *   main.cpp    屏幕 + 取景 + 按键
 *   netcfg.cpp  WiFi 凭据(NVS) + 配网网页
 *   vision.cpp  摄像头引脚/参数 + 拍照 + 调智谱 API
 *   voice.cpp   I2S 录音(INMP441) + 语音识别 + 播报(MAX98357A)
 *   med.cpp     药品记忆 (NVS) + 本地时间判断
 *   webui.cpp   常驻网页: 用药记录增删 + 文本输入(语音的降级入口)
 *   httpio.cpp  共享 HTTPS 传输层
 *   secrets.h   API Key (不进版本库)
 */

#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/gpio.h>      // 直接用 IDF 的 gpio_* 替代 digitalRead/digitalWrite/pinMode
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "netcfg.h"
#include "vision.h"
#include "med.h"
#include "webui.h"
#include "voice.h"
#include "hwtest.h"
#include "cnfont.h"

/* ============================================================
 * 0. 运行模式  ← 想切模式改这一行
 * ============================================================
 * IS_TEST = 1  TEST 模式, 退回成「纯识图」:
 *                · 拍照问的是「这是什么」, 不是「这是什么药」
 *                · 不开用药记录网页 (80 端口不监听)
 *                · 不做用药提醒 (不查记忆、不算时间)
 *                · **不启语音** (不建语音 task、不分配 PSRAM 缓冲)
 * IS_TEST = 0  正常模式: 拍照认药 + 用药记忆 + 网页 + 语音
 *
 * 改完要重新编译烧录。 */
#define IS_TEST   0
#ifndef IS_TEST
#define IS_TEST   0
#endif

/* ============================================================
 * 0.5 硬件基础测试  ← 首次接线先开这个
 * ============================================================
 * HW_TEST = 1  开机进「麦克风 + 屏幕」基础测试, 不返回:
 *                 · 屏幕闪 红/绿/蓝/白 再画中英文   -> 验屏幕 + 字库
 *                 · 放一声 1kHz 测试音              -> 验功放 + 喇叭
 *                 · 屏幕上实时画声波                -> 验麦克风
 *              全程不需要串口、不需要按键、不连 WiFi。
 *              验完改成 0 重新烧录。
 *
 * HW_TEST = 0  正常启动。
 *
 * 和 voice.h 的 MIC_SELFTEST 的分工:
 *   这个        最基础的"屏幕和麦克风活着吗", 波形直接画在屏上
 *   MIC_SELFTEST 完整音频链路(测试音 + 按住录音 + 回放), 需要看串口
 * 两个都开时以 HW_TEST 为准(它先跑且不返回)。
 * ⚠️ 首次接线建议: HW_TEST=1 + MIC_SELFTEST=0; 通过后 HW_TEST=0 + MIC_SELFTEST=1。
 * 硬件都验过了就全关掉(=0), 进正常模式。 */
#define HW_TEST   0

/* ============================================================
 * 0.6 喇叭测试  ← 想验功放+喇叭就开这个
 * ============================================================
 * TTS_TEST = 1  开机连上 WiFi 之后, 用智谱 TTS 合成并播报下面这句话。
 *
 * 比自检里的 1kHz 蜂鸣更接近真实使用: 它把「联网 -> TTS 合成 -> WAV 解析
 * -> I2S 输出 -> 功放 -> 喇叭」整条链路走了一遍, 有任何一环断了都听不到。
 *
 * 验完改成 0(它只在启动时播一次, 留着也不碍事, 但会拖慢开机几秒)。
 * ⚠️ 需要联网, 且账户有余额(TTS 按字符计费)。 */
#define TTS_TEST        1
#define TTS_TEST_TEXT   "语音测试成功，galgame牛逼"

/* ============================================================
 * 1. 屏幕 ST7735S 接线 (可改，避开摄像头用掉的 IO)
 * ============================================================ */
#define TFT_SCLK_PIN   21
#define TFT_MOSI_PIN   14
#define TFT_CS_PIN     47
#define TFT_DC_PIN     48
#define TFT_RST_PIN     1     // 没接 RES 就写 (-1)
#define TFT_BL_PIN      2     // 没接 BLK 就接 3V3, 这里写 (-1)
#define TFT_BL_ACTIVE   HIGH
#define TFT_ROTATION    1     // 1 或 3 = 横屏 160x128

/* 板上 BOOT 键。放这么靠前是因为下面测试模式那段要用到它,
   按键状态机那边的 BOOT_DEBOUNCE / BOOT_HOLD_MS 才是它的使用处。 */
#define BOOT_PIN        0

/* 面板初始化变体: 纯白/花屏/偏移就换一个试 */
#define TFT_TAB           INITR_BLACKTAB
#define TFT_TAB_AUTO      0   // 别开! 每次复位换一个 tab, 画面会在「正常」和
                              // 「斜切/偏移 + 颜色错乱」之间来回跳, 极难排查。
                              // 要试别的 tab 就直接改 TFT_TAB
#define TFT_INVERT        0   // 颜色变反色(底片感) 时改 1

#define INIT_SPI_SPEED_HZ   8000000    // 初始化阶段(抗干扰)
/* 刷图阶段。QQVGA 一帧 38400 字节:
 *   15MHz -> 20.5ms   40MHz -> 7.7ms
 * 这一项直接吃掉帧周期(17.77FPS = 56.3ms)的一大截, 提到 40MHz 才能到 15FPS+。
 * 花屏/雪花/撕裂说明排线扛不住, 逐级降到 27000000 或 20000000。 */
#define SPI_SPEED_HZ        80000000

#define PANEL_SELFTEST      1    // 开机闪 红/绿/蓝/白 自检

/* ============================================================
 * 2. 摄像头引脚 / 帧尺寸
 * ============================================================
 * 都搬到 src/vision.cpp 顶部了 —— 取景(RGB565 QVGA)和拍照(JPEG VGA)两套
 * 配置放在一起才不会改漏一边。这里只留和「取景显示」强相关的两个: */

/* ============================================================
 * 3. 取景参数
 * ============================================================ */
#define CAM_MSB_FIRST   1    // 颜色不对(偏蓝/偏橙)就改成 0

/* -------- OV3660 画面方向 (镜头反转) --------
 * OV3660 出厂默认寄存器 0x3820=0x40 / 0x3821=0x00, 非 JPEG 格式下输出的是
 * 「上下颠倒 + 左右镜像」, 相对正立画面正好转了 180°, 这不是接线问题。
 *
 * 修它就用下面的 CAM_ROT180。
 *
 * 别改用传感器端翻转 (set_hmirror/set_vflip): 驱动底层走 set_image_options(),
 * 它会连带重写 0x4514 / 0x4520 / X_INCREMENT / Y_INCREMENT。在 OV3660 + QVGA +
 * RGB565 下实测会把画面搞成斜切(平行四边形) + 颜色错乱。
 */

/* 1 = 把画面转 180° (抵消 OV3660 的出厂反向)。
 * 在 buildView() 降采样时反向取采样点实现: 不碰传感器、不影响屏幕 UI 文字方向、
 * 也不增加耗时 —— 只是换个索引顺序, 取的还是同一批 2x2 像素块。 */
#define CAM_ROT180      1

#define VIEW_W          160
#define VIEW_H          120
#define VIEW_Y          ((128 - VIEW_H) / 2)   // 128 高分屏居中显示

#ifndef SHOW_FPS
#define SHOW_FPS        1    // 1 = 画面左上角显示帧率, 0 = 纯净画面
#endif

/* ========================= 全局 ========================= */
/* 继承一下, 把上一层面板初始化(commonInit->begin())的 SPI 速度压到低速,
   排线长/接触不良时初始化命令也不容易打飞 */
class TFT_ST7735S : public Adafruit_ST7735 {
public:
    TFT_ST7735S(SPIClass *spi, int8_t cs, int8_t dc, int8_t rst)
        : Adafruit_ST7735(spi, cs, dc, rst) {}
    void begin(uint32_t freq = 0) override
    {
        Adafruit_ST7735::begin(freq ? freq : INIT_SPI_SPEED_HZ);
    }
};

static TFT_ST7735S tft(&SPI, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

static uint16_t viewBuf[VIEW_W * VIEW_H];   // 原生小端 RGB565, 批量发给屏
static int      g_scale = 2;
static bool     g_reported = false;
static uint32_t g_fps = 0;

/* ==================== 面板 tab 自动轮换 ==================== */
static uint8_t nextAutoTab()
{
    static const uint8_t tabs[3]  = { INITR_BLACKTAB, INITR_GREENTAB, INITR_REDTAB };
    static const char   *names[3] = { "BLACKTAB", "GREENTAB", "REDTAB" };

    Preferences prefs;
    prefs.begin("tft", false);
    uint8_t idx = prefs.getUChar("tab", 0);
    if (idx > 2) idx = 0;
    Serial.printf("[TFT] tab=%s (自动轮换, 共3种; 下次上电试 %s)\n",
                  names[idx], names[(idx + 1) % 3]);
    prefs.putUChar("tab", (uint8_t)((idx + 1) % 3));
    prefs.end();
    return tabs[idx];
}

/* ==================== 屏幕初始化 ==================== */
static void displayInit()
{
    if (TFT_BL_PIN >= 0) {
        gpio_set_direction((gpio_num_t)TFT_BL_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)TFT_BL_PIN, TFT_BL_ACTIVE ? 1 : 0);
    }

    SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, -1);

    uint8_t tab = TFT_TAB;
#if TFT_TAB_AUTO
    tab = nextAutoTab();
#else
    Serial.printf("[TFT] tab=%d (固定)\n", (int)tab);
#endif
    tft.initR(tab);
    tft.setSPISpeed(SPI_SPEED_HZ);
    tft.setRotation(TFT_ROTATION);
#if TFT_INVERT
    tft.invertDisplay(true);
#endif
    tft.fillScreen(ST77XX_BLACK);

    Serial.printf("[TFT] %dx%d  SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d\n",
                  tft.width(), tft.height(),
                  TFT_SCLK_PIN, TFT_MOSI_PIN, TFT_CS_PIN,
                  TFT_DC_PIN, TFT_RST_PIN, TFT_BL_PIN);
}

/* ==================== 测试模式 (IS_TEST=1) ====================
 * 开机依次跑三段, 都不启 WiFi / 摄像头(第三段才启用):
 *   ① 屏幕坏点测试   —— 全屏纯色, 眼睛找坏点
 *   ② 中文字库测试   —— 完整性检查 + 按页翻字
 *   ③ 摄像头 AI 识别 —— 就是正常的识图模式
 *
 * 手势统一: 短按 = 当前阶段下一步, 长按 = 跳到下一阶段 */

#if IS_TEST

#define BTN_HOLD_TEST_MS  1200

/* 等按键。返回 true = 长按(跳到下一阶段), false = 短按(当前阶段下一步) */
static bool testWaitButton()
{
    for (;;) {
        if (gpio_get_level((gpio_num_t)BOOT_PIN) == 0) {
            const uint32_t t0 = millis();
            while (gpio_get_level((gpio_num_t)BOOT_PIN) == 0) {
                if (millis() - t0 >= BTN_HOLD_TEST_MS) return true;
                delay(10);
            }
            return false;
        }
        delay(20);
    }
}

/* ---------- ① 屏幕坏点测试 ----------
 * 五种颜色是有讲究的:
 *   红/绿/蓝 单通道 —— 能看出某个子像素坏掉(比如只有蓝点不亮)
 *   白             —— 找暗点(该全亮的地方有黑点)
 *   黑             —— 找亮点(该全灭的地方有点亮着)
 * 注意: 提示文字显示一会儿后必须抹掉 —— 文字本身会盖住坏点。 */
static void pixelDefectTest()
{
    static const uint16_t colors[5] = {
        ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_WHITE, ST77XX_BLACK
    };
    static const char *names[5] = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };

    Serial.println("[TEST] ① 屏幕坏点测试: 短按换色, 长按下一阶段");

    for (int i = 0; i < 5; i++) {
        tft.fillScreen(colors[i]);

        /* 提示文字: 黑底用白字, 其余用黑字(白屏上白字看不见) */
        const uint16_t fg = (i >= 3) ? ST77XX_BLACK : ST77XX_WHITE;
        tft.setTextSize(1);
        tft.setTextColor(fg, colors[i]);
        tft.setCursor(6, 20);
        tft.printf("%s  %d/5", names[i], i + 1);
        tft.setCursor(6, 40);
        tft.print("tap  = next color");
        tft.setCursor(6, 54);
        tft.print("hold = next stage");
        delay(1600);

        tft.fillScreen(colors[i]);       // 抹掉文字, 留纯色给人看
        Serial.printf("[TEST]   纯色 %s (%d/5)\n", names[i], i + 1);

        if (testWaitButton()) return;    // 长按 -> 下一阶段
    }
}

/* ---------- ② 中文字库测试 ---------- */
static void fontIntegrityCheck()
{
    const uint32_t total = cnFontCount();     // 取一次, 别在循环条件里反复调

    Serial.println("\n========== 中文字库自检 ==========");
    Serial.printf("字数 %u, 数据 %u 字节 (%.0f KB)\n",
                  (unsigned)total, (unsigned)(total * 34),
                  total * 34 / 1024.0);

    /* ① 升序 */
    uint32_t disorder = 0;
    uint16_t prev = 0;
    for (uint32_t i = 0; i < total; i++) {
        const uint16_t cp = cnCodePointAt(i);
        if (cp <= prev) disorder++;
        prev = cp;
    }
    Serial.printf("① 码点升序: %s   (逆序/重复 %u 处)\n",
                  disorder ? "!! 失败 !!" : "通过", (unsigned)disorder);

    /* ② 二分查找能命中每个字 */
    uint32_t miss = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (!cnGlyph(cnCodePointAt(i))) miss++;
    }
    Serial.printf("② 二分查找: %s   (查不到 %u 个)\n",
                  miss ? "!! 失败 !!" : "通过", (unsigned)miss);

    /* ③ 抽样看点阵是否全空 */
    uint32_t blank = 0, sampled = 0;
    for (uint32_t i = 0; i < total; i += 97) {          // 质数步长, 避免撞上规律
        const uint8_t *g = cnGlyph(cnCodePointAt(i));
        if (!g) continue;
        sampled++;
        bool any = false;
        for (int b = 0; b < 32 && !any; b++) any = (g[b] != 0);
        if (!any) blank++;
    }
    Serial.printf("③ 点阵非空: %s   (抽 %u, 空 %u)\n",
                  blank ? "!! 可疑 !!" : "通过", (unsigned)sampled, (unsigned)blank);
    Serial.println("=================================\n");
}

static void fontBrowseLoop()
{
    const int      COLS     = 10, ROWS = 7;             // 10*16=160 宽, 7 行装满 128
    const uint32_t PER_PAGE = COLS * ROWS;
    const uint32_t total    = cnFontCount();
    const uint32_t PAGES    = (total + PER_PAGE - 1) / PER_PAGE;

    Serial.printf("[TEST] ② 中文字库: %u 字, %u 页 (短按翻页, 长按下一阶段)\n",
                  (unsigned)total, (unsigned)PAGES);

    uint32_t page = 0;
    for (;;) {
        tft.fillScreen(ST77XX_BLACK);
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        tft.setCursor(0, 0);
        tft.printf("%u/%u cp=%04X", (unsigned)(page + 1), (unsigned)PAGES,
                   (unsigned)cnCodePointAt(page * PER_PAGE));

        for (uint32_t i = 0; i < PER_PAGE; i++) {
            const uint32_t idx = page * PER_PAGE + i;
            if (idx >= total) break;
            cnDrawIndexed(tft, (i % COLS) * CN_CHAR_W,
                          12 + (i / COLS) * CN_CHAR_H, idx, ST77XX_WHITE);
        }

        if (testWaitButton()) return;        // 长按 -> 下一阶段

        page = (page + 1) % PAGES;           // 短按 -> 翻一页
        Serial.printf("[TEST]   第 %u/%u 页, 首字码点 %04X\n",
                      (unsigned)(page + 1), (unsigned)PAGES,
                      (unsigned)cnCodePointAt(page * PER_PAGE));
    }
}

/* ---------- 测试模式主流程 ---------- */
static void testModeRun()
{
    Serial.println("\n########## TEST 模式 ##########");
    Serial.println("顺序: ① 屏幕坏点 -> ② 中文字库 -> ③ 摄像头 AI 识别");
    Serial.println("手势: 全程 短按 = 下一步, 长按 = 跳到下一阶段\n");

    Serial.println("---- [1/3] 屏幕坏点测试 ----");
    pixelDefectTest();

    Serial.println("\n---- [2/3] 中文字库测试 ----");
    fontIntegrityCheck();
    fontBrowseLoop();

    Serial.println("\n---- [3/3] 摄像头 AI 识别 ----");
    Serial.println("短按拍照识图; 想重跑测试就复位\n");
    /* 不返回 —— 后面 setup 会接着初始化摄像头, 然后进正常 loop */
}

#endif  // IS_TEST

/* ==================== 屏幕自检 / 测试图案 ==================== */
static void testColorBars()
{
    static const uint16_t bars[8] = {
        ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_YELLOW,
        ST77XX_CYAN, ST77XX_MAGENTA, ST77XX_WHITE, ST77XX_BLACK
    };
    const int16_t w = tft.width(), h = tft.height();
    const int16_t bw = w / 8;
    for (int8_t i = 0; i < 8; i++)
        tft.fillRect(i * bw, 0, bw, h, bars[i]);
}

static void testChecker()
{
    const int16_t w = tft.width(), h = tft.height();
    const int16_t s = 16;
    for (int16_t y = 0; y < h; y += s)
        for (int16_t x = 0; x < w; x += s)
            tft.fillRect(x, y, s, s, (((x / s) + (y / s)) & 1) ? ST77XX_WHITE : ST77XX_BLACK);
}

static void testFrame()
{
    const int16_t w = tft.width(), h = tft.height();
    tft.fillScreen(ST77XX_BLACK);
    for (int16_t i = 0; i < 3; i++)                 // 三色边框, 检查偏移
        tft.drawRect(i, i, w - 2 * i, h - 2 * i,
                     i == 0 ? ST77XX_RED : (i == 1 ? ST77XX_GREEN : ST77XX_BLUE));
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(14, 10);
    tft.print("ST7735S OK");
    tft.setCursor(14, 26);
    tft.printf("%dx%d", w, h);
    tft.setCursor(14, 42);
    tft.print("SCLK21 SDA14");
    tft.setCursor(14, 58);
    tft.print("RES1 DC48 CS47");
}

static void bootSelfTest()
{
#if PANEL_SELFTEST
    Serial.println("[TFT] 自检: 依次闪 红/绿/蓝/白, 屏幕没反应就是屏幕侧不通");
    tft.fillScreen(ST77XX_RED);   delay(260);
    tft.fillScreen(ST77XX_GREEN); delay(260);
    tft.fillScreen(ST77XX_BLUE);  delay(260);
    tft.fillScreen(ST77XX_WHITE); delay(260);
    tft.fillScreen(ST77XX_BLACK);
#endif
}

/* 纯屏幕测试模式: 按住 BOOT 再复位进入, 用来判断"屏幕/接线"本身是否正常 */
static void panelTestLoop()
{
    Serial.println("[TEST] 纯屏幕测试模式: 不启动摄像头, 复位退出");
    for (uint8_t step = 0;; step++) {
        Serial.printf("[TEST] pattern %u/6\n", (unsigned)(step % 6));
        switch (step % 6) {
        case 0: tft.fillScreen(ST77XX_RED);   break;
        case 1: tft.fillScreen(ST77XX_GREEN); break;
        case 2: tft.fillScreen(ST77XX_BLUE);  break;
        case 3: tft.fillScreen(ST77XX_WHITE); break;
        case 4: testColorBars();              break;
        case 5: (step % 12 < 6) ? testFrame() : testChecker(); break;
        }
        delay(900);
    }
}

/* 摄像头初始化在 vision.cpp:
 *   visionInitViewMode()    取景模式 (RGB565 QVGA)
 *   visionCaptureAndAsk()   内部自行切到 JPEG VGA 拍完再切回来
 * 引脚和两套模式的参数都在那边顶部。 */

/* =============== 取出摄像头像素 (RGB565 值) =============== */
static inline uint16_t camPixel(const uint8_t *buf, int w, int x, int y)
{
    const uint8_t *p = buf + ((size_t)y * w + x) * 2;
#if CAM_MSB_FIRST
    return (uint16_t)((p[0] << 8) | p[1]);
#else
    return (uint16_t)((p[1] << 8) | p[0]);
#endif
}

/* ========== 转成屏幕要的 RGB565 到 viewBuf ========== */
static void buildView(const camera_fb_t *fb)
{
    const int sw = fb->width;
    const int sh = fb->height;

    /* ---- 1:1 快速路径 (取景用 QQVGA 160x120 时走这里) ----
       源尺寸和目标一样, 不需要降采样。绕开下面那套 RGB 分离 / 累加 / 回平均 ——
       n==1 时那些运算纯属白做, 每个像素其实只是搬两个字节再换个序。 */
    if (sw == VIEW_W && sh == VIEW_H) {
        for (int y = 0; y < VIEW_H; y++) {
#if CAM_ROT180
            const int sy = VIEW_H - 1 - y;
#else
            const int sy = y;
#endif
            const uint8_t *src = fb->buf + (size_t)sy * sw * 2;
            uint16_t      *dst = viewBuf + (size_t)y * VIEW_W;
            for (int x = 0; x < VIEW_W; x++) {
#if CAM_ROT180
                const int sx = VIEW_W - 1 - x;
#else
                const int sx = x;
#endif
                const uint8_t *p = src + (size_t)sx * 2;
#if CAM_MSB_FIRST
                dst[x] = (uint16_t)((p[0] << 8) | p[1]);
#else
                dst[x] = (uint16_t)((p[1] << 8) | p[0]);
#endif
            }
        }
        return;
    }

    /* ---- 降采样路径 (源比目标大时走这里) ---- */
    const int xr = (sw / VIEW_W) < 1 ? 1 : sw / VIEW_W;
    const int yr = (sh / VIEW_H) < 1 ? 1 : sh / VIEW_H;
    const int n  = xr * yr;

    for (int y = 0; y < VIEW_H; y++) {
        /* CAM_ROT180: 行和列都倒着取, 出来的就是转 180° 的画面。
           取的是同一批 2x2 块, 只是块的先后顺序反了, 均值不受影响 */
#if CAM_ROT180
        const int sy = (VIEW_H - 1 - y) * yr;
#else
        const int sy = y * yr;
#endif
        uint16_t *dst = viewBuf + (size_t)y * VIEW_W;
        for (int x = 0; x < VIEW_W; x++) {
#if CAM_ROT180
            const int sx = (VIEW_W - 1 - x) * xr;
#else
            const int sx = x * xr;
#endif
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < yr; j++) {
                for (int i = 0; i < xr; i++) {
                    const uint16_t px = camPixel(fb->buf, sw, sx + i, sy + j);
                    r += (px >> 11) & 0x1F;
                    g += (px >> 5) & 0x3F;
                    b += px & 0x1F;
                }
            }
            if (n == 4)       { r >>= 2; g >>= 2; b >>= 2; }
            else if (n == 16) { r >>= 4; g >>= 4; b >>= 4; }
            else if (n != 1)  { r /= n;  g /= n;  b /= n; }
            dst[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

/* ==================== 一帧推到屏幕 ==================== */
static void pushView()
{
    tft.startWrite();
    tft.setAddrWindow(0, VIEW_Y, VIEW_W, VIEW_H);
    tft.writePixels(viewBuf, VIEW_W * VIEW_H);
    tft.endWrite();
}

#if SHOW_FPS
static void drawFps(uint32_t fps)
{
    tft.fillRect(0, VIEW_Y, 42, 10, ST77XX_BLACK);
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(2, VIEW_Y + 1);
    tft.printf("%uFPS", (unsigned)fps);
}
#endif

/* ====================== 开关 ====================== */
/* BOOT_PIN 见文件开头(测试模式那段要先用到它) */
#define BOOT_DEBOUNCE   50      // 短于这个时长的抖动忽略 (ms)
#define BTN_TAP_MAX     1200    // 松手时短于这个 = 短按(拍照); 按住超过它 = 录音
#define BTN_DOUBLE_GAP  400     // 两次短按间隔小于这个 = 双击 -> 配网 (ms)

/* 1 = 开机跑一次拍照链路诊断 (依次试 JPEG QVGA/VGA/SVGA + RGB565 VGA),
 * 结果打到串口然后停住, 不启 WiFi。排障用, 平时保持 0 */
#define BENCH_CAPTURE   0

/* ====================== TFT 互斥锁 ======================
 * 语音 task 和主循环都会往屏幕上画 —— 语音的进度提示是走 onVoiceUi 回调
 * 画上去的, 而 setup()/loop() 里也在画。
 *
 * ⚠️ Adafruit_SPITFT 不是线程安全的, 并发调用会把内部的窗口状态搞乱,
 *    实测**卡死在 SPI 写入里再也出不来**。症状很有迷惑性:
 *    设备不重启、串口没异常, 但语音 task 卡住 -> voiceBusy() 永远为真
 *    -> 主循环每帧直接 return -> 连 [FPS] 都不打印, 看着像死机。
 *
 * 带超时: 拿不到锁就跳过这一帧的绘制。宁可少画一帧, 也不能把任务挂死。 */
static SemaphoreHandle_t s_tftMux = nullptr;

static bool tftLock(uint32_t ms = 3000)
{
    if (!s_tftMux) s_tftMux = xSemaphoreCreateMutex();
    return s_tftMux && xSemaphoreTake(s_tftMux, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void tftUnlock()
{
    if (s_tftMux) xSemaphoreGive(s_tftMux);
}

/* ====================== 状态提示 ======================
 * 屏幕 160x128。这里只显示英文短句(内置字体, 快);
 * 中文要画走 cnDrawWrapped()(16x16 点阵), 见 drawVoiceResult()。 */
static void showStatus(const char *line1, const char *line2 = nullptr)
{
    if (!tftLock()) return;                 // 拿不到锁就跳过, 别把调用方挂死

    tft.fillRect(0, VIEW_Y, VIEW_W, VIEW_H, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setCursor(4, VIEW_Y + 6);
    tft.print(line1);
    if (line2) {
        tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
        tft.setCursor(4, VIEW_Y + 22);
        tft.print(line2);
    }

    tftUnlock();
}

static void onVisionStage(const char *stage)               { showStatus(stage); }
static void onSetupStatus(const char *l1, const char *l2)  { showStatus(l1, l2); }

/* ==================== 语音 UI ====================
 * 这些**在语音 task 里被回调**(不是主循环)。这样是安全的:
 * voiceBusy() 期间主循环完全不画屏幕, 同一时刻只有一方在动 SPI。 */

static int s_lastLevel = -1;

static void drawLevelBar(int level)
{
    const int x = 4;
    const int y = VIEW_Y + 86;
    const int w = VIEW_W - 8;
    const int h = 12;

    if (level == s_lastLevel) return;       // 没变就不刷, 省 SPI 带宽
    if (!tftLock()) return;                 // 拿不到锁就跳过; 不更新 last, 下一帧自然会重试
    s_lastLevel = level;

    const uint16_t color = (level < 60) ? ST77XX_GREEN
                         : (level < 85) ? ST77XX_YELLOW
                                        : ST77XX_RED;
    tft.fillRect(x + 1, y + 1, w - 2, h - 2, ST77XX_BLACK);
    if (level > 0) tft.fillRect(x + 1, y + 1, (w - 2) * level / 100, h - 2, color);
    tft.drawRect(x, y, w, h, ST77XX_WHITE);

    tftUnlock();
}

/* cnDraw() 本身不换行(它只管一路画过去, 返回末尾的 x)。这里按宽度手工折行。
 * 宽度规则和 cnfont 一致: ASCII 8 像素, 汉字 16 像素。 */
static void cnDrawWrapped(int16_t x, int16_t y, int16_t maxW,
                          const char *utf8, uint16_t color, int16_t lineH)
{
    const char *p = utf8;
    int16_t cx = x, cy = y;

    while (p && *p) {
        const uint8_t c = (uint8_t)*p;
        int n = 1, w = 8;                               // ASCII: 8 宽
        if      (c >= 0xF0) n = 4;
        else if (c >= 0xE0) { n = 3; w = CN_CHAR_W; }   // 汉字: 16 宽
        else if (c >= 0xC0) n = 2;

        if (cx + w > x + maxW) { cx = x; cy += lineH; }
        if (cy + CN_CHAR_H > VIEW_Y + VIEW_H) break;    // 画不下了就停

        char one[5] = {0};
        for (int i = 0; i < n && p[i]; i++) one[i] = p[i];
        cx = cnDraw(tft, cx, cy, one, color);
        p += n;
    }
}

static void onVoiceUi(const char *l1, const char *l2, int level)
{
    static char prev1[32] = "";
    static char prev2[40] = "";

    if (l1 && (strcmp(prev1, l1) != 0 || strcmp(prev2, l2 ? l2 : "") != 0)) {
        strlcpy(prev1, l1, sizeof(prev1));
        strlcpy(prev2, l2 ? l2 : "", sizeof(prev2));
        showStatus(l1, l2);             // 会清屏
        s_lastLevel = -1;               // 电平条被清掉了, 下一帧强制重画
    }
    if (level >= 0) drawLevelBar(level);
}

/* 语音结果上屏 —— cnfont 里有 16x16 中文点阵, 汉字能直接画。
 * 这条很关键: 面向的是不方便看手机的场合, 结果只出现在网页上等于没用。 */
static void drawVoiceResult(const char *heard, const char *reply)
{
    if (!tftLock()) return;

    tft.fillRect(0, VIEW_Y, VIEW_W, VIEW_H, ST77XX_BLACK);
    s_lastLevel = -1;

    int16_t y = VIEW_Y + 2;

    /* 转写为空时(比如"没听到声音")就别画那个空标题了 */
    if (heard && *heard) {
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
        tft.setCursor(4, y);
        tft.print("Heard:");
        y += 12;
        cnDrawWrapped(4, y, VIEW_W - 8, heard, ST77XX_WHITE, CN_CHAR_H + 2);
        y += 36;                    // 转写一般 1~2 行, 固定留两行的高度
    }

    cnDrawWrapped(4, y, VIEW_W - 8, reply, ST77XX_YELLOW, CN_CHAR_H + 2);

    tftUnlock();
}

/* ==================== 短按: 拍照识别药品 ====================
 * 分两阶段, 各管各的:
 *   ① 模型只认药名 —— 问得越窄, 回答越短, token 越省, 也越好做本地匹配
 *   ② 该不该吃由本地 medAdvice() 判断 —— 不调 API、快、免费, 而且时间算术
 *      交给代码比交给模型可靠得多
 */
#if IS_TEST
/* TEST 模式: 普通识图 */
static const char ASK_PROMPT[] =
    "这是什么？用一句中文直接回答，不超过20字，不要客套和多余解释。";
#else
/* 正常模式: 只认药名 —— 问得越窄, 回答越短, token 越省, 也好做本地匹配 */
static const char ASK_PROMPT[] =
    "这是什么药？只回答药品的名称，不要剂量、用途、厂家或任何其他内容。"
    "如果包装上的药名看不清或不确定，只回答三个字：看不清。";
#endif

static void runCaptureAsk()
{
#if IS_TEST
    Serial.println("\n[KEY] 短按 -> 拍照识图 (TEST 模式)");
#else
    Serial.println("\n[KEY] 短按 -> 拍照识别药品");
#endif

    const VisionResult r = visionCaptureAndAsk(ASK_PROMPT, onVisionStage);

    if (r.ok) {
        Serial.println("\n========== 模型回答 ==========");
        Serial.println(r.text);
        Serial.println("==============================");

#if !IS_TEST
        /* 第二阶段: 本地查记忆、本地算时间 —— 纯本地, 不调 API */
        const String advice = medAdvice(r.text.c_str());
        Serial.println("\n========== 用药提醒 ==========");
        Serial.println(advice);
        Serial.println("==============================");

        /* 药名和结论都上屏 —— cnfont 有中文点阵, 不用只靠串口 */
        drawVoiceResult(r.text.c_str(), advice.c_str());
        delay(1500);                    // 让人看一眼再被别的提示盖掉

        /* 读出来。拍照比说话简单得多, 所以这条路径对不方便看屏幕的人
           才是主路径 —— 拍一下就知道该不该吃。
           走异步: 同步播的话这几秒里 webuiLoop() 停摆, 手机打不开网页,
           而结论恰恰要显示在网页上。 */
        voiceSpeakAsync(advice.c_str(), true);      // 用药结论, 带免责
#else
        showStatus("See serial", nullptr);
#endif
    } else {
        showStatus("Failed", "see serial");
        Serial.printf("\n[ERR] %s\n", r.text.c_str());
    }

#if SHOT_PREVIEW && IS_TEST
    /* 临时网页看刚拍的照片。**只在 TEST 模式** —— 正常模式下常驻用药网页
       已经占了 80 端口, 两个 WebServer 抢同一个端口会 bind 失败, 而且不报错,
       只是页面打不开, 极难查。正常模式改用常驻网页的 /photo 路由。 */
    showStatus("Photo page:", "see serial");
    visionPreviewLastShot(SHOT_PREVIEW_SECONDS);
#endif

    delay(1200);            // 让提示多停一会儿, 别一闪而过
}

/* ==================== 按键: 短按拍照 / 按住录音 / 双击配网 ====================
 * 三个手势彻底错开, 不会互相误触发。
 *
 * ⚠️ 配网原来挂在「长按 3 秒」上, 必须挪走: 1.2 秒的录音阈值一定先触发,
 *    而且「一天两次早晚各一次」要说 4~8 秒, 会把 3 秒阈值撞上、说到一半跳去配网。
 *
 * ⚠️ 录音期间按键归 mic_task —— 主循环这几秒里可能根本跑不到这里(或跑得很慢),
 *    task 用 i2s_read 的间隙高频轮询 GPIO 检测松手, 主循环一个字节都不碰。
 *
 * setup() 里那个「开机时按住 -> 屏幕测试模式」是启动时判定一次的,
 * 能走到 loop() 就说明当时没按住, 所以两者不会打架。 */
static void handleBootButton()
{
    static bool     down       = false;
    static uint32_t downAt     = 0;
    static bool     holdFired  = false;   // 这次按下是否已经变成录音
    static bool     secondTap  = false;   // 这次按下是双击的第二击
    static uint32_t firstTapUp = 0;       // 第一击松开的时刻; 0 = 没有挂起的单击
    static bool     wasBusy    = false;

    const bool     now = (gpio_get_level((gpio_num_t)BOOT_PIN) == 0);
    const uint32_t t   = millis();

    /* ---- 语音会话进行中: 按键归语音 task ---- */
    if (voiceBusy()) {
        wasBusy = true;
        return;
    }
    if (wasBusy) {
        /* 刚从会话回来 —— 手可能还按着(比如刚按键打断了播报)。
           把这次按下整个吃掉, 否则会被算成一次新按下, 接着按住又开始录音。 */
        wasBusy    = false;
        down       = now;
        holdFired  = true;
        secondTap  = false;
        firstTapUp = 0;
        downAt     = t;
        return;
    }

    /* ---- 单击挂起超时了, 没等来第二击 -> 执行拍照 ---- */
    if (firstTapUp && !down && (t - firstTapUp) >= BTN_DOUBLE_GAP) {
        firstTapUp = 0;
        runCaptureAsk();                    // 阻塞数秒
        /* 阻塞期间手可能又按下了 —— 等它松开。否则手还按着会被当成新的按下,
           接着按住又变成一次莫名其妙的录音 */
        const uint32_t t0 = millis();
        while (gpio_get_level((gpio_num_t)BOOT_PIN) == 0 && millis() - t0 < 5000) delay(10);
        down = false; holdFired = false;
        return;
    }

    if (now && !down) {                                 // 刚按下
        down      = true;
        downAt    = t;
        holdFired = false;
        secondTap = (firstTapUp != 0 && (t - firstTapUp) < BTN_DOUBLE_GAP);
        if (secondTap) firstTapUp = 0;                  // 已确认双击, 取消单击挂起
        return;
    }

    if (now && down && !holdFired && (t - downAt >= BTN_TAP_MAX)) {
        /* 按住到点 -> 开始录音 */
        holdFired  = true;
        firstTapUp = 0;                                 // 长按不算双击的第二击
#if IS_TEST
        Serial.println("\n[KEY] 长按 (TEST 模式已关掉语音)");
#else
        if (voiceStartRec()) {
            Serial.println("\n[KEY] 按住 -> 录音中, 松手结束并识别");
        } else {
            Serial.println("\n[KEY] 上一段语音还没处理完, 本次忽略");
        }
#endif
        return;
    }

    if (!now && down) {                                 // 刚松开
        down = false;
        if (holdFired) {                                // 录音已经在 task 里跑了
            secondTap = false;
            return;
        }
        const uint32_t held = t - downAt;
        if (held < BOOT_DEBOUNCE) {                     // 抖动
            secondTap = false;
            return;
        }

        if (secondTap) {                                // 双击 -> 配网
            secondTap = false;
            Serial.println("\n[KEY] 双击 -> 进配网模式");
            WiFi.disconnect(true);
            wifiRunSetupPortal(onSetupStatus);          // 不会返回: 存完或超时都会重启
            return;
        }
        if (held < BTN_TAP_MAX) {
            firstTapUp = t;                             // 单击: 先挂起, 等双击窗口关
        }
        secondTap = false;
    }
}

/* ====================== setup / loop ====================== */
void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== ESP32-S3-CAM + ST7735S 取景器 ===");

    /* IDF 写法, 等价于 pinMode(BOOT_PIN, INPUT_PULLUP) */
    gpio_config_t bootCfg = {};
    bootCfg.pin_bit_mask = 1ULL << BOOT_PIN;
    bootCfg.mode         = GPIO_MODE_INPUT;
    bootCfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    bootCfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    bootCfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&bootCfg);

    const bool bootHeld = (gpio_get_level((gpio_num_t)BOOT_PIN) == 0);
    if (bootHeld) Serial.println("[SYS] 检测到 BOOT 按下 -> 纯屏幕测试模式");

    Serial.printf("[SYS] PSRAM: %u KB, 可用堆: %u KB\n",
                  (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(ESP.getFreeHeap() / 1024));

    /* loopTask 固定跑在 core 1 (CONFIG_ARDUINO_RUNNING_CORE=1);
       core 0 上是 cam_task(CONFIG_CAMERA_CORE0) 和 lwIP 协议栈 */
    Serial.printf("[SYS] 本固件的主循环跑在 core %d\n", xPortGetCoreID());

    /* TFT 互斥锁在这里就建好, 别指望 tftLock() 里的懒初始化:
       语音 task 和主循环可能同时第一次调用, 各自建一个 mutex = 等于没锁 */
    s_tftMux = xSemaphoreCreateMutex();

    displayInit();
    bootSelfTest();
    if (bootHeld) panelTestLoop();

#if HW_TEST
    /* 麦克风 + 屏幕基础测试 —— 不返回。首次接线先跑这个, 通过再上 MIC_SELFTEST。 */
    hwTestRun(tft);
#endif

#if MIC_SELFTEST
    /* ---- 麦克风 / 功放自检 ----
       首次接好线先用它: 先放 1kHz 测试音验功放, 再按住 BOOT 录音打 ASCII
       波形 + 喇叭回放验麦克风。INMP441 + ESP32-S3 有「读数恒为 0」的已知案例,
       必须先把硬件排除掉, 否则后面出问题分不清是接线还是代码。
       跑完停在这里不返回 —— 验完把 voice.h 的 MIC_SELFTEST 改成 0 重新烧录。 */
    showStatus("Voice selftest", "see serial");
    if (!voiceBegin(onVoiceUi)) {
        showStatus("Voice FAILED", "check serial");
        for (;;) delay(1000);
    }
    voiceSelfTest();               // 不会返回
#endif

#if IS_TEST
    /* ① 屏幕坏点 ② 中文字库 —— 都不需要摄像头/WiFi, 所以放在这里。
       第三段(AI 识别)就是后面正常的 loop, 不用特殊处理。 */
    testModeRun();
#endif

#if BENCH_CAPTURE
    showStatus("Bench capture", "see serial");
    visionBenchCapture();          // 不返回, 测完停在原地
#endif

    /* ---- WiFi: 有凭据就连, 没凭据/连不上就进配网 ---- */
    if (!wifiConnectStored(10000)) {
        Serial.println(wifiHasStored() ? "[WIFI] 连不上, 进配网模式"
                                       : "[WIFI] 还没配过网, 进配网模式");
        wifiRunSetupPortal(onSetupStatus);          // 不会返回, 内部会重启
    }

#if !IS_TEST
    /* ---- 语音链路 ----
       开机就建 task + 分配 PSRAM: 拍照链路反复 alloc/free 960KB framebuffer,
       等到长按那一刻再分配/建 task, 堆已经碎了, 大概率失败 */
    if (!voiceBegin(onVoiceUi)) {
        showStatus("Voice FAILED", "see serial");   // 不致命: 拍照和网页还能用
    } else {
        voiceSetResultCb(drawVoiceResult);          // 转写文字和答复都上屏
    }

    /* ---- 用药记录网页 ----
       麦克风没接/坏了时这是降级录入途径 —— 语音走的是同一条处理链路 */
    webuiBegin();

#if TTS_TEST
    /* ---- 喇叭测试 ----
       用真实 TTS 播一句话: 把「联网 -> TTS 合成 -> WAV 解析 -> I2S -> 功放」
       整条链路走一遍, 比 1kHz 蜂鸣更接近真实使用场景。

       ⚠️ **不等它播完** —— 播报跑在语音 task 里(优先级 3, 高于 loopTask 的 1),
       如果在这里同步等, 一旦 TTS 请求慢或卡住, 整个开机流程会被拖死
       (实测卡过 40 秒不进主循环)。丢给后台就行, 结果看串口的 [TTS] 日志。 */
    showStatus("TTS test", "listen to speaker");
    Serial.printf("\n[TTS-TEST] 开始播报: %s\n", TTS_TEST_TEXT);
    voiceSpeakAsync(TTS_TEST_TEXT);
#endif
#endif

    /* ---- NTP 对时 ----
       「早上吃过了」这类判断需要真实时间, 而 ESP32 没有 RTC。
       同步失败也不拦着启动 —— 只是提醒功能降级成「只记录、不判断时序」。 */
    configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org");
    struct tm tmNow;
    if (getLocalTime(&tmNow, 8000)) {
        Serial.printf("[TIME] 已对时: %04d-%02d-%02d %02d:%02d\n",
                      tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                      tmNow.tm_hour, tmNow.tm_min);
    } else {
        Serial.println("[TIME] NTP 同步失败 —— 用药提醒将只记录、不判断时序");
    }

    /* ---- 摄像头 ---- */
    showStatus("Camera init...");
    if (!visionInitViewMode()) {
        showStatus("Camera FAILED", "check FPC cable");
        for (;;) delay(1000);
    }
}

void loop()
{
    static uint32_t frames = 0;
    static uint32_t tLast  = 0;

    /* 按键在 fb_get 前后各采一次: fb_get 要等下一帧(~56ms), 前后各一次
       等于把按键采样率从 ~17Hz 提到 ~35Hz, 双击窗口和 1.2 秒阈值都更准 */
    handleBootButton();

#if !IS_TEST
    if (voiceBusy()) {
        /* 语音会话进行中(录音/上传/播报): 不取景、不刷屏。
           屏幕归语音 task 独占, 顺便省下 CPU 和 PSRAM 带宽给 I2S 和 TLS。
           ⚠️ webuiLoop() 不能跟着跳过 —— 结果要显示在网页上,
              这几十秒里手机必须打得开。 */
        webuiLoop();
        delay(5);
        return;
    }
#endif

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        if (!g_reported) {
            g_scale = fb->width / VIEW_W;
            if (g_scale < 1) g_scale = 1;
            Serial.printf("[CAM] 实际输出 %ux%u RGB565, 降采样 %dx -> %dx%d\n",
                          (unsigned)fb->width, (unsigned)fb->height, g_scale,
                          VIEW_W, VIEW_H);
            g_reported = true;
        }

        if (fb->format == PIXFORMAT_RGB565 &&
            fb->width >= VIEW_W && fb->height >= VIEW_H) {
            buildView(fb);                  // 只写内存, 不碰 TFT, 所以放在锁外
            if (tftLock()) {                // 和语音 task 的屏幕操作串行化
                pushView();
#if SHOW_FPS
                drawFps(g_fps);             // 每帧都重画, 否则会被下一帧盖掉
#endif
                tftUnlock();
            }
            frames++;
        }
        esp_camera_fb_return(fb);
    } else {
        delay(5);               // 拿不到帧就歇一下 —— 但别 return, 按键还要靠下面检测
    }

    handleBootButton();         // 第二次采样(见 loop 开头)

#if !IS_TEST
    /* 网页请求 —— 一次只处理一个, 处理完立刻回来接着取景 */
    webuiLoop();
#endif

    const uint32_t now = millis();
    if (now - tLast >= 1000) {
        g_fps  = frames * 1000 / (now - tLast);
        frames = 0;
        tLast  = now;
        Serial.printf("[FPS] %u\n", (unsigned)g_fps);
    }
}

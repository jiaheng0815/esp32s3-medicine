#pragma once
#include <Arduino.h>

/* 语音链路 —— INMP441 录音 + 智谱 ASR 转写 + GLM-TTS 播报 + MAX98357A 放音。
 *
 * 两个模块共用 BCLK/WS 两根时钟线, 分时切换 I2S 方向:
 *     录音: RX @16kHz, data_in  = GPIO40 (INMP441 的 SD)
 *     播报: TX @24kHz, data_out = GPIO39 (MAX98357A 的 DIN)
 * 任何时刻只有一个 I2S 存在, 所以既不会和摄像头争 GDMA,
 * 也不用担心喇叭的声音被麦克风录进去。
 *
 * 为什么不给两个模块各配一组时钟: ESP32-S3 上非 strapping 的空闲脚
 * 只有 38/39/40/41/42 五个, 而两组 I2S 各要 3 根线, 不够。
 *
 * ⚠️ 但共用时钟带来一个副作用: RX 模式下 BCLK/WS 一直在翻转, MAX98357A
 *    看到有时钟就工作, 而 DIN 此刻是悬空的 —— 功放会把噪声放大出来,
 *    再被 INMP441 录回去(自激)。所以功放的 SD/EN 必须能被软件关掉:
 *    走 GPIO38, 只有播报时才拉高。 */

/* ---------- 引脚 (换脚改这里; 注意 3/45/46 是 strapping 脚, 别用) ---------- */
#define VOICE_PIN_BCLK    41        // 共用位时钟  -> INMP441 SCK / MAX98357A BCLK
#define VOICE_PIN_WS      42        // 共用帧同步  -> INMP441 WS  / MAX98357A LRC
#define VOICE_PIN_MIC_SD  40        // 麦克风数据  -> INMP441 SD
#define VOICE_PIN_SPK_DIN 39        // 功放数据    -> MAX98357A DIN
#define VOICE_PIN_AMP_SD  38        // 功放使能    -> MAX98357A SD/EN (拉高才工作)

/* BOOT 按键 —— 和 main.cpp 的 BOOT_PIN 是同一个脚。
 * 录音 task 要自己高频轮询它来检测"松手", 所以这里也得有一份。
 * ⚠️ 改 main.cpp 的 BOOT_PIN 时这里要一起改。 */
#define VOICE_PIN_BOOT    0

/* ---------- 录音参数 ---------- */
#define VOICE_RATE_HZ     16000     // INMP441 用 16k; GLM-ASR 也吃这个采样率
#define VOICE_MAX_SEC     10        // 单次录音上限 —— 智谱 ASR 限 30 秒, 留足余量

/* WAV 头长度 / PCM 起点。
 *
 * ⚠️ 不是标准的 44: ESP32-S3 的 GDMA 访问 PSRAM 要求 16 字节对齐
 *    (SOC_GDMA_PSRAM_MIN_ALIGN = 16), 而 44 不是 16 的倍数。
 *
 * 做法是在 fmt 和 data 之间塞一个 20 字节的 **JUNK chunk**(RIFF 里专门用来
 * 做对齐填充的合法块): 44 + 20 = 64, PCM 正好落在 16 字节边界上。
 * 关键是 data chunk 要如实声明在 56 偏移 —— 随便填 20 字节垃圾的话,
 * 解析方(包括本文件回放用的 wavParse)会算错 data 的位置。 */
#define VOICE_WAV_HDR     64        // 完整头: 44 字节标准头 + 20 字节 JUNK 块
#define VOICE_PCM_OFF     64        // PCM 起点, 和头等长

/* ---------- 静音 / 断线判定 (16bit 满量程 32768) ---------- */
/* 整段峰值低于它就认为"没收到声音", 直接拦下不调 API。
 * 安静房间底噪几百, 正常说话峰值几千往上。
 * INMP441 没接好的典型症状就是「一切正常但全是 0」。 */
#define VOICE_SILENT_PEAK 200

/* ---------- 开关 ---------- */

/* 1 = 开机进「麦克风 + 功放」自检, 不连 WiFi、不进主流程。
 *
 * 首次接好线**必须先用它确认硬件** —— INMP441 + ESP32-S3 这个组合在
 * Arduino 论坛有「读数恒为 0 且最终未解决」的案例(换过 3 个麦克风、2 块板),
 * 而且本项目已知的坑全是「看代码看不出来」的类型。先确认有信号, 再往下走。
 *
 * 自检内容: 放 1kHz 测试音验功放 -> 按住 BOOT 录音打 ASCII 波形 -> 喇叭回放。
 * 波形和回放要分开看: 有波形但没声音 = 功放侧问题, 两层独立定位。
 * 确认没问题后改成 0。
 *
 * 注意: 更基础的那一层验证(main.cpp 的 HW_TEST, 屏幕上直接画波形)建议先跑 ——
 * 那个不用按键也不用串口, 更适合第一次接线。 */
#define MIC_SELFTEST      0

/* ---------- 屏幕回调 ----------
 * voice.cpp 不碰 tft(那是 main.cpp 的 static 全局), 要画什么都回调出去。
 * level >= 0 表示"顺便画个电平条"(0~100); < 0 表示只更新文字。
 *
 * ⚠️ 这个回调**在 mic_task 里被调用**, 而主循环在 voiceBusy() 期间完全不画屏幕,
 *    所以同一时刻只有一方在动 SPI —— 不会有并发问题。 */
typedef void (*VoiceUiCb)(const char *line1, const char *line2, int level);

/* 结果显示回调 —— 转写文字 + 本地答复。
 * 屏幕有 16x16 中文点阵(见 cnfont.h), 结果应该直接留在屏上:
 * 这个功能面向的就是不方便掏手机的场合。
 * 和 VoiceUiCb 一样在语音 task 里被调用。 */
typedef void (*VoiceResultCb)(const char *heard, const char *reply);
void voiceSetResultCb(VoiceResultCb cb);

/* ---------- 生命周期 ---------- */

/* setup() 里调一次: 分配 PSRAM 缓冲 + 配置功放使能脚 + 创建语音 task。
 * ⚠️ 必须开机就做 —— 拍照链路反复 alloc/free 960KB framebuffer,
 *    等到长按那一刻再分配/建 task, 堆已经碎了, 大概率失败。 */
bool voiceBegin(VoiceUiCb ui);

/* ---------- 会话 ---------- */

/* 长按到点(>=1.2 秒)时调。true = 已经开始录音, 之后主循环交出手所有权。
 * false = 上一段还没处理完, 本次手势忽略。 */
bool voiceStartRec();

/* 录音 / 上传 / 播报 任一进行中。
 * 主循环看到它就: 不取景、不刷屏、不碰按键(按键归语音 task)。 */
bool voiceBusy();

/* 异步播报一句话, 立刻返回。
 * 拍照那条路(本地算出的用药结论)走这里 —— 不能同步播, 否则播报的几秒里
 * webuiLoop() 停摆, 手机打不开结果页, 而结果恰恰要显示在网页上。
 * 文本为空返回 true(什么都不做)。 */
bool voiceSpeakAsync(const char *text);

/* ---------- 结果 (给网页显示) ---------- */

/* 定长数组 + 临界区拷贝 —— 语音 task 写、主线程读, 用 String 跨任务传不安全 */
#define VOICE_TEXT_MAX    256
#define VOICE_REPLY_MAX   400

struct VoiceResult {
    bool     ok;
    char     text[VOICE_TEXT_MAX];      // ASR 转写的原文
    char     reply[VOICE_REPLY_MAX];    // 本地处理后给用户的答复(也就是播报的那句)
    uint32_t stamp;                     // millis(); 0 = 还没跑过语音
};

VoiceResult voiceLastResult();

/* ---------- 硬件自检 (MIC_SELFTEST=1 时由 setup 调, 不会返回) ---------- */
void voiceSelfTest();

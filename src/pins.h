#pragma once
/* ============================================================
 * 全项目的引脚定义 —— **只在这一处**。
 *
 * 以前散在 4 个文件里, 而且有两处重复: I2S 那五个脚 `voice.h` 和 `hwtest.cpp`
 * 各抄了一份, BOOT 脚 `main.cpp` 和 `voice.h` 各一份。换线时要改好几处,
 * 改漏一处的症状是「屏幕正常但麦克风没数据」这类极难查的问题。
 * 现在**改引脚只改这个文件**。
 *
 * ⚠️ 硬件约束(见 CLAUDE.md): 可用 GPIO 只有 3 / 38 / 39 / 40 / 41 / 42 / 45 / 46;
 *    22-25 在 ESP32-S3 上不存在; 3 / 45 / 46 是 strapping 脚, 上电瞬间有电平要求。
 * ============================================================ */

/* 把引脚号变成字符串, 写日志/屏幕提示时用它 —— 免得手抄一个数字,
   改了宏之后提示和实际走散(那种不一致特别费时间) */
#define PIN_STR_(x) #x
#define PIN_STR(x)  PIN_STR_(x)

/* ==================== ST7735S 屏 (SPI) ==================== */
#define TFT_SCLK_PIN   21
#define TFT_MOSI_PIN   14
#define TFT_CS_PIN     47
#define TFT_DC_PIN     48
#define TFT_RST_PIN     1     // 没接 RES 就写 (-1)
#define TFT_BL_PIN      2     // 没接 BLK 就接 3V3, 这里写 (-1)

/* ==================== OV3660 摄像头 (DVP) ==================== */
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

/* ==================== I2S: INMP441 麦克风 + MAX98357A 功放 ====================
 * BCLK/WS 两条线是**录音和播报共用**的, 任何时刻只有一个方向在跑。
 * ⚠️ 功放 SD 必须走 GPIO, 不能直接接 3V3 —— 否则录音时自激(见 CLAUDE.md 坑 #9) */
#define I2S_PIN_BCLK     41   // 共用位时钟 -> INMP441 SCK / MAX98357A BCLK
#define I2S_PIN_WS       42   // 共用帧同步 -> INMP441 WS  / MAX98357A LRC
#define I2S_PIN_MIC_SD   40   // 麦克风数据 -> INMP441 SD
#define I2S_PIN_SPK_DIN  39   // 功放数据   -> MAX98357A DIN
#define I2S_PIN_AMP_SD   38   // 功放使能   -> MAX98357A SD/EN (拉高才工作)

/* ==================== 按键 (原来是板上 BOOT 键 GPIO0) ====================
 * 换到别的脚时**只改这一行** —— main.cpp(按键状态机) 和 voice.cpp(录音松手检测)
 * 共用的就是它, 以前两边各写死一个 0, 改一处漏一处就变成「录音松不开手」。
 *
 * 接线: 按钮一端接这个脚, 另一端接 **GND**。代码按「按下 = 低电平」处理
 * (内部上拉在 main.cpp 的 setup() 里开)。
 *
 * ⚠️ **选脚之前先看这段: 这块板子上"干净"的脚已经没有了。**
 *    摄像头占用 4-18, 屏幕占 1/2/14/21/47/48, I2S 占 38-42,
 *    Flash + 八线 PSRAM 占 26-37(qio_opi), USB 占 19/20 —— 全排满了。
 *    剩下没被占的只有 **3 / 45 / 46**, 而这三个恰好都是 strapping 脚。
 *    所以要换, 现实中只有三条路:
 *      a) **用 GPIO3**(最省事) —— 它是 JTAG 源选择脚, 只在**上电那一瞬间**有影响,
 *         装上按钮后别在开机时按着它就行。日常当按键用没问题。
 *      b) **把 TFT_BL(2) 让出来** —— 背光直接接 3V3 常亮, 就腾出一个干净脚给按钮。
 *      c) 板上的按钮不用了, 挂个 PCF8574 之类的 I/O 扩展(但本项目的 I2C 脚被
 *         摄像头的 SCCB 占着, 得先确认能共用)。
 *    (45/46 是 VDD_SPI 电压选择和 ROM 打印使能, 上电被拉低会改变**启动行为**,
 *     比 GPIO3 更不建议。)
 *
 * ⚠️ GPIO0(板上 BOOT 键)自己也是 strapping 脚: 上电瞬间被拉低会进下载模式。
 *    所以换到普通 GPIO 反而更安全 —— 不会因为按钮卡住就进不了正常启动。
 * ⚠️ 换走之后就没有"按住那个键上电 → 进屏幕测试模式"这个入口了(这个功能本身
 *    跟着新脚走, 见 main.cpp 的 bootHeld); **烧录不受影响** —— 板上是 CH340,
 *    用 DTR/RTS 自动进下载模式, 不需要按键配合。 */
#define BUTTON_PIN       3

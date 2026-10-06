#pragma once
#include <Arduino.h>
#include <string>

/* 拍照 + 智谱 GLM 图像理解 —— 自包含, 不碰屏幕(tft)。
 * 摄像头相关的引脚和参数都在 vision.cpp, 屏幕相关的在 main.cpp。 */

/* 1 = 每次拍完照片, 保留那张 JPEG, 让人能用手机浏览器看到模型到底收到了什么。
 * 调识别精度时打开它 —— 是"图不清楚"还是"模型不行", 看一眼就定了, 不用猜。
 * 代价: 多占一份 JPEG 的 PSRAM(SHOT_PREVIEW_SECONDS 那 15 秒的阻塞只在
 *       TEST 模式下才有, 见下)。
 *
 * ⚠️ 两个模式看到的路径不一样:
 *   IS_TEST=1  临时网页(占 80 端口, 阻塞 15 秒) —— 板上没有别的服务器, 不冲突
 *   IS_TEST=0  常驻用药网页的 /photo 路由 —— **不能**再开临时服务器:
 *              常驻网页已经占了 80 端口, 两个 WebServer 抢同一个端口会
 *              bind 失败, 而且它不报错, 只是预览页打不开, 极难查
 * 图确认没问题了就把 SHOT_PREVIEW 改成 0。 */
#define SHOT_PREVIEW          1
#define SHOT_PREVIEW_SECONDS  15

/* 阶段回调: 用来在屏幕上显示进度。stage 是英文短句
 * (屏幕字体只有 ASCII, 显示不了中文), 传 nullptr 也行。 */
typedef void (*VisionStageCb)(const char *stage);

struct VisionResult {
    bool        ok;
    std::string text;    // ok=true 时是模型答案; ok=false 时是中文错误描述
};

/* 初始化成取景模式 (RGB565 QVGA), main.cpp 启动时调一次 */
bool visionInitViewMode();

/* 完整流程: 切 JPEG/VGA -> 拍照 -> base64 -> HTTPS 调智谱 -> 解析
 *           -> 无论成败都切回 RGB565/QVGA 取景模式。
 * 会阻塞数秒 (摄像头切换约 2~4 秒 + 网络往返), 期间不取景。 */
VisionResult visionCaptureAndAsk(const char *prompt, VisionStageCb cb = nullptr);

/* 拍照链路诊断: 依次试几种 pixformat/framesize 组合, 结果打到串口。
 * 由 main.cpp 的 BENCH_CAPTURE 开关触发; 跑完停在原地 (不会返回)。 */
void visionBenchCapture();

/* 把最近一次拍的照片通过临时网页放出去, 浏览器就能看。
 * 阻塞 seconds 秒(期间不取景), WiFi 没连或还没拍过照时直接返回。
 * ⚠️ 只在 IS_TEST 模式下用 —— 正常模式的常驻网页已经占了 80 端口。 */
void visionPreviewLastShot(uint16_t seconds);

/* 最近一次拍的 JPEG 字节(SHOT_PREVIEW=1 时才保留, 否则返回 nullptr)。
 * 给常驻网页的 /photo 路由用 —— 正常模式下不能临时再开一个 80 端口服务器。 */
const uint8_t *visionLastJpeg(size_t &len);

#include "logcfg.h"   /* 必须第一行: 日志配置要在 <Arduino.h> 之前生效 */
#include "strutil.h"  /* std::string 与 Arduino String 的桥接 */

static const char *TAG = "vision";    // 日志标签

#include "pins.h"
#include "vision.h"
#include "secrets.h"
#include "httpio.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#ifndef ZHIPU_API_KEY
#error "缺少 src/secrets.h —— 里面要定义 ZHIPU_API_KEY"
#endif

/* 摄像头引脚(CAM_PIN_*)搬到了 src/pins.h —— 和屏幕、I2S 放一起, 换线只改一处。
   这里往下就只剩「取景 / 拍照」两套模式参数了。 */

/* ==================== 取景 / 拍照 参数 ==================== */
/* OV3660 驱动在非 JPEG 格式下是按 16MHz XCLK 调的, 别乱改 */
#define CAM_XCLK_HZ     16000000

/* 取景模式: 用 QQVGA 160x120 —— 正好等于屏幕的 VIEW_W x VIEW_H, 1:1 直出不用缩放。
 *
 * 这不是随便挑的: OV3660 驱动给不同尺寸分配了两档 PLL, 帧率差近一倍 ——
 *   QVGA 及以上 -> 16MHz SYSCLK / 8MHz PCLK -> 10.25 FPS  (天花板)
 *   低于 QVGA   -> 32MHz SYSCLK / 8MHz PCLK -> 17.77 FPS
 * 所以想上 15FPS 就必须把采集降到 QVGA 以下, 代码再怎么优化都绕不过这一条。 */
#define VIEW_FRAMESIZE  FRAMESIZE_QQVGA
#define VIEW_FB_COUNT   2
#define VIEW_QUALITY    12            // RGB565 下无效, 但留着保持一致

/* 拍照模式: 发给模型认东西用。
 *
 * 注意: 这里拍的是 RGB565, 再在软件里编码成 JPEG —— 不是让传感器直接出 JPEG。
 * 实测这块 OV3660 的硬件 JPEG 完全不出帧 (QVGA/VGA/SVGA 全试过, 每次 8 秒超时零帧),
 * 而 RGB565 连 VGA 都能稳稳拿到 614400 字节的完整帧。
 * 原因在驱动侧: esp32-camera 的 ov3660 驱动里 sensor_fmt_jpeg[] 写的其实是
 * YUV422/YUYV 配置 (连它自己的注释都这么标), 传感器压根没切进 JPEG 模式,
 * 而 ESP32 这边是按 JPEG 去找 SOI/EOI 标记的, 所以一帧都攒不出来。
 * 改用 frame2jpg_cb() 软件编码既绕开这个坑, 输出还能直接进 PSRAM, 不占内部 RAM。 */
/* 分辨率尽量给足: OV3660 是 3MP 传感器, VGA 只用了它 1/15 的像素。
 * 拍小东西(虫子、元件丝印、远处物体)时, 分辨率往往比换模型更管用。
 * SVGA 800x600 的 RGB565 是 960000 字节, PSRAM 放得下, 软件编码约 440ms。
 * 想再清楚可以上 FRAMESIZE_XGA (1024x768, 编码约 720ms)。 */
#define SHOT_FRAMESIZE  FRAMESIZE_SVGA
#define SHOT_QUALITY    0             // 对应 camera_config 的 jpeg_quality: 0-63, 越小越清晰也越大
                                      // (保持最高画质; 上传提速走"大分块+自动降级", 不再动这里)
#define SHOT_FB_COUNT   1             // 1 张就够 —— 省下的内存留给 TLS 握手

/* frame2jpg_cb 的 quality 语义和上面相反: 0~100 且越大越好, 换算一下 (12 -> 81) */
#define SHOT_JPEG_Q100  (100 - (SHOT_QUALITY * 100) / 63)

/* 软件编码输出的缓冲 (PSRAM)。
 *
 * ⚠️ 它必须比实际 JPEG 大。小了的话 frame2jpg_cb 会中途返回 0、编码器直接中止,
 * 你会拿到一段**腰斩的 JPEG** —— 而 API 只会回一句「图片输入格式/解析错误」,
 * 从错误信息完全看不出是缓冲不够。
 *
 * 大小依据: SVGA 800x600 的 RGB565 原始数据是 960000 字节, 而 jpge 在最高质量
 * (SHOT_QUALITY=0) 下压缩比只有 1.8:1 左右 —— 512KB 都不够, 实测直接撞上限。
 * 本板是 R8N16 (8MB PSRAM), 给 1MB 绰绰有余。
 * 注意 base64 缓冲是这个的 1.33 倍, 算 PSRAM 峰值时要一起算。 */
#define SHOT_JPG_CAP    (1024 * 1024)

/* 编码结果离缓冲上限这么近就认为被腰斩了(而不是"刚好这么长") */
#define SHOT_JPG_NEAR_CAP 16

/* 0 = 不交换字节。这里是踩过坑的, 把结论记下来:
 *
 * esp32-camera 的 fmt2jpg 默认按【大端】解释 RGB565 —— 源码里
 *     static bool rgb565_big_endian = true;
 * 它拿第一个字节当高字节(R5+G3)、第二个当低字节(G3+B5)。
 * 而本项目的摄像头输出正好也是高字节在前 (见 main.cpp 的 CAM_MSB_FIRST=1,
 * 屏幕上颜色正常就是证据) —— 两边本来就一致, 【不需要交换】。
 *
 * 曾经误判成"取景路径翻了、JPEG 路径没翻", 在这里加了 bswap16,
 * 结果把正确的数据搅成位错乱: 画面上出现大块品红/青/绿、边缘全是彩色噪点,
 * 像热成像。注意区分: 那种"色块错乱"是字节序问题, 而"整体偏绿/偏黄"
 * 是传感器 AWB 的问题, 两码事。
 *
 * 换模组后如果颜色彻底错乱(而不是偏色), 再把它改成 1 试。
 * 库里另有 jpgSetRgb565BE(false) 可切, 但需要手动声明, 这里用不着。 */
#define SHOT_SWAP_BYTES  0

/* 1 = 编码前把整张图原地【上下翻转】。
 *
 * ⚠️ 2026-10-06 实测修正 —— 这里原来写的是"原地转 180°", 前提是
 *    "OV3660 输出上下颠倒 + 左右镜像"。**实测没有左右镜像, 只有上下颠倒。**
 *    于是 180° 多翻了半圈, 屏幕和照片【双双】变成左右镜像
 *    (字左右反着、上下正常)。两条路一起错, 所以两边看着一致, 反而不像 bug。
 *
 * 现在只翻上下: 行对调, 【列不动】。取景那条路(main.cpp 的 CAM_VFLIP)同步改。
 *
 * 480000 个像素原地翻约十几毫秒, 相对 450ms 的编码可以忽略。 */
#define SHOT_VFLIP    1

/* 1 = 编码前再把整张图原地【转 180°】(在 SHOT_VFLIP 的基础上)。
 *
 * 2026-10-11 加。和 SHOT_VFLIP 是**两码事**, 别混:
 *   SHOT_VFLIP  修的是**传感器出厂方向**(OV3660 非 JPEG 输出上下颠倒, 见上面)
 *   SHOT_ROT180 修的是**安装方向** —— 现场设备倒装, 就是"由于位置原因只能
 *               这样拍摄"造成的; 传感器再怎么修, 机器倒着, 出来的图就是倒的
 *
 * 为什么值得加: 药盒文字倒置会显著拉低认字率。同一张实拍图 PC 对照实验:
 *   倒着发 -> 3 次全错(「盐酸氯环利嗪片」读成「盐酸氟环利嗪片」)
 *   转正发 -> 3 次全对
 *
 * ⚠️ 取景(屏幕)特意**不做**同样的旋转, 和上面「两条路一致」的教训不冲突:
 *   取景是给人看的, 机器倒装时画面跟着倒才是"所见即所得"(屏幕上的提示文字
 *   是独立画的, 跟着转反而乱); 照片是给模型读字的, 转正只对它有好处。
 *   两个消费者目的不同, 各自取最优。
 *
 * 机器正装(位置恢复正常)之后改回 0。 */
#define SHOT_ROT180   1

/* 拍照前丢几帧, 等 AWB(自动白平衡) / AEC(自动曝光) 收敛。
 *
 * 传感器是拍照前才 init 的, 增益从零开始调整; 这时候立刻拍会拿到"未校正"
 * 的原始色彩 —— 典型症状是整体蒙一层黄绿, 而且偏暗。
 * 绿色尤其容易先出问题: Bayer 阵列里 G 占 50%(R/B 各 25%), 采样量是它们的两倍,
 * 白平衡一旦没配平, 先溢出的就是绿色。
 * SVGA 一帧约 150ms, 丢 3 帧约半秒。 */
#define SHOT_WARMUP_FRAMES  3

/* ==================== 智谱 API ==================== */
#define ZHIPU_HOST      "open.bigmodel.cn"
#define ZHIPU_PATH      "/api/paas/v4/chat/completions"

/* 模型 ID(2026-10-11 定): "glm-5v-turbo" —— 识图(拍照)链路用强模型。
 * 实测(同一张药盒实拍图): 药名读得准, 关掉思考后 2~9 秒出答案(首次 25 秒是冷启动)。
 * (备选 "glm-5.3-flash": 更轻, 但它是**始终思考**模型、关不掉(见下),
 *  ~6 秒; 留给以后可能加的纯问答场景, 这条链路暂时不用它。) */
#define ZHIPU_MODEL     "glm-5v-turbo"

/* 思考参数 —— ⚠️ 各模型写法**完全不同**, 换模型时必须一起改, 否则 API 直接 400:
 *   glm-5v-turbo    支持 "thinking":{"type":"disabled"} 关掉 —— 实测思考 0 字符, 2~9 秒
 *   glm-5.3-flash   **关不掉**, 发 disabled 会 400(「该模型始终思考，不支持关闭思考；
 *                   请使用 low、high 或 max」), 只能 "reasoning_effort":"low" —— 6~7 秒
 * 两种写法的**字段名都不一样**(thinking vs reasoning_effort), 所以直接存完整
 * JSON 片段, 比存值清楚。"" = 不附加任何字段(用模型默认, 最慢的那档)。 */
#define MODEL_EXTRA_PARAMS  ",\"thinking\":{\"type\":\"disabled\"}"

/* 上传诊断探针: 拍完照后, 把同一张图的 base64 先发一份到 postman-echo.com
   (先小包证明可达, 再大包), 用来区分「设备发不了大包」和「只对这个接口卡」。
   排查已完成 (结论: 大 TLS 记录会被对端拒, 64B 小块可用), 关掉它;
   需要重新扫描时改回 1。 */
#define SHOT_UPLOAD_PROBE 0

/* max_tokens 要给「思考 + 答案」留够空间。
 * 之前设 64 就是踩了这个坑: 思考过程把那点额度吃光, content 一个字没轮上,
 * 响应里 finish_reason 是 "length"、content 是空字符串。
 * 当前模型 glm-5v-turbo 关掉思考后回答很短(药名/用途一两句), 2048 实测
 * finish_reason=stop 收尾干净; 就算哪天把思考又打开, 这个量也够它想完再答。 */
#define SHOT_MAX_TOKENS   2048

#define TLS_TIMEOUT_S   15            // WiFiClientSecure 的 setTimeout 单位是「秒」

/* ==================== 摄像头 init / 切换 ==================== */
static bool camInit(pixformat_t fmt, framesize_t fsize, int fbCount, int quality)
{
    camera_config_t c = {};
    c.pin_pwdn     = CAM_PIN_PWDN;
    c.pin_reset    = CAM_PIN_RESET;
    c.pin_xclk     = CAM_PIN_XCLK;
    c.pin_sccb_sda = CAM_PIN_SIOD;
    c.pin_sccb_scl = CAM_PIN_SIOC;
    c.pin_d7       = CAM_PIN_D7;
    c.pin_d6       = CAM_PIN_D6;
    c.pin_d5       = CAM_PIN_D5;
    c.pin_d4       = CAM_PIN_D4;
    c.pin_d3       = CAM_PIN_D3;
    c.pin_d2       = CAM_PIN_D2;
    c.pin_d1       = CAM_PIN_D1;
    c.pin_d0       = CAM_PIN_D0;
    c.pin_vsync    = CAM_PIN_VSYNC;
    c.pin_href     = CAM_PIN_HREF;
    c.pin_pclk     = CAM_PIN_PCLK;

    c.xclk_freq_hz = CAM_XCLK_HZ;
    c.ledc_timer   = LEDC_TIMER_0;
    c.ledc_channel = LEDC_CHANNEL_0;

    c.pixel_format = fmt;
    c.frame_size   = fsize;
    c.jpeg_quality = quality;
    c.fb_count     = fbCount;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    /* 双缓冲才适合 GRAB_LATEST(永远拿最新帧); 单缓冲用 WHEN_EMPTY 更省心 */
    c.grab_mode    = (fbCount > 1) ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

    const esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "[CAM] init 失败 (fmt=%d size=%d fb=%d), err=0x%X",
                      (int)fmt, (int)fsize, fbCount, err);
        return false;
    }

    /* 补一次 set_framesize / set_quality。
       esp_camera_init 内部的顺序是 set_framesize() 然后 set_pixformat(),
       可 set_framesize 得先读 sensor->pixformat 才知道该选哪组 PLL 参数;
       而 set_pixformat 随后写的那批格式寄存器有可能又把时序设置覆盖回去。
       这里在最后重放一次, 让 PLL 成为最后落地的那个 —— 这是 RGB565 切 JPEG
       时最可疑的一环: JPEG 要走 40MHz SYSCLK, 被覆盖成别的值就一帧都出不来
       (而 RGB565 的 16MHz 恰好是个安全默认值, 所以切回取景又正常)。 */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        if (s->set_framesize) {
            const int r = s->set_framesize(s, fsize);
            if (r != 0) ESP_LOGI(TAG, "[CAM] 重放 set_framesize(%d) 失败: %d", (int)fsize, r);
        }
        if (fmt == PIXFORMAT_JPEG && s->set_quality) s->set_quality(s, quality);

        /* OV3660 的画质补偿 —— 取自 Arduino 官方 CameraWebServer 示例里
           针对 OV3660_PID 的那一段: 稍微提亮、降饱和。
           这颗传感器默认偏暗偏艳, 而"暗"和"艳"恰好最伤颜色辨认和文字阅读,
           模型认品牌/认字全靠这两样。只在拍照模式设, 取景不需要。 */
        if (fmt == PIXFORMAT_RGB565 && fsize >= FRAMESIZE_VGA) {
            if (s->set_brightness) s->set_brightness(s, 1);
            if (s->set_saturation) s->set_saturation(s, -2);

            /* 明确打开自动白平衡和 AWB 增益。
               驱动默认应该是开的, 但这里显式设一遍不亏 —— 偏绿多半就是它
               没生效或者没来得及收敛(配合下面的 SHOT_WARMUP_FRAMES 一起看)。 */
            if (s->set_whitebal) s->set_whitebal(s, 1);
            if (s->set_awb_gain) s->set_awb_gain(s, 1);
        }
    }
    return true;
}

/* 切换摄像头模式必须先 deinit 再 init:
 * framebuffer 是 esp_camera_init 时按 pixel_format + frame_size 算好分配的,
 * 运行时的 set_pixformat/set_framesize 不会重新分配它 —— 尺寸一变就把内存写花。 */
static bool camSwitch(pixformat_t fmt, framesize_t fsize, int fbCount, int quality)
{
    esp_camera_deinit();
    delay(80);                    // 给 PSRAM 和 sensor 一点时间收拾干净

    if (camInit(fmt, fsize, fbCount, quality)) return true;

    /* 重试一次: 偶发失败大多是上一轮还没完全释放 */
    ESP_LOGI(TAG, "[CAM] 切换失败, 重试一次...");
    esp_camera_deinit();
    delay(250);
    if (camInit(fmt, fsize, fbCount, quality)) return true;

    /* 两次都失败 —— 此时摄像头停在 deinit 状态, 主循环再也拿不到帧, 屏幕会一直黑。
       与其留一个黑屏、还只能手动复位才能救的设备, 不如自己重启:
       重启回来至少取景是好的, 用户还能继续用 */
    ESP_LOGI(TAG, "[CAM] 两次都失败, 重启恢复取景...");
    delay(400);
    ESP.restart();
    return false;                            // 走不到这里
}

static bool backToView()
{
    return camSwitch(PIXFORMAT_RGB565, VIEW_FRAMESIZE, VIEW_FB_COUNT, VIEW_QUALITY);
}

bool visionInitViewMode()
{
    /* 开机首次初始化特意不走 camSwitch: 那时还没有"当前模式"要释放,
       而且这里失败不该自动重启 —— 摄像头排线没插好的话会变成无限重启,
       交给调用方去屏幕上报错、停在那儿更好 */
    if (!camInit(PIXFORMAT_RGB565, VIEW_FRAMESIZE, VIEW_FB_COUNT, VIEW_QUALITY)) {
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    /* 这里打的是**请求**的配置。实际输出尺寸由驱动/传感器再定一档,
       开机第一条 [CAM] 实际输出 ... 才是真正在用的 —— 两行对不上是正常的,
       别看到"请求 QVGA"就以为拿到的是 320x240。 */
    if (s) ESP_LOGI(TAG, "[CAM] PID=0x%04X, 取景请求 RGB565 QVGA(320x240), 实际见下一行",
                    s->id.PID);
    return true;
}

/* ==================== 最近一次的照片 (供预览) ==================== */
static uint8_t *s_lastJpg    = nullptr;
static size_t   s_lastJpgLen = 0;

/* ==================== 软件 JPEG 编码 ====================
 * frame2jpg_cb 走回调把编码结果交给调用方, 这样输出可以直接落在 PSRAM 里 ——
 * 不带回调的 frame2jpg() 内部是裸 malloc, VGA 下要 60~120KB 内部 RAM,
 * 会把堆打碎, 紧跟着的 TLS 握手(要 2 个 16.4KB 连续块)就可能失败。 */
struct JpgBuf {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
};

static size_t jpgOutCb(void *arg, size_t index, const void *data, size_t len)
{
    JpgBuf *o = (JpgBuf *)arg;
    if (index + len > o->cap) return 0;          // 返回 0 让编码器知道装不下
    memcpy(o->buf + index, data, len);
    if (index + len > o->len) o->len = index + len;
    return len;
}

/* ==================== 小工具 ====================
 * HTTP 传输层的通用部分已抽到 httpio.cpp 共用 (语音识别那条链路也要用同一套):
 *   httpWriteAll / httpReadHeader / httpReadBody
 *   httpDecodeChunked / httpExtractContent / httpJsonEscape / httpConnect */

/* ==================== 主流程 ==================== */
VisionResult visionCaptureAndAsk(const char *prompt, VisionStageCb cb)
{
    VisionResult r = { false, "" };

    if (WiFi.status() != WL_CONNECTED) {
        r.text = "WiFi 没连上 —— 双击 BOOT 重新配网";
        return r;
    }

    /* ---- 1. 切到拍照模式 (RGB565 —— 硬件 JPEG 在这块板子上不出帧, 见宏注释) ---- */
    if (cb) cb("Capturing...");
    if (!camSwitch(PIXFORMAT_RGB565, SHOT_FRAMESIZE, SHOT_FB_COUNT, SHOT_QUALITY)) {
        /* 切失败时摄像头停在 deinit 状态, 必须切回取景模式,
           否则主循环的 esp_camera_fb_get() 会一直拿不到帧、屏幕全黑 */
        ESP_LOGI(TAG, "[CAM] 切拍照模式失败, 试着切回取景...");
        if (!backToView()) {
            ESP_LOGI(TAG, "[CAM] 取景模式也切不回来了, 请复位设备!");
            r.text = "摄像头切换失败, 请复位设备";
            return r;
        }
        r.text = "摄像头切换失败 (取景已恢复, 可以再按一次)";
        return r;
    }

    /* ---- 2. 先丢几帧, 让 AWB / AEC 收敛 ----
       传感器刚 init 完, 自动白平衡和自动曝光的增益还在从零开始调整。
       这时候立刻拍, 拿到的就是"未校正"的原始色彩 —— 典型症状是
       整体蒙一层黄绿(绿色通道在 Bayer 里占 50%, 最容易先溢出),
       而且偏暗。丢掉前几帧给它们时间稳住, 代价约半秒。 */
    for (int i = 0; i < SHOT_WARMUP_FRAMES; i++) {
        camera_fb_t *drop = esp_camera_fb_get();
        if (drop) esp_camera_fb_return(drop);
    }

    /* ---- 3. 拍一张 RGB565, 当场软件编码成 JPEG ---- */
    uint8_t *jpg = nullptr;
    size_t   jpgLen = 0;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        r.text = "拍照超时 —— 摄像头一帧都没出";
    } else if (fb->format != PIXFORMAT_RGB565) {
        ESP_LOGI(TAG, "[CAM] 帧格式不对: format=%d (期望 %d)",
                      (int)fb->format, (int)PIXFORMAT_RGB565);
        esp_camera_fb_return(fb);
        r.text = "拿到的帧格式不对 (详见串口)";
    } else {
#if SHOT_SWAP_BYTES
        /* 把字节序翻正, 否则 frame2jpg 解出来的红蓝是反的 (详见 SHOT_SWAP_BYTES 注释)。
           原地改 fb->buf 是安全的: 这块 buffer 此刻归我们独占, 编码完就还回去了。
           SVGA 48 万个像素, 这一趟大约十几毫秒, 相对 450ms 的编码可以忽略。 */
        {
            uint16_t    *px = (uint16_t *)fb->buf;
            const size_t n  = (size_t)fb->width * fb->height;
            for (size_t i = 0; i < n; i++) px[i] = __builtin_bswap16(px[i]);
        }
#endif

#if SHOT_VFLIP
        /* 原地上下翻转: 只把上下两半的行对调, 【列不动】。
           只遍历上半部分的行, 每个像素正好被交换一次。
           奇数高度时中间那行本来就在正中间, 原地不动。 */
        {
            uint16_t    *px = (uint16_t *)fb->buf;
            const size_t w  = fb->width;
            const size_t h  = fb->height;

            for (size_t y = 0; y < h / 2; y++) {
                uint16_t *rowA = px + y * w;
                uint16_t *rowB = px + (h - 1 - y) * w;
                for (size_t x = 0; x < w; x++) {
                    const uint16_t t = rowA[x];
                    rowA[x]           = rowB[x];
                    rowB[x]           = t;
                }
            }
        }
#endif

#if SHOT_ROT180
        /* 原地转 180° = 行对调 + 每行内左右反转。自包含(不依赖上面 VFLIP 的值):
           两个开关都是"再叠一层"的语义 —— 现场是 VFLIP=1 且机器倒装, 两个都开。
           48 万像素两趟遍历约几十毫秒, 相对 450ms 的编码可以忽略。 */
        {
            uint16_t    *px = (uint16_t *)fb->buf;
            const size_t w  = fb->width;
            const size_t h  = fb->height;

            for (size_t y = 0; y < h / 2; y++) {          // ① 行对调(上下翻)
                uint16_t *rowA = px + y * w;
                uint16_t *rowB = px + (h - 1 - y) * w;
                for (size_t x = 0; x < w; x++) {
                    const uint16_t t = rowA[x];
                    rowA[x]           = rowB[x];
                    rowB[x]           = t;
                }
            }

            for (size_t y = 0; y < h; y++) {              // ② 每行内左右反转
                uint16_t *row = px + y * w;
                for (size_t x = 0; x < w / 2; x++) {
                    const uint16_t t = row[x];
                    row[x]             = row[w - 1 - x];
                    row[w - 1 - x]     = t;
                }
            }
        }
#endif

        /* 打印照片中心 32x32 的平均色 —— 判断偏色最直接的办法。
           对着白纸或灰卡拍: 三个数应该接近相等, 哪个明显高就是往哪个色偏。
           读法和 frame2jpg 保持一致(小端), 所以它反映的就是模型实际看到的颜色,
           跟屏幕上看到的可能不一样 (取景那条路自己处理过字节序)。 */
        {
            const int cw = 32, ch = 32;
            const int x0 = ((int)fb->width  - cw) / 2;
            const int y0 = ((int)fb->height - ch) / 2;
            uint32_t sr = 0, sg = 0, sb = 0;

            for (int y = 0; y < ch; y++) {
                const uint8_t *row = fb->buf + (size_t)(y0 + y) * fb->width * 2;
                for (int x = 0; x < cw; x++) {
                    const uint8_t *p = row + (size_t)(x0 + x) * 2;
                    const uint16_t v = (uint16_t)(p[0] | (p[1] << 8));   // 小端, 同 frame2jpg
                    sr += (v >> 11) & 0x1F;
                    sg += (v >> 5)  & 0x3F;
                    sb += v & 0x1F;
                }
            }
            const uint32_t n = (uint32_t)cw * ch;
            ESP_LOGI(TAG, "[IMG] 照片中心平均色: R=%u G=%u B=%u  (0-255, 相等=白平衡正常)",
                          (unsigned)(sr / n * 255 / 31),
                          (unsigned)(sg / n * 255 / 63),
                          (unsigned)(sb / n * 255 / 31));

            /* 再把中心那个像素的原始字节打出来。
               只信平均值会被骗: R/B 通道错位时, 如果两者数值接近, 平均值几乎不变,
               看着还挺正常 —— 但画面已经天翻地覆了。原始字节骗不了人。 */
            const uint8_t *pc = fb->buf +
                ((size_t)(y0 + ch / 2) * fb->width + (x0 + cw / 2)) * 2;
            const uint16_t vc = (uint16_t)((pc[0] << 8) | pc[1]);   // 同 frame2jpg 的读法
            ESP_LOGI(TAG, "[IMG] 中心像素原始字节: %02X %02X -> R=%u G=%u B=%u",
                          pc[0], pc[1],
                          (unsigned)(((vc >> 11) & 0x1F) * 255 / 31),
                          (unsigned)(((vc >> 5)  & 0x3F) * 255 / 63),
                          (unsigned)((vc & 0x1F) * 255 / 31));
        }

        uint8_t *buf = (uint8_t *)heap_caps_malloc(SHOT_JPG_CAP, MALLOC_CAP_SPIRAM);
        if (!buf) {
            r.text = "PSRAM 不够放 JPEG 输出缓冲";
        } else {
            JpgBuf out = { buf, SHOT_JPG_CAP, 0 };
            const uint32_t t0 = millis();
            const bool encOk = frame2jpg_cb(fb, SHOT_JPEG_Q100, jpgOutCb, &out);

            if (encOk && out.len > 0 &&
                out.len < (size_t)SHOT_JPG_CAP - SHOT_JPG_NEAR_CAP) {
                jpg    = buf;
                jpgLen = out.len;
                ESP_LOGI(TAG, "[CAM] RGB565 %ux%u -> JPEG %u 字节 (软件编码 %u ms)",
                              (unsigned)fb->width, (unsigned)fb->height,
                              (unsigned)out.len, (unsigned)(millis() - t0));
            } else if (out.len >= (size_t)SHOT_JPG_CAP - SHOT_JPG_NEAR_CAP) {
                /* 贴着上限 = 被腰斩。这种帧发出去 API 只会回「格式错误」,
                   在这里拦住并说清楚原因, 比让用户去猜 400 是什么意思强 */
                ESP_LOGI(TAG, "[CAM] JPEG 撞到缓冲上限 (%u / %u 字节) —— 图被截断了!"
                              "       要么调大 SHOT_JPG_CAP, 要么把 SHOT_QUALITY 调大(降质量)\n",
                              (unsigned)out.len, (unsigned)SHOT_JPG_CAP);
                heap_caps_free(buf);
                r.text = "JPEG 超出缓冲被截断 —— 调大 SHOT_JPG_CAP 或调小 SHOT_QUALITY";
            } else {
                heap_caps_free(buf);
                r.text = "软件 JPEG 编码失败";
            }
        }
        esp_camera_fb_return(fb);   // 编码完立刻还 —— 切模式前不能有未归还的 fb
    }

    /* ---- 3. 立刻切回取景模式 ----
       JPEG 已经在手上了, 后面上传要好几秒, 没必要占着摄像头 */
    if (!backToView()) {
        ESP_LOGI(TAG, "[CAM] 切回取景模式失败!");
        if (jpg) heap_caps_free(jpg);
        r.text = "切回取景模式失败, 请复位";
        return r;
    }

    if (!jpg) return r;                      // 拍照阶段已经写好错误原因

    /* ---- 4. base64 到 PSRAM ---- */
    if (cb) cb("Encoding...");
    const size_t b64Cap = 4 * ((jpgLen + 2) / 3) + 1;
    uint8_t *b64 = (uint8_t *)heap_caps_malloc(b64Cap, MALLOC_CAP_SPIRAM);
    if (!b64) {
        heap_caps_free(jpg);
        r.text = "PSRAM 不够做 base64";
        return r;
    }
    size_t b64Len = 0;
    if (mbedtls_base64_encode(b64, b64Cap, &b64Len, jpg, jpgLen) != 0) {
        heap_caps_free(jpg);
        heap_caps_free(b64);
        r.text = "base64 编码失败";
        return r;
    }
#if SHOT_PREVIEW
    /* 留一份下来给预览网页用 (下一张拍完时再释放上一份) */
    if (s_lastJpg) { heap_caps_free(s_lastJpg); s_lastJpg = nullptr; }
    s_lastJpg    = jpg;
    s_lastJpgLen = jpgLen;
    jpg = nullptr;                           // 所有权转走了, 后面别再 free
#else
    heap_caps_free(jpg);                     // 原图用完就扔, 压峰值内存
#endif
    ESP_LOGI(TAG, "[IMG] 原图 %u -> base64 %u 字节",
                  (unsigned)jpgLen, (unsigned)b64Len);

    size_t uploadChunkGuess = 64;   /* 探针扫描出的可用块大小; 缺省 64 (实测最小可用) */
#if SHOT_UPLOAD_PROBE
    /* ---- 诊断探针: TLS 记录大小扫描 ----
       背景: 明文 HTTP 大包完全正常, 但 TLS 记录一大就被对端 RST ——
       问题在 mbedtls 发出去的大记录上。这里从小到大逐档试整包上传,
       第一档能整包通过就停, 那就是可行的修复参数。
       结果只看 [PROBE]/[写] 日志。排查完把 SHOT_UPLOAD_PROBE 改回 0 */
    {
        auto probeTls = [&](const uint8_t *payload, size_t plen, const char *tag, size_t chunk) -> bool {
            WiFiClientSecure pcli;
            if (!httpConnect(pcli, "postman-echo.com", 443, TLS_TIMEOUT_S)) {
                ESP_LOGW(TAG, "[PROBE] %s: TLS 连接失败", tag);
                return false;
            }
            const char *kPre = "{\"probe\":\"";
            const char *kMid = "\",\"junk\":\"";
            const char *kSuf = "\"}";
            const size_t pBodyLen = strlen(kPre) + strlen(tag) + strlen(kMid) + plen + strlen(kSuf);
            pcli.printf("POST /post HTTP/1.1\r\n");
            pcli.printf("Host: postman-echo.com\r\n");
            pcli.print("Content-Type: application/json\r\n");
            pcli.printf("Content-Length: %u\r\n", (unsigned)pBodyLen);
            pcli.print("Connection: close\r\n\r\n");
            pcli.print(kPre);
            pcli.print(tag);
            pcli.print(kMid);
            const uint32_t tp = millis();
            const bool ok = httpWriteAllChunk(pcli, payload, plen, chunk);
            if (ok) pcli.print(kSuf);
            ESP_LOGW(TAG, "[PROBE] %s: payload %u 字节 (chunk %u), 写%s, 耗时 %u ms",
                          tag, (unsigned)plen, (unsigned)chunk, ok ? "完成" : "未完成",
                          (unsigned)(millis() - tp));
            if (ok) {
                bool ch = false;
                const int code = httpReadHeader(pcli, ch);
                const std::string body = httpReadBody(pcli, 5000);
                ESP_LOGW(TAG, "[PROBE] %s: 响应码 %d, 体 %u 字节", tag, code, (unsigned)body.size());
            }
            pcli.stop();
            return ok;
        };
        static const uint8_t kSmallProbe[] = "hello-clawd-probe";
        probeTls(kSmallProbe, sizeof(kSmallProbe) - 1, "small", 4096);
        /* 从大到小扫描: 找到最大的可用块 (块越大上传越快); 64 是已验证下限 */
        static const size_t kChunks[] = {1024, 512, 256, 128, 64};
        for (size_t ci = 0; ci < sizeof(kChunks) / sizeof(kChunks[0]); ci++) {
            char tg[16];
            snprintf(tg, sizeof(tg), "scan-%u", (unsigned)kChunks[ci]);
            if (probeTls(b64, b64Len, tg, kChunks[ci])) {
                uploadChunkGuess = kChunks[ci];
                ESP_LOGW(TAG, "[PROBE] 扫描结束: chunk=%u 字节可以整包通过", (unsigned)kChunks[ci]);
                break;
            }
        }
    }
#endif

    /* ---- 5. 拼请求 ---- */
    const std::string prefix =
        std::string("{\"model\":\"") + ZHIPU_MODEL +
        "\",\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    std::string suffix =
        std::string("\"}},{\"type\":\"text\",\"text\":\"") + httpJsonEscape(prompt) +
        "\"}]}],\"max_tokens\":" + std::to_string(SHOT_MAX_TOKENS) + ",\"temperature\":0.1";
    /* 思考参数: 各模型写法不同, 见 MODEL_EXTRA_PARAMS 的注释 */
    suffix += MODEL_EXTRA_PARAMS;
    suffix += "}";
    const size_t bodyLen = prefix.size() + b64Len + suffix.size();

    /* ---- 6. HTTPS 发出去 ---- */
    if (cb) cb("Uploading...");

    WiFiClientSecure client;
    /* httpConnect() 里做了 setInsecure + setTimeout(单位秒) + connect ——
       三步的顺序有讲究, 见 httpio.h 的注释 */
    if (!httpConnect(client, ZHIPU_HOST, 443, TLS_TIMEOUT_S)) {
        heap_caps_free(b64);
        ESP_LOGI(TAG, "[API] TLS 连接失败");
        r.text = "TLS 连接失败 (内存不够或网络不通)";
        return r;
    }
    ESP_LOGI(TAG, "[API] TLS 已连接, 准备发 %u 字节 body", (unsigned)bodyLen);

    client.printf("POST %s HTTP/1.1\r\n", ZHIPU_PATH);
    client.printf("Host: %s\r\n", ZHIPU_HOST);
    client.printf("Authorization: Bearer %s\r\n", ZHIPU_API_KEY);
    client.print("Content-Type: application/json\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)bodyLen);
    client.print("Connection: close\r\n\r\n");

    /* body 分块写, 不拼成一整个大 String —— 那样要再拷一份 100+KB。
       writeAll() 里面会循环, 单次 write() 发不完一个 TLS 记录之外的部分 */
    client.print(prefix.c_str());
    const uint32_t tBody = millis();
    const bool bodySent = httpWriteAllChunk(client, b64, b64Len, uploadChunkGuess);
    ESP_LOGI(TAG, "[API] 正文写%s, 耗时 %u ms",
                  bodySent ? "完成" : "未完成", (unsigned)(millis() - tBody));
    if (bodySent) client.print(suffix.c_str());
    heap_caps_free(b64);

    if (!bodySent) {
        ESP_LOGI(TAG, "[API] 图片没能发完");
        /* 诊断: 卡住时对端很可能早就回了响应(比如 4xx), 只是没人读过。
           把连接里已经到的字节捞出来看一眼, 再断开 */
        std::string early;
        const uint32_t tR = millis();
        while (millis() - tR < 1500 && early.size() < 600) {
            while (client.available() && early.size() < 600)
                early += (char)client.read();
            if (!client.connected() && !client.available()) break;
            delay(5);
        }
        ESP_LOGI(TAG, "[API] 断开前读到对端 %u 字节 (connected=%d): %s",
                      (unsigned)early.size(), (int)client.connected(), early.c_str());
        /* 诊断: mbedtls 自己的错误串 —— 能区分是 socket 超时/连接重置/内存不够 */
        char errBuf[160] = {0};
        const int errRes = client.lastError(errBuf, sizeof(errBuf));
        ESP_LOGW(TAG, "[API] mbedtls lastError(%d): %s | RSSI=%d, WiFi.status=%d",
                      errRes, errBuf, (int)WiFi.RSSI(), (int)WiFi.status());
        client.stop();
        r.text = "上传中断 (图片没发完)";
        return r;
    }

    /* ---- 7. 收响应 ---- */
    if (cb) cb("Waiting...");

    bool chunked = false;
    const int code = httpReadHeader(client, chunked);

    const std::string raw = httpReadBody(client, 20000);   // 总读取超时 20 秒
    client.stop();

    const std::string body = chunked ? httpDecodeChunked(raw) : raw;

    if (code != 200) {
        ESP_LOGE(TAG, "[API] HTTP %d, 原始响应(前512字节): %s",
                 code, strSub(body, 0, 512).c_str());
        r.text = "HTTP " + std::to_string(code) + " —— 详见串口";
        return r;
    }

    const std::string answer = httpExtractContent(body);
    if (answer.empty()) {
        /* content 是空的有两种典型原因, 分开提示好排障 */
        if (strIndex(body, "reasoning_content") >= 0) {
            ESP_LOGI(TAG, "[API] content 是空的, 但响应里有 reasoning_content ——");
            ESP_LOGI(TAG, "      模型的思考过程把 max_tokens 吃光了, 答案没来得及生成。");
            ESP_LOGI(TAG, "      检查 DISABLE_THINKING 是否生效, 或把 SHOT_MAX_TOKENS 再调大");
        }
        ESP_LOGI(TAG, "[API] 原始响应(前512字节): %s", strSub(body, 0, 512).c_str());
        r.text = "模型没返回答案 (详见串口)";
        return r;
    }

    r.ok   = true;
    r.text = answer;
    return r;
}

/* ==================== 拍照链路诊断 ====================
 * 一次烧录把几种模式组合全试一遍, 好判断问题出在
 * 「JPEG 不支持 / 某个分辨率不支持 / 还是第一次切换本身就不行」。 */
void visionBenchCapture()
{
    struct Case { framesize_t size; const char *name; };
    static const Case cases[] = {
        { FRAMESIZE_QVGA, "RGB565 QVGA 320x240" },
        { FRAMESIZE_VGA,  "RGB565 VGA  640x480" },
        { FRAMESIZE_SVGA, "RGB565 SVGA 800x600" },
    };

    ESP_LOGI(TAG, "\n========== 拍照链路诊断 ==========");
    ESP_LOGI(TAG, "(取景模式是 RGB565 QVGA, 下面每个都是从这里切过去的)");
    ESP_LOGI(TAG, "JPEG 硬件模式已知在这块 OV3660 上不出帧, 不再测试");

    for (const Case &c : cases) {
        ESP_LOGI(TAG, "\n--- %s ---", c.name);

        if (!camSwitch(PIXFORMAT_RGB565, c.size, 1, SHOT_QUALITY)) {
            ESP_LOGI(TAG, "    init 失败");
            continue;
        }

        const uint32_t t0 = millis();
        camera_fb_t *fb = esp_camera_fb_get();
        const uint32_t dt = millis() - t0;

        if (!fb) {
            ESP_LOGI(TAG, "    拿不到帧 (等了 %u ms)", (unsigned)dt);
            continue;
        }
        ESP_LOGI(TAG, "    拍到 %u 字节 %ux%u (等了 %u ms)",
                      (unsigned)fb->len, (unsigned)fb->width, (unsigned)fb->height,
                      (unsigned)dt);

        /* 顺便量一下软件编码的耗时和产物大小 —— 这才是真正要上传的东西 */
        uint8_t *buf = (uint8_t *)heap_caps_malloc(SHOT_JPG_CAP, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGI(TAG, "    PSRAM 不够, 跳过编码测试");
        } else {
            JpgBuf out = { buf, SHOT_JPG_CAP, 0 };
            const uint32_t t1 = millis();
            if (frame2jpg_cb(fb, SHOT_JPEG_Q100, jpgOutCb, &out) && out.len > 0) {
                ESP_LOGI(TAG, "    编码成 JPEG %u 字节 (耗时 %u ms), base64 后约 %u 字节",
                              (unsigned)out.len, (unsigned)(millis() - t1),
                              (unsigned)(4 * ((out.len + 2) / 3)));
            } else {
                ESP_LOGI(TAG, "    软件 JPEG 编码失败");
            }
            heap_caps_free(buf);
        }
        esp_camera_fb_return(fb);
    }

    ESP_LOGI(TAG, "\n========== 诊断结束 ==========");
    ESP_LOGI(TAG, "把上面这段发给开发者; 掉电重启即可恢复正常模式");
    for (;;) delay(1000);          // 停住, 别让取景把日志冲掉
}

/* ==================== 照片预览 ====================
 * 把刚拍的 JPEG 挂在临时网页上, 手机浏览器打开就能看。
 * 调识别精度时这是最直接的诊断手段: 一眼就能分清是"图本身不行"
 * 还是"模型认不出", 不用靠模型的描述去猜。 */
void visionPreviewLastShot(uint16_t seconds)
{
    if (!s_lastJpg || s_lastJpgLen == 0) {
        ESP_LOGI(TAG, "[IMG] 还没有可预览的照片");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGI(TAG, "[IMG] WiFi 没连, 没法预览");
        return;
    }

    WebServer server(80);

    server.on("/", HTTP_GET, [&server]() {
        server.send(200, "text/html; charset=utf-8",
                    "<!DOCTYPE html><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width,initial-scale=1'>"
                    "<body style='font-family:sans-serif;margin:12px'>"
                    "<b>刚发给模型的就是这张</b><br><br>"
                    "<img src=/photo style='max-width:100%;border:1px solid #888'>"
                    "</body>");
    });

    server.on("/photo", HTTP_GET, [&server]() {
        server.send_P(200, "image/jpeg", (PGM_P)s_lastJpg, s_lastJpgLen);
    });

    server.onNotFound([&server]() { server.send(404, "text/plain", "404"); });

    server.begin();

    ESP_LOGI(TAG, "\n[IMG] 照片预览已开 —— 浏览器打开 %s  (%.0f 秒后自动关)",
                  (std::string("http://") + toStd(WiFi.localIP().toString())).c_str(),
                  (double)seconds);
    ESP_LOGI(TAG, "[IMG] 这张 JPEG %u 字节, 就是刚发给模型的那张",
                  (unsigned)s_lastJpgLen);

    const uint32_t t0 = millis();
    while (millis() - t0 < (uint32_t)seconds * 1000) {
        server.handleClient();
        delay(2);
    }

    server.stop();
    ESP_LOGI(TAG, "[IMG] 预览结束, 恢复取景");
}

const uint8_t *visionLastJpeg(size_t &len)
{
    len = s_lastJpgLen;
    return s_lastJpg;
}

#pragma once

/* 统一的日志配置 —— 每个 .cpp 的**第一行** #include "logcfg.h"。
 *
 * 这个文件把三件必须在正确时机做的事包在一起了, 调用方不用操心顺序:
 *
 *   ① 在 <Arduino.h> **之前** 定下编译期开关
 *      - LOG_LOCAL_LEVEL      IDF 层级别上限。不定义会用 CONFIG_LOG_MAXIMUM_LEVEL=1
 *                             (只保留 ERROR), ESP_LOGI/W 被编译成空语句
 *      - CORE_DEBUG_LEVEL     Arduino 层同级的关卡(platformio.ini 给的是 1),
 *                             log_i / log_w 在编译期按它裁剪
 *      - CONFIG_ARDUHAL_LOG_COLORS  颜色开关, 默认 0(关)
 *   ② 包含 <Arduino.h>(它会把 esp32-hal-log.h 一起带进来)
 *   ③ 在它**之后** 改掉默认格式 —— ARDUHAL_LOG_FORMAT 没有 #ifndef 保护,
 *      只能等它展开后再 #undef 重定义
 *
 * 为什么不用 platformio.ini 的 build_flags:
 *   改它要触发 9.5 分钟全量重建, 而这三个开关用文件内定义就能覆盖。
 *
 * 输出格式(精简版, 一行装得下):
 *     [   460][E][voice] HTTP 401
 *     [   471][W][voice] 整段几乎没有声音 (峰值 42)
 *     [  1234][I][main]  === 取景器启动 ===
 *  其中 [voice] 是各文件自己定义的 TAG(见 ESP_LOGx 的第一个参数)。
 *
 * 级别: 0=NONE 1=ERROR 2=WARN 3=INFO 4=DEBUG 5=VERBOSE
 *       颜色: ERROR=红 WARN=黄 INFO=绿 DEBUG=青 VERBOSE=白
 *
 * 级别**在编译期固定**, 不用 esp_log_level_set() 做运行时调节 ——
 * 这样"哪条日志会出来"完全可预测, 不会有"某处偷偷调了级别导致日志消失"这种事。
 *
 * 代价: CORE_DEBUG_LEVEL=4 会把 Arduino 内核和各库(esp32-camera / WiFi / lwIP)
 * 的 DEBUG 日志一起放出来, 没法单独压某一个库。
 * 真嫌吵就把它降到 3(INFO) —— 但那样本项目的 ESP_LOGD 也会一起没。
 * 要更细的粒度只能拆文件(不同 .cpp 各自定义不同的 CORE_DEBUG_LEVEL)。 */

/* ============ ① 必须在 <Arduino.h> 之前 ============ */

#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#undef CORE_DEBUG_LEVEL
#define CORE_DEBUG_LEVEL 4                  /* 保留到 DEBUG 级 */

#ifndef CONFIG_ARDUHAL_LOG_COLORS
#define CONFIG_ARDUHAL_LOG_COLORS 1         /* 彩色输出 */
#endif

/* ============ ② 包含 Arduino ============ */

#include <Arduino.h>

/* ============ ③ 之后才能改的: 日志格式 ============ */

/* Arduino 默认的格式带「文件:行号 函数名」, 一行 60+ 字符, 160 宽的串口容易折行。
 * 换成 [时间][级别] 的短格式。
 *
 * ⚠️ 参数列表必须和 format 里的占位符一一对应 —— 原版是
 *    (时间戳, 文件名, 行号, 函数名) 四个, 这里只留时间戳, 所以后面的
 *    pathToFileName(__FILE__) / __LINE__ / __FUNCTION__ 都要去掉,
 *    否则 printf 会因为参数对不上而打出乱码。
 *
 * TAG 是 esp_log_printf_() 自己拼的, 不受这里影响。 */
#undef ARDUHAL_LOG_FORMAT
#define ARDUHAL_LOG_FORMAT(letter, format)                              \
    ARDUHAL_LOG_COLOR_ ## letter "[%6u][" #letter "] " format           \
    ARDUHAL_LOG_RESET_COLOR "\r\n",                                     \
    (unsigned long)(esp_timer_get_time() / 1000ULL)

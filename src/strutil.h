#pragma once

/* std::string 桥接层 + 常用字符串工具。
 *
 * 背景: 本项目的字符串处理统一用 C++ STL 的 std::string, 但 Arduino 的几个库
 * (WiFi / WebServer / Preferences / ESP) 的 API 强制返回或接收 Arduino String ——
 * 那些是第三方库, 改不掉。所以只在**边界处**用 toStd() / toArd() 过一手,
 * 库返回值立刻转成 std::string, 传回库之前再转回去。
 *
 * 另外 std::string 没有 Arduino String 的那些便利方法(indexOf / substring /
 * trim / toInt / startsWith), 这里按**语义对齐**补齐, 迁移时逐个换名即可:
 *
 *     s.indexOf("x")          ->  strIndex(s, "x")          找不到返回 -1
 *     s.substring(2, 5)       ->  strSub(s, 2, 5)           左闭右开
 *     s.substring(2)          ->  strSub(s, 2)              到结尾
 *     s.trim()                ->  strTrim(s)                原地改
 *     s.toInt()               ->  strToInt(s)               失败返回 0, 不抛
 *
 * ⚠️ 最容易踩的坑: Arduino String 支持**隐式数字拼接**(`s += 123`), std::string
 * 不支持 —— 必须显式 std::to_string(123)。漏了会编译报错(好情况), 但传指针时
 * 会静默拼出地址值(坏情况)。迁移时逐处核对。
 *
 * 用法: 每个用到字符串的 .cpp 顶部(紧跟 logcfg.h 之后)
 *       #include "strutil.h" */

#include "logcfg.h"         /* 保证 <Arduino.h> 已进来(String 定义在它里面) */

#include <string>
#include <cstring>
#include <cstdlib>

/* ==================== 与 Arduino String 互转 ==================== */

inline std::string toStd(const String &s) { return std::string(s.c_str()); }
inline String      toArd(const std::string &s) { return String(s.c_str()); }

/* ==================== 常用操作 ==================== */

/* 找不到返回 -1 —— 对齐 Arduino String::indexOf 的语义
   (std::string::find 返回的是 size_t npos, 直接比较会踩坑)。
   from 是起始位置, 对齐 indexOf(needle, from) 那个重载。 */
inline int strIndex(const std::string &s, const char *needle, size_t from = 0)
{
    const size_t p = s.find(needle, from);
    return (p == std::string::npos) ? -1 : (int)p;
}

inline int strIndex(const std::string &s, const std::string &needle, size_t from = 0)
{
    return strIndex(s, needle.c_str(), from);
}

/* 左闭右开, 对齐 Arduino String::substring; to 省略表示到结尾 */
inline std::string strSub(const std::string &s, size_t from, size_t to = std::string::npos)
{
    if (from >= s.size()) return std::string();
    if (to > s.size())    to = s.size();
    if (to <= from)       return std::string();
    return s.substr(from, to - from);
}

/* 去首尾空白, 原地修改 */
inline void strTrim(std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') b++;
    while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
    s.assign(s, b, e - b);
}

/* 解析失败返回 0 —— 对齐 Arduino String::toInt(它不抛异常,
   而 std::stoi 会抛 std::invalid_argument, 在嵌入式里没 try/catch 兜着就是崩) */
inline int strToInt(const std::string &s) { return atoi(s.c_str()); }

inline bool strStartsWith(const std::string &s, const char *prefix)
{
    const size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>

/* 16x16 中文点阵字库。
 *
 * 数据在 cnfont_data.h (由 tools/gen_cnfont.py 生成), 7445 字 / 247KB,
 * 直接编进固件放 flash 里, 不占 RAM。
 *
 * 查找用二分: 字库按 Unicode 码点升序排列, 所以不需要 UTF-8→GB2312 的
 * 转换表(那要 14KB 外加一堆转换代码)。O(log n) 约 13 次比较, 够快。 */

#define CN_CHAR_W   16
#define CN_CHAR_H   16

/* 字库里的汉字/符号总数。
 * 刻意用函数而不是宏: 数据本身是 static const 数组, 谁 include 数据头谁就多一份副本,
 * 247KB 说多不多但也没必要浪费。让它只存在于 cnfont.cpp 一个编译单元里。 */
uint32_t cnFontCount();

/* 查一个字的点阵(32 字节), 找不到返回 nullptr。传 Unicode 码点, 不是 UTF-8 字节 */
const uint8_t *cnGlyph(uint16_t cp);

/* 查一个 ASCII 字符的 8x16 点阵(16 字节, 每行 1 字节)。
 * 范围 0x20~0x7E, 越界或不可打印返回 nullptr。
 * 直接索引(不二分): ASCII 码点连续, c - 0x20 就是下标。
 * 宽是 8 而不是 16 —— 英文本来就窄, 塞进 16 宽的格子又大又空。 */
const uint8_t *cnAsciiGlyph(uint8_t c);

/* 按存储顺序取第 idx 个字的码点; 越界返回 0。
 * 字库自检模式按页翻字时用。 */
uint16_t cnCodePointAt(uint32_t idx);

/* 画一段 UTF-8 字符串(可混 ASCII)。
 * ASCII 走 Adafruit 内置字体, 其余走点阵字库; 字库里没有的字符画成方框。
 * 返回画完后的 x 坐标。 */
int16_t cnDraw(Adafruit_GFX &g, int16_t x, int16_t y, const char *utf8, uint16_t color);

/* 按字库渲染后这段字符串占多宽(像素) */
int16_t cnWidth(const char *utf8);

/* 按存储顺序画第 idx 个字 —— 字库自检翻页用(它要按码点顺序遍历, 不是按字符串) */
void cnDrawIndexed(Adafruit_GFX &g, int16_t x, int16_t y, uint32_t idx, uint16_t color);

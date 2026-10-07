#include "cnfont.h"
#include "cnfont_data.h"

/* 每字 34 字节: [码点 2 字节小端][点阵 32 字节] */
#define CN_REC 34

/* ==================== UTF-8 解码 ==================== */
/* 取出下一个字符的码点并前移指针。返回 0 表示到结尾或遇到非法序列。 */
static uint16_t utf8Next(const char *&p)
{
    const uint8_t c = (uint8_t)*p;
    if (c == 0) return 0;

    if (c < 0x80) {                       // ASCII
        p++;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        const uint16_t cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        const uint16_t cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
        return cp;
    }
    /* 4 字节: 超出 BMP, 字库不收。
       ⚠️ 也要校验后续字节 —— 直接 p += 4 在字符串被截断或数据损坏时会**跨过结尾的
       '\0' 继续往后读**。校验之后不合法就落到下面按"非法字节跳过 1 字节"处理。 */
    if ((c & 0xF8) == 0xF0 &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        p += 4;
        return 0xFFFF;
    }
    p++;                                  // 非法字节, 跳过
    return 0;
}

uint32_t cnFontCount()
{
    return CNFONT_COUNT;
}

/* ==================== 查找 ==================== */
static uint16_t recCode(uint32_t idx)
{
    return (uint16_t)(cnfont_data[idx * CN_REC] |
                      (cnfont_data[idx * CN_REC + 1] << 8));
}

const uint8_t *cnGlyph(uint16_t cp)
{
    if (cp == 0) return nullptr;

    int32_t lo = 0, hi = CNFONT_COUNT - 1;
    while (lo <= hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        const uint16_t v  = recCode(mid);
        if (v == cp) return cnfont_data + mid * CN_REC + 2;
        if (v < cp)  lo = mid + 1;
        else         hi = mid - 1;
    }
    return nullptr;
}

uint16_t cnCodePointAt(uint32_t idx)
{
    return (idx < CNFONT_COUNT) ? recCode(idx) : 0;
}

const uint8_t *cnAsciiGlyph(uint8_t c)
{
    if (c < CNFONT_ASCII_FIRST || c >= CNFONT_ASCII_FIRST + CNFONT_ASCII_COUNT)
        return nullptr;
    return cnfont_ascii + (uint32_t)(c - CNFONT_ASCII_FIRST) * CNFONT_ASCII_H;
}

/* ==================== 绘制 ==================== */
/* 画一个 16x16 字模。
 * 不逐像素调 drawPixel —— 那样一个字要 256 次调用, 一屏 80 字就是两万次,
 * 光是函数开销和边界检查就够呛。改成把每行的连续亮点合并成线段,
 * 用 drawFastHLine 一笔画过去; 汉字每行通常只有两三段。 */
static void drawGlyph16(Adafruit_GFX &g, int16_t x, int16_t y,
                        const uint8_t *bits, uint16_t color)
{
    for (int16_t row = 0; row < CN_CHAR_H; row++) {
        const uint16_t line = (uint16_t)((bits[row * 2] << 8) | bits[row * 2 + 1]);

        int16_t runStart = -1;
        for (int16_t col = 0; col <= CN_CHAR_W; col++) {
            const bool on = (col < CN_CHAR_W) && (line & (0x8000 >> col));

            if (on && runStart < 0) {
                runStart = col;
            } else if (!on && runStart >= 0) {
                g.drawFastHLine(x + runStart, y + row, col - runStart, color);
                runStart = -1;
            }
        }
    }
}

/* 画一个 8x16 ASCII 字模(每行 1 字节)。同样按行合并线段。 */
static void drawGlyph8x16(Adafruit_GFX &g, int16_t x, int16_t y,
                          const uint8_t *bits, uint16_t color)
{
    for (int16_t row = 0; row < CNFONT_ASCII_H; row++) {
        const uint8_t line = bits[row];

        int16_t runStart = -1;
        for (int16_t col = 0; col <= CNFONT_ASCII_W; col++) {
            const bool on = (col < CNFONT_ASCII_W) && (line & (0x80 >> col));

            if (on && runStart < 0) {
                runStart = col;
            } else if (!on && runStart >= 0) {
                g.drawFastHLine(x + runStart, y + row, col - runStart, color);
                runStart = -1;
            }
        }
    }
}

static void drawMissing(Adafruit_GFX &g, int16_t x, int16_t y, uint16_t color)
{
    g.drawRect(x + 2, y + 2, CN_CHAR_W - 4, CN_CHAR_H - 4, color);   // 空心方框表示缺字
}

int16_t cnDraw(Adafruit_GFX &g, int16_t x, int16_t y, const char *utf8, uint16_t color)
{
    if (!utf8) return x;

    const char *p = utf8;
    while (true) {
        const uint16_t cp = utf8Next(p);
        if (cp == 0) break;

        if (cp < 0x80) {
            /* ASCII 走 8x16 表 —— 和 16x16 的汉字同高, 混排时基线齐。
               (以前用 Adafruit 内置的 6x8, 数字会比汉字矮一截, 看着别扭) */
            const uint8_t *a = cnAsciiGlyph((uint8_t)cp);
            if (a) drawGlyph8x16(g, x, y, a, color);
            else   drawMissing(g, x, y, color);
            x += CNFONT_ASCII_W;
            continue;
        }

        const uint8_t *bits = cnGlyph(cp);
        if (bits) drawGlyph16(g, x, y, bits, color);
        else      drawMissing(g, x, y, color);
        x += CN_CHAR_W;
    }
    return x;
}

void cnDrawIndexed(Adafruit_GFX &g, int16_t x, int16_t y, uint32_t idx, uint16_t color)
{
    if (idx >= CNFONT_COUNT) return;

    const uint8_t *bits = cnfont_data + idx * CN_REC + 2;
    if (bits) drawGlyph16(g, x, y, bits, color);
}

int16_t cnWidth(const char *utf8)
{
    if (!utf8) return 0;

    int16_t w = 0;
    const char *p = utf8;
    while (true) {
        const uint16_t cp = utf8Next(p);
        if (cp == 0) break;
        w += (cp < 0x80) ? CNFONT_ASCII_W : CN_CHAR_W;
    }
    return w;
}

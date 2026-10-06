#!/usr/bin/env python3
"""
生成 16x16 中文点阵字库, 给 ESP32 用。

用法:
    python tools/gen_cnfont.py

产物:
    src/cnfont_data.h   — C 数组(直接编进固件)
    tools/font_preview.png — 预览图, 用来肉眼检查点阵质量

为什么按 Unicode 码点排序, 而不是按 GB2312 区位码:
    按 GB2312 区位码索引确实是 O(1), 但 ESP32 上的字符串本来就是 UTF-8,
    那就得再带一张 UTF-8→GB2312 的转换表(约 7000 项, 14KB)外加转换代码。
    改成按码点升序存 + 二分查找, 这张表就完全不需要了 ——
    O(log n) 约 13 次比较, 渲染几十个字时完全可以忽略。
    遍历时仍然走 GB2312 字符集, 因为它正好覆盖常用汉字。

每个字 34 字节:
    [码点 2 字节, 小端] [点阵 32 字节]
点阵 32 字节 = 16 行 x 每行 2 字节, 高位在左, 1 = 亮点
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "C:/Windows/Fonts/simhei.ttf"   # 黑体, 点阵化后笔画清晰
SIZE      = 16

# ASCII 单独一套 8x16 —— 英文本来就窄, 塞进 16 宽会又大又空;
# 8x16 是经典搭配(DOS/VGA 时代就是它), 和 16x16 的汉字混排正合适。
# 8 像素宽对普通中文字体太挤, 必须用等宽字体; Consolas 就是为小字号设计的。
ASCII_FONT_PATH = "C:/Windows/Fonts/consola.ttf"
ASCII_W, ASCII_H = 8, 16

HERE      = os.path.dirname(os.path.abspath(__file__))
ROOT      = os.path.dirname(HERE)
OUT_H     = os.path.join(ROOT, "src", "cnfont_data.h")
OUT_PNG   = os.path.join(HERE, "font_preview.png")

PREVIEW_PER_SHEET = 1400  # 每张预览图放多少字(20 列 x 70 行); 全字库分几张
PREVIEW_SCALE     = 2     # 放大倍数


def collect_chars():
    """遍历 GB2312 双字节区, 收集所有可解码的字符"""
    chars = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def rasterize(ch, font):
    """把一个字渲染成 16x16 点阵(32 字节)"""
    img = Image.new("1", (SIZE, SIZE), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=1)

    out = bytearray()
    for y in range(SIZE):
        for xb in range(0, SIZE, 8):
            b = 0
            for bit in range(8):
                b = (b << 1) | (1 if img.getpixel((xb + bit, y)) else 0)
            out.append(b)
    return bytes(out)


def rasterize_ascii(ch, font):
    """ASCII 字符 -> 8x16 点阵(16 字节, 每行 1 字节, 高位在左)"""
    img = Image.new("1", (ASCII_W, ASCII_H), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=1)

    out = bytearray()
    for y in range(ASCII_H):
        b = 0
        for x in range(ASCII_W):
            b = (b << 1) | (1 if img.getpixel((x, y)) else 0)
        out.append(b)
    return bytes(out)


def make_ascii_table():
    """生成 0x20~0x7E 共 95 个可打印 ASCII 的 8x16 点阵"""
    font = ImageFont.truetype(ASCII_FONT_PATH, ASCII_H - 2)   # 留点上下间距
    rows = []
    for code in range(0x20, 0x7F):
        rows.append(rasterize_ascii(chr(code), font))
    return rows


def save_ascii_preview(rows, path):
    """把 ASCII 表画成预览图, 用来确认 8 像素宽下没糊"""
    scale, cols = 4, 16
    n    = len(rows)
    r    = (n + cols - 1) // cols
    cell_w, cell_h = ASCII_W * scale, ASCII_H * scale
    pad  = 2
    pv   = Image.new("RGB", (cols * (cell_w + pad) + pad,
                             r * (cell_h + pad) + pad), (255, 255, 255))
    d    = ImageDraw.Draw(pv)
    for i, bits in enumerate(rows):
        cx = pad + (i % cols) * (cell_w + pad)
        cy = pad + (i // cols) * (cell_h + pad)
        for y in range(ASCII_H):
            for x in range(ASCII_W):
                if bits[y] & (0x80 >> x):
                    d.rectangle([cx + x * scale, cy + y * scale,
                                 cx + (x + 1) * scale - 1,
                                 cy + (y + 1) * scale - 1], fill=(0, 0, 0))
        d.text((cx, cy), f"{i + 0x20:02X}", fill=(200, 200, 200))
    pv.save(path)


def save_mixed_preview(hanzi_map, ascii_rows, path):
    """混排示例: 汉字 16 宽 + ASCII 8 宽。
    这是实际用起来最该看的一张 —— 单字表看不出基线齐不齐、间距挤不挤。"""
    lines = [
        "阿莫西林: 一天2次",
        "距上次 08:30 已 4 小时",
        "该吃了, 别重复!",
        "GLM-4.6V API 响应 200 OK",
    ]
    scale = 4
    pad   = 8
    line_h = SIZE * scale

    # 先算最宽一行
    def width_of(s):
        w = 0
        for ch in s:
            cp = ord(ch)
            w += ASCII_W * scale if cp < 0x80 else SIZE * scale
        return w

    W = max(width_of(s) for s in lines) + pad * 2
    H = len(lines) * line_h + pad * 2 + (len(lines) - 1) * 6
    pv = Image.new("RGB", (W, H), (255, 255, 255))
    d  = ImageDraw.Draw(pv)

    y = pad
    for s in lines:
        x = pad
        for ch in s:
            cp = ord(ch)
            if cp < 0x80:
                idx = cp - 0x20
                if 0 <= idx < len(ascii_rows):
                    bits, gw, gh = ascii_rows[idx], ASCII_W, ASCII_H
                    for r in range(gh):
                        for c in range(gw):
                            if bits[r] & (0x80 >> c):
                                d.rectangle([x + c*scale, y + r*scale,
                                             x + (c+1)*scale - 1, y + (r+1)*scale - 1],
                                            fill=(0, 0, 0))
                x += ASCII_W * scale
            else:
                bits = hanzi_map.get(cp)
                if bits:
                    for r in range(SIZE):
                        row = (bits[r*2] << 8) | bits[r*2+1]
                        for c in range(SIZE):
                            if row & (0x8000 >> c):
                                d.rectangle([x + c*scale, y + r*scale,
                                             x + (c+1)*scale - 1, y + (r+1)*scale - 1],
                                            fill=(0, 0, 0))
                x += SIZE * scale
        y += line_h + 6

    pv.save(path)


def main():
    font = ImageFont.truetype(FONT_PATH, SIZE)
    chars = collect_chars()

    # 按码点升序 —— 二分查找的前提
    items = []
    for ch in sorted(chars, key=ord):
        cp = ord(ch)
        if cp > 0xFFFF:
            continue
        items.append((cp, rasterize(ch, font)))

    # ---- 预览图: 全字库分片输出, 每片一张 PNG ----
    # 全画进一张会有上万像素高, 谁也看不清; 按 1400 字切片, 每片约 640x2400。
    cols  = 20
    pad   = 2
    cell  = SIZE * PREVIEW_SCALE
    sheet_w = cols * (cell + pad) + pad
    total_sheets = (len(items) + PREVIEW_PER_SHEET - 1) // PREVIEW_PER_SHEET

    for sheet in range(total_sheets):
        chunk = items[sheet * PREVIEW_PER_SHEET:(sheet + 1) * PREVIEW_PER_SHEET]
        rows  = (len(chunk) + cols - 1) // cols
        pv = Image.new("RGB", (sheet_w, rows * (cell + pad) + pad), (255, 255, 255))
        d  = ImageDraw.Draw(pv)

        for i, (cp, bits) in enumerate(chunk):
            cx = pad + (i % cols) * (cell + pad)
            cy = pad + (i // cols) * (cell + pad)
            for y in range(SIZE):
                for x in range(SIZE):
                    if bits[y * 2 + (x >> 3)] & (0x80 >> (x & 7)):
                        d.rectangle([cx + x * PREVIEW_SCALE, cy + y * PREVIEW_SCALE,
                                     cx + (x + 1) * PREVIEW_SCALE - 1,
                                     cy + (y + 1) * PREVIEW_SCALE - 1],
                                    fill=(0, 0, 0))
        path = OUT_PNG.replace(".png", f"_{sheet + 1}.png")
        pv.save(path)
        print(f"  预览片 {sheet + 1}/{total_sheets}: {len(chunk)} 字 "
              f"(码点 {chunk[0][0]:04X}~{chunk[-1][0]:04X}) -> {os.path.basename(path)}")

    # ---- C 数组 ----
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("/* 由 tools/gen_cnfont.py 自动生成 —— 不要手改。\n"
                f" *   {len(items)} 个汉字 + 符号, 每字 34 字节(2 码点 + 32 点阵)\n"
                f" *   按 Unicode 码点升序, 供 cnfont.cpp 二分查找。 */\n\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"#define CNFONT_COUNT {len(items)}\n\n")
        f.write("static const uint8_t cnfont_data[] = {\n")
        line = "    "
        for cp, bits in items:
            for b in (cp & 0xFF, cp >> 8):
                line += f"0x{b:02X},"
            for b in bits:
                line += f"0x{b:02X},"
            if len(line) > 100:
                f.write(line + "\n")
                line = "    "
        if line.strip():
            f.write(line + "\n")
        f.write("};\n")

        # ---- ASCII 8x16 表 ----
        # 单独成表, 不混进上面那个: 它按码点直接索引(c - 0x20)就是 O(1),
        # 根本不需要二分, 混进去反而要多存 2 字节码点。
        ascii_rows = make_ascii_table()
        save_ascii_preview(ascii_rows, os.path.join(HERE, "ascii_preview.png"))
        save_mixed_preview(dict(items), ascii_rows,
                           os.path.join(HERE, "mixed_preview.png"))

        f.write("\n/* ASCII 0x20~0x7E 的 8x16 点阵, 每字符 16 字节(每行 1 字节, 高位在左)。\n"
                " * 宽 8 而不是 16 —— 英文本来就窄, 塞进 16 宽的格子又大又空。 */\n")
        f.write("#define CNFONT_ASCII_W     8\n")
        f.write("#define CNFONT_ASCII_H     16\n")
        f.write("#define CNFONT_ASCII_FIRST 0x20\n")
        f.write(f"#define CNFONT_ASCII_COUNT {len(ascii_rows)}\n\n")
        f.write("static const uint8_t cnfont_ascii[] = {\n")
        line = "    "
        for bits in ascii_rows:
            for b in bits:
                line += f"0x{b:02X},"
            if len(line) > 100:
                f.write(line + "\n")
                line = "    "
        if line.strip():
            f.write(line + "\n")
        f.write("};\n")

    total = len(items) * 34
    print(f"汉字:  {len(items)} 字, {total} 字节 ({total/1024:.0f} KB)")
    print(f"ASCII: {len(ascii_rows)} 字符, {len(ascii_rows)*16} 字节 (8x16)")
    print(f"  -> {OUT_H}")
    print(f"  -> tools/font_preview_1..{total_sheets}.png  (汉字分片预览)")
    print(f"  -> tools/ascii_preview.png                  (ASCII 8x16 预览)")


if __name__ == "__main__":
    main()

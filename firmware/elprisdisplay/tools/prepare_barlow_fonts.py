#!/usr/bin/env python3
"""
Hämtar Barlow/Barlow Condensed från Google Fonts och genererar BarlowFonts.h
för Adafruit_GFX/GxEPD2.

Kräver Python 3 + Pillow:
    python3 -m pip install --user pillow

Kör från projektmappen:
    python3 tools/prepare_barlow_fonts.py

TTF-filerna används endast som byggunderlag lokalt. Scriptet skriver en C-header
med de bitmapfonter som sketchen använder.
"""

from pathlib import Path
from urllib.request import urlretrieve
import sys

try:
    from PIL import Image, ImageFont
except ImportError:
    sys.exit("Pillow saknas. Installera med: python3 -m pip install --user pillow")

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / ".font-cache"
OUT = ROOT / "BarlowFonts.h"
CACHE.mkdir(exist_ok=True)

FONT_URLS = {
    "Barlow-SemiBold.ttf":
        "https://raw.githubusercontent.com/google/fonts/main/ofl/barlow/Barlow-SemiBold.ttf",
    "Barlow-Bold.ttf":
        "https://raw.githubusercontent.com/google/fonts/main/ofl/barlow/Barlow-Bold.ttf",
    "BarlowCondensed-SemiBold.ttf":
        "https://raw.githubusercontent.com/google/fonts/main/ofl/barlowcondensed/BarlowCondensed-SemiBold.ttf",
}

FONTS = [
    ("BarlowSemiBold24", "Barlow-SemiBold.ttf", 24),
    ("BarlowBold20", "Barlow-Bold.ttf", 20),
    ("BarlowSemiBold40", "Barlow-SemiBold.ttf", 40),
    ("BarlowSemiBold18", "Barlow-SemiBold.ttf", 18),
    ("BarlowSemiBold20", "Barlow-SemiBold.ttf", 20),
    ("BarlowSemiBold22", "Barlow-SemiBold.ttf", 22),
    ("BarlowCondensedSemiBold20", "BarlowCondensed-SemiBold.ttf", 20),
    ("BarlowCondensedSemiBold22", "BarlowCondensed-SemiBold.ttf", 22),
    ("BarlowBold16", "Barlow-Bold.ttf", 16),
]

FIRST = 32
LAST = 255


def fetch_fonts():
    for name, url in FONT_URLS.items():
        path = CACHE / name
        if not path.exists():
            print(f"Hämtar {name} ...")
            urlretrieve(url, path)


def pack_bitmap(mask):
    width, height = mask.size
    pixels = mask.load()
    out = bytearray()
    byte = 0
    bit = 0

    for y in range(height):
        for x in range(width):
            byte <<= 1
            if pixels[x, y] > 0:
                byte |= 1
            bit += 1
            if bit == 8:
                out.append(byte)
                byte = 0
                bit = 0

    if bit:
        byte <<= (8 - bit)
        out.append(byte)

    return bytes(out)


def glyph_for(font, code):
    # Latin-1 controlområdet ska vara tomt.
    if 127 <= code <= 159:
        return b"", (0, 0, 0, 0, 0, 0)

    ch = chr(code)

    try:
        bbox = font.getbbox(ch, anchor="ls")
    except TypeError:
        bbox = font.getbbox(ch)

    if bbox is None:
        advance = max(1, round(font.getlength(ch)))
        return b"", (0, 0, advance, 0, 0, 0)

    x0, y0, x1, y1 = bbox
    width = max(0, x1 - x0)
    height = max(0, y1 - y0)
    advance = max(1, round(font.getlength(ch)))

    if width == 0 or height == 0:
        return b"", (0, 0, advance, x0, y0, 0)

    img = Image.new("L", (width, height), 0)
    draw_font = font

    # Rendera glyphen så att dess bbox börjar i (0,0).
    from PIL import ImageDraw
    d = ImageDraw.Draw(img)
    d.text((-x0, -y0), ch, font=draw_font, fill=255, anchor="ls")

    bitmap = pack_bitmap(img)
    return bitmap, (width, height, advance, x0, y0, 0)


def make_font(name, ttf_path, px):
    font = ImageFont.truetype(str(ttf_path), px)
    ascent, descent = font.getmetrics()
    y_advance = ascent + descent

    bitmaps = bytearray()
    glyphs = []

    for code in range(FIRST, LAST + 1):
        bitmap, metrics = glyph_for(font, code)
        width, height, advance, xoff, yoff, _ = metrics
        offset = len(bitmaps)

        if offset > 65535:
            raise RuntimeError(
                f"{name}: bitmapOffset överstiger uint16_t ({offset})"
            )

        bitmaps.extend(bitmap)
        glyphs.append((offset, width, height, advance, xoff, yoff))

    return bytes(bitmaps), glyphs, y_advance


def bytes_c(data, indent="  ", per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + ", ".join(f"0x{x:02X}" for x in chunk) + ",")
    return "\n".join(lines)


def write_header():
    parts = [
        "#pragma once",
        "#include <Adafruit_GFX.h>",
        "",
        "// Auto-genererad av tools/prepare_barlow_fonts.py",
        "// Källfamilj: Barlow / Barlow Condensed (SIL OFL 1.1).",
        "",
    ]

    for name, filename, px in FONTS:
        ttf = CACHE / filename
        bitmap, glyphs, y_advance = make_font(name, ttf, px)

        parts.append(f"const uint8_t {name}Bitmaps[] PROGMEM = {{")
        parts.append(bytes_c(bitmap))
        parts.append("};")
        parts.append("")

        parts.append(f"const GFXglyph {name}Glyphs[] PROGMEM = {{")
        for offset, w, h, adv, xoff, yoff in glyphs:
            parts.append(
                f"  {{ {offset:5d}, {w:3d}, {h:3d}, {adv:3d}, "
                f"{xoff:4d}, {yoff:4d} }},"
            )
        parts.append("};")
        parts.append("")

        parts.append(
            f"const GFXfont {name} PROGMEM = {{ "
            f"(uint8_t*){name}Bitmaps, "
            f"(GFXglyph*){name}Glyphs, "
            f"0x{FIRST:02X}, 0x{LAST:02X}, {y_advance} }};"
        )
        parts.append("")

    OUT.write_text("\n".join(parts), encoding="utf-8")
    print(f"Skapade {OUT}")


if __name__ == "__main__":
    fetch_fonts()
    write_header()

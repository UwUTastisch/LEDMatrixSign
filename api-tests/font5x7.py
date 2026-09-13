import re
from pathlib import Path

HEADER = Path("src/gfx/font5x7.h")


def parse_font_header(path=HEADER):
    text = path.read_text()
    # find FIRST, LAST, W, H
    first_m = re.search(r"FONT5x7_FIRST\s*=\s*0x([0-9A-Fa-f]+)", text)
    last_m = re.search(r"FONT5x7_LAST\s*=\s*0x([0-9A-Fa-f]+)", text)
    w_m = re.search(r"FONT5x7_W\s*=\s*([0-9]+)", text)
    h_m = re.search(r"FONT5x7_H\s*=\s*([0-9]+)", text)
    first = int(first_m.group(1), 16) if first_m else 0x20
    last = int(last_m.group(1), 16) if last_m else 0x7E
    w = int(w_m.group(1)) if w_m else 5
    h = int(h_m.group(1)) if h_m else 7

    # extract the FONT5x7 array contents between braces
    array_m = re.search(r"FONT5x7\s*\[\s*\]\s*\[\s*\d+\s*\]\s*=\s*\{([\s\S]*?)\n\};", text)
    glyphs = []
    if array_m:
        body = array_m.group(1)
        # Find only entries that contain exactly five hex bytes (e.g. {0x00, 0x00, 0x00, 0x00, 0x00})
        matches = re.findall(r"\{\s*(0x[0-9A-Fa-f]+(?:\s*,\s*0x[0-9A-Fa-f]+){4})\s*\}", body)
        for m in matches:
            nums = [int(x.strip(), 16) for x in m.split(",")]
            glyphs.append(nums)

    # build map char -> list of column bytes
    font = {}
    code = first
    for glyph in glyphs:
        if code > last:
            break
        ch = chr(code)
        font[ch] = glyph
        code += 1

    return {
        "first": first,
        "last": last,
        "w": w,
        "h": h,
        "glyphs": font,
    }


FONT5x7 = parse_font_header()

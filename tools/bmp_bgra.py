#!/usr/bin/env python3
"""
bmp_bgra.py — dependency-free BMP reader/writer for the LED Matrix Sign 2.0
asset format.

The 2.0 firmware (`src/gfx/asset.h`) expects assets as **32-bit BGRA8888
uncompressed BMP**:
  * BITMAPINFOHEADER (40 bytes), bpp = 32, compression = 0 (BI_RGB)
  * pixel rows bottom-up (positive height), 4 bytes/pixel in B,G,R,A order

This module reads the common BMP variants the 1.0 firmware produced
(24-bit BI_RGB, 32-bit BI_RGB / BI_BITFIELDS) and re-emits them in the exact
byte layout the loader wants. No third-party dependencies, so it runs anywhere
Python 3.8+ is installed.

Usable as a library (read_bmp / write_bgra8888_bmp / convert_file) or a CLI:

    python3 bmp_bgra.py in.bmp out.bmp
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from typing import List


@dataclass
class Image:
    """An RGBA image held as a flat top-down bytearray, 4 bytes/px (R,G,B,A)."""
    width: int
    height: int
    rgba: bytearray  # top-down, row-major, 4 bytes per pixel (R,G,B,A)

    def pixel(self, x: int, y: int):
        i = (y * self.width + x) * 4
        return tuple(self.rgba[i:i + 4])


def read_bmp(path: str) -> Image:
    """Read a 24- or 32-bit uncompressed BMP into a top-down RGBA Image."""
    with open(path, "rb") as fh:
        data = fh.read()
    return decode_bmp(data)


def decode_bmp(data: bytes) -> Image:
    if len(data) < 54 or data[0:2] != b"BM":
        raise ValueError("not a BMP file (missing 'BM' signature)")

    data_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError(f"unsupported DIB header size {dib_size} (need >= 40)")

    width = struct.unpack_from("<i", data, 18)[0]
    height_raw = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]

    if bpp not in (24, 32):
        raise ValueError(f"unsupported bit depth {bpp} (need 24 or 32)")
    # BI_RGB = 0, BI_BITFIELDS = 3. We accept 3 only for 32bpp and assume BGRA.
    if compression not in (0, 3):
        raise ValueError(f"unsupported compression {compression} (need 0 or 3)")

    height = abs(height_raw)
    bottom_up = height_raw > 0
    bytes_pp = bpp // 8
    # BMP rows are padded to a 4-byte boundary.
    row_size = ((bpp * width + 31) // 32) * 4

    rgba = bytearray(width * height * 4)
    for src_y in range(height):
        dst_y = (height - 1 - src_y) if bottom_up else src_y
        row_start = data_offset + src_y * row_size
        for x in range(width):
            p = row_start + x * bytes_pp
            b = data[p]
            g = data[p + 1]
            r = data[p + 2]
            a = data[p + 3] if bytes_pp == 4 else 255
            # 32-bit BI_RGB files often leave the alpha byte 0; treat a fully
            # zero alpha channel as opaque so legacy images stay visible.
            o = (dst_y * width + x) * 4
            rgba[o] = r
            rgba[o + 1] = g
            rgba[o + 2] = b
            rgba[o + 3] = a

    img = Image(width, height, rgba)
    if bytes_pp == 4 and _alpha_all_zero(img):
        _force_opaque(img)
    return img


def _alpha_all_zero(img: Image) -> bool:
    return all(img.rgba[i] == 0 for i in range(3, len(img.rgba), 4))


def _force_opaque(img: Image) -> None:
    for i in range(3, len(img.rgba), 4):
        img.rgba[i] = 255


def write_bgra8888_bmp(img: Image, path: str) -> None:
    """Write an Image as a 32-bit BGRA8888 BI_RGB BMP (bottom-up)."""
    w, h = img.width, img.height
    row_size = w * 4  # 32bpp is inherently 4-byte aligned
    pixel_bytes = row_size * h
    data_offset = 14 + 40
    file_size = data_offset + pixel_bytes

    out = bytearray()
    # BITMAPFILEHEADER (14 bytes)
    out += b"BM"
    out += struct.pack("<I", file_size)
    out += struct.pack("<HH", 0, 0)          # reserved
    out += struct.pack("<I", data_offset)
    # BITMAPINFOHEADER (40 bytes)
    out += struct.pack("<I", 40)             # header size
    out += struct.pack("<i", w)
    out += struct.pack("<i", h)              # positive -> bottom-up
    out += struct.pack("<H", 1)              # planes
    out += struct.pack("<H", 32)             # bpp
    out += struct.pack("<I", 0)              # BI_RGB
    out += struct.pack("<I", pixel_bytes)    # image size
    out += struct.pack("<i", 2835)           # x ppm (~72 dpi)
    out += struct.pack("<i", 2835)           # y ppm
    out += struct.pack("<I", 0)              # colors used
    out += struct.pack("<I", 0)              # important colors

    # Pixel data, bottom-up, B,G,R,A
    for y in range(h - 1, -1, -1):
        base = y * w * 4
        for x in range(w):
            o = base + x * 4
            r = img.rgba[o]
            g = img.rgba[o + 1]
            b = img.rgba[o + 2]
            a = img.rgba[o + 3]
            out += bytes((b, g, r, a))

    with open(path, "wb") as fh:
        fh.write(out)


def convert_file(src: str, dst: str) -> Image:
    """Read any supported BMP at `src` and write a BGRA8888 BMP to `dst`."""
    img = read_bmp(src)
    write_bgra8888_bmp(img, dst)
    return img


def _main(argv: List[str]) -> int:
    if len(argv) != 3:
        print("usage: bmp_bgra.py <in.bmp> <out.bmp>", file=sys.stderr)
        return 2
    img = convert_file(argv[1], argv[2])
    print(f"converted {argv[1]} -> {argv[2]} ({img.width}x{img.height}, 32-bit BGRA8888)")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv))

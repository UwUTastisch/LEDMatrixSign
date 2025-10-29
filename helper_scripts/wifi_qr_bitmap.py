#!/usr/bin/env python3
"""
wifi_qr_bitmap.py

Generate a Wi‑Fi QR code saved as a BMP where each QR module is exactly one pixel.
You can specify the final bitmap matrix size (width x height in pixels). The generated
QR (which is square) will be centered inside that matrix; background is black and
QR modules are white by default (use --invert to swap).

Dependencies:
    pip install qrcode[pil] pillow

Usage examples:
    # smallest possible (one pixel per QR module) and auto-fit matrix:
    python wifi_qr_bitmap.py --ssid "MyWiFi" --password "secret" --auth WPA --modules 0 --out wifi.bmp

    # force output matrix 200x200 pixels (QR will be centered; modules remain 1px):
    python wifi_qr_bitmap.py --ssid MyWiFi --password secret --auth WPA --matrix 200 200 --out wifi.bmp

    # invert: QR black on white background
    python wifi_qr_bitmap.py --ssid MyWiFi --password secret --auth WPA --matrix 128 128 --invert --out wifi.bmp

Notes:
  - Each QR module is exactly one pixel in the output.
  - If the QR's module size (determined by needed version) is larger than the provided
    matrix, the script will exit with an error unless you increase the matrix size.
"""

import argparse
import sys
from math import floor

try:
    import qrcode
except ImportError:
    print("Error: missing dependency 'qrcode'. Install with: pip install qrcode[pil]")
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    print("Error: missing dependency 'Pillow'. Install with: pip install pillow")
    sys.exit(1)


def build_wifi_payload(ssid: str, auth: str, password: str, hidden: bool) -> str:
    """Build the standard WIFI: QR payload.
    Format: WIFI:T:WPA;S:SSID;P:password;H:true;;
    """
    auth = auth.upper() if auth else "NOPASS"
    hidden_part = "H:true;" if hidden else ""
    # Escape semicolons/backslashes or commas if present in SSID/password
    esc = lambda s: s.replace('\\', '\\\\').replace(';', '\\;').replace(',', '\\,')
    return f"WIFI:T:{auth};S:{esc(ssid)};P:{esc(password)};{hidden_part};"


def generate_qr_matrix(payload: str, error_correction=qrcode.constants.ERROR_CORRECT_M):
    """Return a 2D list (matrix) of booleans for the QR modules (True = dark module).
    This uses qrcode.QRCode with version=None (auto) to choose a version that fits the payload.
    """
    qr = qrcode.QRCode(version=None, error_correction=error_correction, box_size=1, border=4)
    qr.add_data(payload)
    qr.make(fit=True)
    matrix = qr.get_matrix()  # list of lists of booleans
    return matrix


def matrix_to_bitmap(matrix, out_width, out_height, invert=False, dark=(255,255,255), background=(0,0,0)):
    """Create a PIL Image (RGB) of size out_width x out_height where each matrix cell is one pixel.
    The QR matrix is centered in the target bitmap. If matrix is larger than target, raise ValueError.
    invert=True -> dark modules become black and background white (swap colors).
    """
    m_h = len(matrix)
    m_w = len(matrix[0]) if m_h>0 else 0
    if m_w != m_h:
        # defensive, but QR matrices are always square
        pass

    if m_w > out_width or m_h > out_height:
        raise ValueError(f"Target matrix too small: QR is {m_w}x{m_h} modules but target is {out_width}x{out_height}.")

    # choose colors
    if invert:
        dark_color = background
        bg_color = dark
    else:
        dark_color = dark
        bg_color = background

    img = Image.new('RGB', (out_width, out_height), bg_color)
    px = img.load()

    # compute top-left offset to center QR
    off_x = floor((out_width - m_w) / 2)
    off_y = floor((out_height - m_h) / 2)

    for y in range(m_h):
        for x in range(m_w):
            if matrix[y][x]:
                img.putpixel((off_x + x, off_y + y), dark_color)
    return img


def main():
    p = argparse.ArgumentParser(description="Generate a Wi-Fi QR BMP with 1px-per-module and black background.")
    p.add_argument("--ssid", required=True, help="Wi‑Fi SSID")
    p.add_argument("--password", default='', help="Wi‑Fi password (empty for open networks)")
    p.add_argument("--auth", choices=["WEP","WPA","WPA2","WPA3","NOPASS"], default='WPA', help="Auth type")
    p.add_argument("--hidden", action='store_true', help="Mark network hidden (H:true) in payload")
    p.add_argument("--matrix", nargs=2, type=int, metavar=("WIDTH","HEIGHT"), help="Output bitmap matrix size in pixels (width height). Each QR spot = 1 pixel. If omitted, will use the QR's native module size + 2*border (so the image will be square).", default=None)
    p.add_argument("--modules", type=int, default=0, help="Deprecated alias: set --matrix N N by passing a single modules value. If >0 it forces output to modules x modules.")
    p.add_argument("--invert", action='store_true', help="Invert colors: QR dark modules -> black, background -> white.")
    p.add_argument("--out", "-o", default='wifi_qr.bmp', help="Output BMP filename")
    args = p.parse_args()

    payload = build_wifi_payload(args.ssid, args.auth, args.password, args.hidden)

    matrix = generate_qr_matrix(payload)
    qr_size = len(matrix)

    if args.modules and args.modules > 0:
        out_w = out_h = args.modules
    elif args.matrix:
        out_w, out_h = args.matrix
    else:
        # default: make an image exactly the size of the QR matrix (plus border already included in matrix)
        out_w = out_h = qr_size

    # Ensure integer, positive
    if out_w <= 0 or out_h <= 0:
        print("Error: output matrix dimensions must be positive integers")
        sys.exit(1)

    try:
        img = matrix_to_bitmap(matrix, out_w, out_h, invert=args.invert, dark=(255,255,255), background=(0,0,0))
    except ValueError as e:
        print("Error:", e)
        print(f"QR requires {qr_size}x{qr_size} modules. Increase --matrix to at least {qr_size} {qr_size}.")
        sys.exit(2)

    # Save as BMP (bitmap). Pillow will produce a BMP file.
    img.save(args.out, format='BMP')
    print(f"Saved {args.out} ({out_w}x{out_h}) — QR size: {qr_size}x{qr_size} modules. Background: black. Modules: white.")


if __name__ == '__main__':
    main()

from PIL import Image, ImageDraw, ImageFont
import sys

# Usage:
#   python sign_generator.py "FRESH" "LOCAL" "HONEY"
#   python sign_generator.py -c 255,200,0 "FRESH" "LOCAL" "HONEY"
#   python sign_generator.py -d custom_name -c 0,255,128 "OPEN" "24" "HRS"
# Supports 1 to 3 lines.
# -c R,G,B   font color (default: 255,255,255 = white)
# -d NAME    output filename without extension

# ── parse args ────────────────────────────────────────────────────────────────

args = sys.argv[1:]

custom_name = None
font_color  = (255, 255, 255)   # default: white text on black bg

def pop_flag(args, flag, n_values=1):
    if flag not in args:
        return None, args
    i = args.index(flag)
    if i + n_values >= len(args):
        print(f"Error: {flag} requires {n_values} argument(s)")
        sys.exit(1)
    values = args[i + 1 : i + 1 + n_values]
    return values, args[:i] + args[i + 1 + n_values:]

d_vals, args = pop_flag(args, "-d")
if d_vals:
    custom_name = d_vals[0]

c_vals, args = pop_flag(args, "-c")
if c_vals:
    try:
        parts = c_vals[0].split(",")
        assert len(parts) == 3
        font_color = tuple(int(p.strip()) for p in parts)
    except Exception:
        print("Error: -c expects R,G,B  e.g.  -c 255,200,0")
        sys.exit(1)

if len(args) < 1 or len(args) > 3:
    print('Usage: sign_generator.py [-c R,G,B] [-d name] "LINE1" ["LINE2"] ["LINE3"]')
    sys.exit(1)

lines = args

# ── canvas ────────────────────────────────────────────────────────────────────

WIDTH   = 48
HEIGHT  = 48
PADDING = 2

BG_COLOR   = (0, 0, 0)
TEXT_COLOR = font_color

# ── pixel-perfect font loading ────────────────────────────────────────────────
# Render in 1-bit mode ('1') — PIL draws ZERO antialiasing in this mode.
# Every pixel is exactly 0 or 1. Then we colorize via a mask paste onto RGB.

FONT_PATHS = [
    "/usr/share/fonts/opentype/unifont/unifont.otf",   # pixel-grid designed
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
    "DejaVuSansMono-Bold.ttf",
    "LiberationMono-Bold.ttf",
]

def load_font(size):
    for path in FONT_PATHS:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            pass
    return ImageFont.load_default()

def measure(lines, font):
    scratch = ImageDraw.Draw(Image.new("1", (512, 128), 0))
    return [
        (lambda b: (b[2] - b[0], b[3] - b[1]))(scratch.textbbox((0, 0), l, font=font))
        for l in lines
    ]

def block_height(sizes, spacing):
    return sum(h for _, h in sizes) + spacing * max(0, len(sizes) - 1)

# ── auto-size: find largest font that fits ────────────────────────────────────

avail_w = WIDTH  - 2 * PADDING
avail_h = HEIGHT - 2 * PADDING

best = {"font": None, "size": 1, "sizes": [], "spacing": 0}

for pt in range(1, HEIGHT + 1):
    font  = load_font(pt)
    sizes = measure(lines, font)
    leftover = avail_h - sum(h for _, h in sizes)
    n_gaps   = max(1, len(lines) - 1)
    spacing  = max(0, min(4, leftover // n_gaps))
    th = block_height(sizes, spacing)
    tw = max(w for w, _ in sizes)
    if th <= avail_h and tw <= avail_w:
        best = {"font": font, "size": pt, "sizes": sizes, "spacing": spacing}
    else:
        break

font    = best["font"] or load_font(1)
sizes   = best["sizes"] or measure(lines, font)
spacing = best["spacing"]

# ── render in 1-bit (no AA), then composite onto RGB ─────────────────────────

mask_img  = Image.new("1", (WIDTH, HEIGHT), 0)
mask_draw = ImageDraw.Draw(mask_img)

th    = block_height(sizes, spacing)
cur_y = (HEIGHT - th) // 2

for i, line in enumerate(lines):
    w, h = sizes[i]
    x = (WIDTH - w) // 2
    mask_draw.text((x, cur_y), line, font=font, fill=1)
    cur_y += h + spacing

rgb = Image.new("RGB", (WIDTH, HEIGHT), BG_COLOR)
rgb.paste(Image.new("RGB", (WIDTH, HEIGHT), TEXT_COLOR), mask=mask_img)

# ── save ─────────────────────────────────────────────────────────────────────

if custom_name:
    output_file = f"{custom_name}.bmp"
else:
    cleaned     = "_".join(l.lower().replace(" ", "_") for l in lines)
    output_file = f"{cleaned}.bmp"

rgb.save(output_file)
print(f"Saved {WIDTH}x{HEIGHT} BMP -> {output_file}  (font {best['size']} pt, color {TEXT_COLOR})")
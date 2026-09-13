#!/usr/bin/env python3
"""Bundle webui/src/ into one self-contained index.html.

    python3 webui/build.py                # -> webui/dist/index.html
    python3 webui/build.py --install      # -> example-sd-card-content/index.html
    python3 webui/build.py -o /media/SD/index.html
    python3 webui/build.py --font src/gfx/font5x7.h   # refresh the glyph table

`--install` overwrites the copy the firmware ships, which `data/` symlinks to,
so `pio run --target uploadfs` picks it up. On an SD card, put the file at the
root. Either way the firmware serves it at http://<sign>/ and the page talks to
the API on the same origin.

No dependencies beyond the Python standard library.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"

LINK_RE = re.compile(r'<link\s+rel="stylesheet"\s+href="([^"]+)"\s*/?>')
SCRIPT_RE = re.compile(r'<script\s+src="([^"]+)"\s*(?:defer)?\s*>\s*</script>')
FONT_ROW_RE = re.compile(r"\{(0x[0-9A-Fa-f]{2}(?:\s*,\s*0x[0-9A-Fa-f]{2}){4})\}")
FONT_JS_RE = re.compile(r"(const FONT_TABLE = \(\(\) => \{\n  const hex = '')((?:\n  \+ '[0-9a-f]*')+)(;)")


def font_hex_from_header(path: Path) -> str:
    rows = FONT_ROW_RE.findall(path.read_text(encoding="utf-8"))
    if len(rows) != 95:
        sys.exit(f"{path}: expected 95 glyphs (0x20..0x7E), found {len(rows)}")
    return "".join(b[2:].lower() for row in rows for b in re.findall(r"0x[0-9A-Fa-f]{2}", row))


def replace_font(js: str, hexstr: str) -> str:
    chunks = "".join(f"\n  + '{hexstr[i:i + 100]}'" for i in range(0, len(hexstr), 100))
    new, n = FONT_JS_RE.subn(lambda m: m.group(1) + chunks + m.group(3), js)
    if n != 1:
        sys.exit("could not find the FONT_TABLE literal in app.js")
    return new


def build(font: Path | None) -> str:
    html = (SRC / "index.html").read_text(encoding="utf-8")

    def inline_css(match: re.Match) -> str:
        css = (SRC / match.group(1)).read_text(encoding="utf-8")
        return "<style>\n" + css.replace("</style", "<\\/style") + "\n</style>"

    def inline_js(match: re.Match) -> str:
        js = (SRC / match.group(1)).read_text(encoding="utf-8")
        if font:
            js = replace_font(js, font_hex_from_header(font))
        # A literal "</script" would end the inline block early.
        return "<script>\n" + js.replace("</script", "<\\/script") + "\n</script>"

    html, n_css = LINK_RE.subn(inline_css, html)
    html, n_js = SCRIPT_RE.subn(inline_js, html)
    if n_css != 1 or n_js != 1:
        sys.exit(f"expected one stylesheet and one script tag in src/index.html (found {n_css}, {n_js})")
    return html


# example-sd-card-content/ in the firmware repo, i.e. the copy the device ships
INSTALL_PATH = ROOT.parent / "example-sd-card-content" / "index.html"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-o", "--output", type=Path,
                        help="where to write (default: webui/dist/index.html)")
    parser.add_argument("--install", action="store_true",
                        help=f"write to {INSTALL_PATH.name} in example-sd-card-content/ instead")
    parser.add_argument("--font", type=Path,
                        help="firmware gfx/font5x7.h to take the glyph table from")
    args = parser.parse_args()

    if args.output and args.install:
        sys.exit("use either --output or --install, not both")
    output = args.output or (INSTALL_PATH if args.install else ROOT / "dist" / "index.html")
    if args.install and not output.parent.is_dir():
        sys.exit(f"{output.parent} not found — run --install from a firmware checkout")

    html = build(args.font)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(html, encoding="utf-8")
    print(f"wrote {output} ({len(html.encode('utf-8')) / 1024:.1f} KB)")


if __name__ == "__main__":
    main()

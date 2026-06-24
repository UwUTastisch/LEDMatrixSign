#!/usr/bin/env python3
"""
gen_font.py — generate src/gfx/font5x7.h from human-readable glyph grids.

Each glyph is 5 wide x 7 tall, authored as 7 rows of 5 characters
('#' = lit, anything else = off). Glyphs are encoded column-major:
5 bytes per glyph, byte i = column i, bit b (0..6) = row b (top..bottom).

This keeps the font readable/reviewable in this file while emitting a
compact, correct C array. Unknown code points render as a hollow box at
runtime (handled in font.h), and lowercase falls back to uppercase.

Run:  python3 tools/gen_font.py
"""
import os

# 7 rows x 5 cols.  '#' = on.  Author uppercase, digits, space, punctuation.
G = {}

def g(ch, *rows):
    assert len(rows) == 7, f"{ch!r} must have 7 rows, got {len(rows)}"
    for r in rows:
        assert len(r) == 5, f"{ch!r} row {r!r} must be 5 wide"
    G[ch] = rows

g(" ",
  "     ","     ","     ","     ","     ","     ","     ")
g("!",
  "  #  ","  #  ","  #  ","  #  ","  #  ","     ","  #  ")
g('"',
  " # # "," # # "," # # ","     ","     ","     ","     ")
g("#",
  " # # "," # # ","#####"," # # ","#####"," # # "," # # ")
g("$",
  "  #  "," ####","# #  "," ### ","  # #","#### ","  #  ")
g("%",
  "##  #","## # ","   # ","  #  "," #   ","# ## ","#  ##")
g("&",
  " ##  ","#  # ","#  # "," ##  ","#  ##","#  # "," ## #")
g("'",
  "  #  ","  #  "," #   ","     ","     ","     ","     ")
g("(",
  "   # ","  #  "," #   "," #   "," #   ","  #  ","   # ")
g(")",
  " #   ","  #  ","   # ","   # ","   # ","  #  "," #   ")
g("*",
  "     ","# # #"," ### ","#####"," ### ","# # #","     ")
g("+",
  "     ","  #  ","  #  ","#####","  #  ","  #  ","     ")
g(",",
  "     ","     ","     ","     ","  #  ","  #  "," #   ")
g("-",
  "     ","     ","     ","#####","     ","     ","     ")
g(".",
  "     ","     ","     ","     ","     ","  #  ","  #  ")
g("/",
  "    #","   # ","  #  ","  #  ","  #  "," #   ","#    ")
g("0",
  " ### ","#   #","#  ##"," # # ","##  #","#   #"," ### ")
g("1",
  "  #  "," ##  ","  #  ","  #  ","  #  ","  #  "," ### ")
g("2",
  " ### ","#   #","    #","   # ","  #  "," #   ","#####")
g("3",
  "#####","   # ","  #  ","   # ","    #","#   #"," ### ")
g("4",
  "   # ","  ## "," # # ","#  # ","#####","   # ","   # ")
g("5",
  "#####","#    ","#### ","    #","    #","#   #"," ### ")
g("6",
  "  ## "," #   ","#    ","#### ","#   #","#   #"," ### ")
g("7",
  "#####","    #","   # ","  #  "," #   "," #   "," #   ")
g("8",
  " ### ","#   #","#   #"," ### ","#   #","#   #"," ### ")
g("9",
  " ### ","#   #","#   #"," ####","    #","   # "," ##  ")
g(":",
  "     ","  #  ","  #  ","     ","  #  ","  #  ","     ")
g(";",
  "     ","  #  ","  #  ","     ","  #  ","  #  "," #   ")
g("<",
  "   # ","  #  "," #   ","#    "," #   ","  #  ","   # ")
g("=",
  "     ","     ","#####","     ","#####","     ","     ")
g(">",
  " #   ","  #  ","   # ","    #","   # ","  #  "," #   ")
g("?",
  " ### ","#   #","    #","   # ","  #  ","     ","  #  ")
g("@",
  " ### ","#   #","# ###","# # #","# ###","#    "," ### ")
g("A",
  " ### ","#   #","#   #","#####","#   #","#   #","#   #")
g("B",
  "#### ","#   #","#   #","#### ","#   #","#   #","#### ")
g("C",
  " ### ","#   #","#    ","#    ","#    ","#   #"," ### ")
g("D",
  "###  ","#  # ","#   #","#   #","#   #","#  # ","###  ")
g("E",
  "#####","#    ","#    ","#### ","#    ","#    ","#####")
g("F",
  "#####","#    ","#    ","#### ","#    ","#    ","#    ")
g("G",
  " ### ","#   #","#    ","# ###","#   #","#   #"," ### ")
g("H",
  "#   #","#   #","#   #","#####","#   #","#   #","#   #")
g("I",
  " ### ","  #  ","  #  ","  #  ","  #  ","  #  "," ### ")
g("J",
  "  ###","   # ","   # ","   # ","#  # ","#  # "," ##  ")
g("K",
  "#   #","#  # ","# #  ","##   ","# #  ","#  # ","#   #")
g("L",
  "#    ","#    ","#    ","#    ","#    ","#    ","#####")
g("M",
  "#   #","## ##","# # #","#   #","#   #","#   #","#   #")
g("N",
  "#   #","##  #","# # #","#  ##","#   #","#   #","#   #")
g("O",
  " ### ","#   #","#   #","#   #","#   #","#   #"," ### ")
g("P",
  "#### ","#   #","#   #","#### ","#    ","#    ","#    ")
g("Q",
  " ### ","#   #","#   #","#   #","# # #","#  # "," ## #")
g("R",
  "#### ","#   #","#   #","#### ","# #  ","#  # ","#   #")
g("S",
  " ### ","#   #","#    "," ### ","    #","#   #"," ### ")
g("T",
  "#####","  #  ","  #  ","  #  ","  #  ","  #  ","  #  ")
g("U",
  "#   #","#   #","#   #","#   #","#   #","#   #"," ### ")
g("V",
  "#   #","#   #","#   #","#   #","#   #"," # # ","  #  ")
g("W",
  "#   #","#   #","#   #","# # #","# # #","## ##","#   #")
g("X",
  "#   #","#   #"," # # ","  #  "," # # ","#   #","#   #")
g("Y",
  "#   #","#   #"," # # ","  #  ","  #  ","  #  ","  #  ")
g("Z",
  "#####","    #","   # ","  #  "," #   ","#    ","#####")
g("[",
  " ### "," #   "," #   "," #   "," #   "," #   "," ### ")
g("\\",
  "#    "," #   "," #   ","  #  ","  #  ","   # ","    #")
g("]",
  " ### ","   # ","   # ","   # ","   # ","   # "," ### ")
g("^",
  "  #  "," # # ","#   #","     ","     ","     ","     ")
g("_",
  "     ","     ","     ","     ","     ","     ","#####")
g("`",
  " #   ","  #  ","   # ","     ","     ","     ","     ")
g("{",
  "  ## ","  #  "," #   ","#    "," #   ","  #  ","  ## ")
g("|",
  "  #  ","  #  ","  #  ","     ","  #  ","  #  ","  #  ")
g("}",
  " ##  ","  #  ","   # ","    #","   # ","  #  "," ##  ")
g("~",
  "     ","     "," #  #","# ## ","     ","     ","     ")

def encode(rows):
    cols = []
    for c in range(5):
        byte = 0
        for r in range(7):
            if rows[r][c] == '#':
                byte |= (1 << r)
        cols.append(byte)
    return cols

FIRST, LAST = 0x20, 0x7E
out = []
out.append("// font5x7.h — AUTO-GENERATED by tools/gen_font.py. Do not edit by hand.")
out.append("// 5x7 column-major glyphs, ASCII 0x20..0x7E (5 bytes/glyph, bit b = row b).")
out.append("#pragma once")
out.append("#include <stdint.h>")
out.append("")
out.append(f"static const uint8_t FONT5x7_FIRST = 0x{FIRST:02X};")
out.append(f"static const uint8_t FONT5x7_LAST  = 0x{LAST:02X};")
out.append("static const uint8_t FONT5x7_W = 5;")
out.append("static const uint8_t FONT5x7_H = 7;")
out.append("")
out.append("static const uint8_t FONT5x7[][5] = {")
missing = []
for code in range(FIRST, LAST + 1):
    ch = chr(code)
    rows = G.get(ch)
    if rows is None and 'a' <= ch <= 'z':
        rows = G.get(ch.upper())            # lowercase → uppercase fallback
    if rows is None:
        rows = ["#####"] + ["#   #"] * 5 + ["#####"]  # hollow box for unknowns
        missing.append(ch)
    cols = encode(rows)
    label = ch if ch not in '\\' else '\\\\'
    out.append("  {%s}, // 0x%02X '%s'" % (
        ", ".join("0x%02X" % b for b in cols), code, label if ch != ' ' else ' '))
out.append("};")
out.append("")

dst = os.path.join(os.path.dirname(__file__), "..", "src", "gfx", "font5x7.h")
dst = os.path.normpath(dst)
with open(dst, "w") as f:
    f.write("\n".join(out) + "\n")
print(f"Wrote {dst}: {LAST-FIRST+1} glyphs ({FIRST:#x}..{LAST:#x})")
if missing:
    print("Rendered as hollow box (not authored):", " ".join(missing))

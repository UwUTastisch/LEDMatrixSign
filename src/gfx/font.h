// font.h — named bitmap-font registry.
//
// The spec notes fonts "should be cached and precompiled" and "will be
// hardcoded later". We ship one real glyph set (5x7, see font5x7.h) and expose
// named fonts that reference it, optionally integer-scaled. "12x16_serif" is
// served as a 2x-scaled 5x7 for now (clearly marked) so layouts that ask for a
// big font still render; swap in a dedicated glyph table later without touching
// callers.
#pragma once
#include <Arduino.h>
#include "font5x7.h"

struct Glyph
{
    const uint8_t *cols; // `scale*5` logical columns derived from base 5
    uint8_t baseW, baseH;
};

class Font
{
public:
    const char *name;
    uint8_t scale;   // integer upscale of the 5x7 base
    uint8_t spacing; // blank columns between glyphs (in scaled px)

    Font(const char *n, uint8_t s, uint8_t sp) : name(n), scale(s), spacing(sp) {}

    uint8_t glyphW() const { return FONT5x7_W * scale; }
    uint8_t glyphH() const { return FONT5x7_H * scale; }
    uint8_t advance() const { return glyphW() + spacing; }

    // total pixel width of a string (no trailing spacing)
    int measure(const String &s) const
    {
        if (s.length() == 0) return 0;
        return s.length() * advance() - spacing;
    }

    // Is logical pixel (gx,gy) inside glyph `ch` lit?  gx,gy in scaled coords.
    bool pixel(char ch, int gx, int gy) const
    {
        if (gx < 0 || gy < 0) return false;
        int bx = gx / scale; // base-font column 0..4
        int by = gy / scale; // base-font row    0..6
        if (bx >= FONT5x7_W || by >= FONT5x7_H) return false;
        uint8_t code = (uint8_t)ch;
        if (code < FONT5x7_FIRST || code > FONT5x7_LAST) code = '?';
        const uint8_t *g = FONT5x7[code - FONT5x7_FIRST];
        return (g[bx] >> by) & 0x1;
    }
};

namespace Fonts
{
    // Registry kept tiny and static (precompiled). Add rows as real fonts land.
    inline const Font &byName(const String &n)
    {
        static const Font f5x7("5x7", 1, 1);
        static const Font f10x14("10x14", 2, 1);     // 2x 5x7
        static const Font f12x16("12x16_serif", 2, 2); // stub: 2x 5x7, wider gap
        if (n == "10x14") return f10x14;
        if (n == "12x16_serif" || n == "12x16") return f12x16;
        return f5x7; // default / unknown name → 5x7
    }
}

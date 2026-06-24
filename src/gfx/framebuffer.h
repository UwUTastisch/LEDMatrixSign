// framebuffer.h — matrix-sized RGBA raster target + drawing primitives.
//
// One FrameBuffer == one logical layer (the FrameFactory keeps two and
// composites them). Pixels are RGBA; alpha 0 means "transparent / nothing
// drawn here" which is what makes overlay compositing work. All object
// rasterisation (text, lines, rects, assets, gradients) lands here.
#pragma once
#include <Arduino.h>
#include <vector>
#include <math.h>
#include "color.h"
#include "font.h"

class FrameBuffer
{
public:
    uint16_t w = 0, h = 0;
    std::vector<Rgba> px;

    void init(uint16_t width, uint16_t height)
    {
        w = width;
        h = height;
        px.assign((size_t)w * h, Rgba(0, 0, 0, 0));
    }

    inline bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    inline Rgba &atRef(int x, int y) { return px[(size_t)y * w + x]; }
    inline const Rgba &atRef(int x, int y) const { return px[(size_t)y * w + x]; }

    // wipe everything to transparent
    void clearAll() { std::fill(px.begin(), px.end(), Rgba(0, 0, 0, 0)); }

    // ——— pixel write with source-over alpha blending ———
    inline void blend(int x, int y, const Rgba &c)
    {
        if (!in(x, y) || c.a == 0) return;
        if (c.a == 255)
        {
            atRef(x, y) = c;
            return;
        }
        Rgba &d = atRef(x, y);
        uint16_t sa = c.a, da = 255 - sa;
        d.r = (uint8_t)((c.r * sa + d.r * da) / 255);
        d.g = (uint8_t)((c.g * sa + d.g * da) / 255);
        d.b = (uint8_t)((c.b * sa + d.b * da) / 255);
        d.a = (uint8_t)(c.a > d.a ? c.a : d.a);
    }

    // ——— primitives ———
    void hline(int x0, int x1, int y, const Rgba &c)
    {
        if (x0 > x1) std::swap(x0, x1);
        for (int x = x0; x <= x1; x++) blend(x, y, c);
    }
    void vline(int x, int y0, int y1, const Rgba &c)
    {
        if (y0 > y1) std::swap(y0, y1);
        for (int y = y0; y <= y1; y++) blend(x, y, c);
    }

    // Bresenham line with `thickness` (square brush), colored by a ColorSpec
    // sampled along its bounding box.
    void line(int x0, int y0, int x1, int y1, int thickness, const ColorSpec &cs)
    {
        int bx = min(x0, x1), by = min(y0, y1);
        int bw = abs(x1 - x0) + 1, bh = abs(y1 - y0) + 1;
        int t = max(1, thickness);
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        int x = x0, y = y0;
        for (;;)
        {
            for (int oy = 0; oy < t; oy++)
                for (int ox = 0; ox < t; ox++)
                {
                    int px_ = x + ox, py_ = y + oy;
                    float u = bw > 1 ? (float)(px_ - bx) / (bw - 1) : 0;
                    float v = bh > 1 ? (float)(py_ - by) / (bh - 1) : 0;
                    blend(px_, py_, cs.at(u, v));
                }
            if (x == x1 && y == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
    }

    // Rectangle from (x,y), size dx*dy. border<=0 → fill, else outline of that
    // thickness. Colored via ColorSpec across the rect's box.
    void rect(int x, int y, int dx, int dy, int border, const ColorSpec &cs)
    {
        if (dx <= 0 || dy <= 0) return;
        for (int j = 0; j < dy; j++)
            for (int i = 0; i < dx; i++)
            {
                bool edge = (i < border) || (j < border) ||
                            (i >= dx - border) || (j >= dy - border);
                if (border > 0 && !edge) continue;
                float u = dx > 1 ? (float)i / (dx - 1) : 0;
                float v = dy > 1 ? (float)j / (dy - 1) : 0;
                blend(x + i, y + j, cs.at(u, v));
            }
    }

    // Draw one glyph at (x,y). `cs` sampled across the glyph box.
    void glyph(const Font &font, char ch, int x, int y, const ColorSpec &cs)
    {
        int gw = font.glyphW(), gh = font.glyphH();
        for (int gy = 0; gy < gh; gy++)
            for (int gx = 0; gx < gw; gx++)
                if (font.pixel(ch, gx, gy))
                {
                    float u = gw > 1 ? (float)gx / (gw - 1) : 0;
                    float v = gh > 1 ? (float)gy / (gh - 1) : 0;
                    blend(x + gx, y + gy, cs.at(u, v));
                }
    }

    // Draw a string; `cs` is sampled across the *whole string box* so gradients
    // span the full text, not each glyph.
    void text(const Font &font, const String &s, int x, int y, const ColorSpec &cs)
    {
        int total = font.measure(s);
        int gh = font.glyphH();
        int penX = x;
        for (uint16_t k = 0; k < s.length(); k++)
        {
            char ch = s[k];
            int gw = font.glyphW();
            for (int gy = 0; gy < gh; gy++)
                for (int gx = 0; gx < gw; gx++)
                    if (font.pixel(ch, gx, gy))
                    {
                        int absX = penX + gx;
                        float u = total > 1 ? (float)(absX - x) / (total - 1) : 0;
                        float v = gh > 1 ? (float)gy / (gh - 1) : 0;
                        blend(absX, y + gy, cs.at(u, v));
                    }
            penX += font.advance();
        }
    }

    // 4x4 purple/black checker marking a missing dependency/asset (spec).
    void missingPattern(int x, int y)
    {
        const Rgba purple(160, 0, 200, 255), black(0, 0, 0, 255);
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                blend(x + i, y + j, ((i + j) & 1) ? black : purple);
    }

    // Blit a decoded BGRA asset buffer at (x,y). If `tint` is non-null the
    // asset is treated as monochrome/grayscale and recolored: luminance*alpha
    // drives the tint's alpha (spec: "monochrome/grayscale assets can be
    // colored").
    void blitBGRA(const uint8_t *bgra, int aw, int ah, int x, int y,
                  const Rgba *tint = nullptr)
    {
        for (int j = 0; j < ah; j++)
            for (int i = 0; i < aw; i++)
            {
                const uint8_t *p = bgra + ((size_t)j * aw + i) * 4;
                uint8_t b = p[0], g = p[1], r = p[2], a = p[3];
                if (tint)
                {
                    uint8_t lum = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                    uint8_t srcA = (uint16_t)lum * a / 255;
                    blend(x + i, y + j, Rgba(tint->r, tint->g, tint->b, srcA));
                }
                else
                {
                    blend(x + i, y + j, Rgba(r, g, b, a));
                }
            }
    }

    // ——— compositing: draw `over` on top of *this* (source-over) ———
    void composite(const FrameBuffer &over)
    {
        if (over.w != w || over.h != h) return;
        for (size_t i = 0; i < px.size(); i++)
        {
            const Rgba &s = over.px[i];
            if (s.a == 0) continue;
            if (s.a == 255) { px[i] = s; continue; }
            Rgba &d = px[i];
            uint16_t sa = s.a, da = 255 - sa;
            d.r = (uint8_t)((s.r * sa + d.r * da) / 255);
            d.g = (uint8_t)((s.g * sa + d.g * da) / 255);
            d.b = (uint8_t)((s.b * sa + d.b * da) / 255);
            d.a = (uint8_t)(s.a > d.a ? s.a : d.a);
        }
    }
};

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

    // ——— clipping helpers ———
    // The primitives iterate over the object's geometry, which comes straight
    // from JSON. A rectangle of 2e9 x 2e9 used to loop for hours inside one
    // render() call and trip the task watchdog, so every loop is clipped to
    // the buffer first. The *extent* used for gradient sampling stays the
    // object's full size, so clipping changes nothing about what is drawn —
    // only how many pixels are visited.
    struct Span { int lo, hi; }; // inclusive; empty when lo > hi

    // Bounds that keep one render() short. A brush or a line much larger than
    // the panel can only be a mistake or hostile input.
    static const int kMaxBrush = 64;
    static const int kMaxLineSteps = 8192;

    Span clipX(long long from, long long to) const
    {
        long long lo = from < to ? from : to, hi = from < to ? to : from;
        if (lo < 0) lo = 0;
        if (hi > (long long)w - 1) hi = (long long)w - 1;
        return {(int)lo, (int)hi};
    }
    Span clipY(long long from, long long to) const
    {
        long long lo = from < to ? from : to, hi = from < to ? to : from;
        if (lo < 0) lo = 0;
        if (hi > (long long)h - 1) hi = (long long)h - 1;
        return {(int)lo, (int)hi};
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
    void line(long long lx0, long long ly0, long long lx1, long long ly1,
              int thickness, const ColorSpec &cs)
    {
        long long bx = min(lx0, lx1), by = min(ly0, ly1);
        long long bw = llabs(lx1 - lx0) + 1, bh = llabs(ly1 - ly0) + 1;
        int t = max(1, thickness);
        if (t > kMaxBrush) t = kMaxBrush;

        // Nothing of the bounding box (plus the brush) is on screen.
        if (bx + bw - 1 + t < 0 || by + bh - 1 + t < 0 || bx >= w || by >= h) return;

        // Bresenham takes max(|dx|,|dy|)+1 steps. Clipping the endpoints would
        // change the pixel sequence, so a line far longer than the panel is
        // refused outright instead: it cannot be meaningful here, and letting
        // it run used to hang render() until the watchdog fired.
        long long steps = max(bw, bh);
        if (steps > kMaxLineSteps)
        {
            Serial.printf("⚠️ line spans %lld px — ignored (max %d)\n",
                          steps, kMaxLineSteps);
            return;
        }

        int x0 = (int)lx0, y0 = (int)ly0, x1 = (int)lx1, y1 = (int)ly1;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        int x = x0, y = y0;
        for (;;)
        {
            for (int oy = 0; oy < t; oy++)
                for (int ox = 0; ox < t; ox++)
                {
                    long long px_ = (long long)x + ox, py_ = (long long)y + oy;
                    if (px_ < 0 || py_ < 0 || px_ >= w || py_ >= h) continue;
                    float u = bw > 1 ? (float)(px_ - bx) / (bw - 1) : 0;
                    float v = bh > 1 ? (float)(py_ - by) / (bh - 1) : 0;
                    blend((int)px_, (int)py_, cs.at(u, v));
                }
            if (x == x1 && y == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
    }

    // Rectangle from (x,y), size dx*dy. border<=0 → fill, else outline of that
    // thickness. Colored via ColorSpec across the rect's box.
    void rect(long long x, long long y, long long dx, long long dy,
              long long border, const ColorSpec &cs)
    {
        if (dx <= 0 || dy <= 0) return;
        // Visit only the on-screen part; `dx`/`dy` still drive the gradient.
        Span cx = clipX(x, x + dx - 1);
        Span cy = clipY(y, y + dy - 1);
        for (int py = cy.lo; py <= cy.hi; py++)
            for (int px = cx.lo; px <= cx.hi; px++)
            {
                long long i = px - x, j = py - y;
                bool edge = (i < border) || (j < border) ||
                            (i >= dx - border) || (j >= dy - border);
                if (border > 0 && !edge) continue;
                float u = dx > 1 ? (float)i / (dx - 1) : 0;
                float v = dy > 1 ? (float)j / (dy - 1) : 0;
                blend(px, py, cs.at(u, v));
            }
    }

    // Draw one glyph at (x,y). `cs` sampled across the glyph box.
    void glyph(const Font &font, char ch, long long x, long long y, const ColorSpec &cs)
    {
        int gw = font.glyphW(), gh = font.glyphH();
        if (x + gw <= 0 || y + gh <= 0 || x >= w || y >= h) return;
        for (int gy = 0; gy < gh; gy++)
            for (int gx = 0; gx < gw; gx++)
                if (font.pixel(ch, gx, gy))
                {
                    long long ax = x + gx, ay = y + gy;
                    if (ax < 0 || ay < 0 || ax >= w || ay >= h) continue;
                    float u = gw > 1 ? (float)gx / (gw - 1) : 0;
                    float v = gh > 1 ? (float)gy / (gh - 1) : 0;
                    blend((int)ax, (int)ay, cs.at(u, v));
                }
    }

    // Draw a string; `cs` is sampled across the *whole string box* so gradients
    // span the full text, not each glyph.
    void text(const Font &font, const String &s, long long x, long long y,
              const ColorSpec &cs)
    {
        int total = font.measure(s);
        int gh = font.glyphH();
        int gw = font.glyphW();
        if (y + gh <= 0 || y >= h) return; // whole line is above/below
        long long penX = x;
        for (uint16_t k = 0; k < s.length(); k++)
        {
            if (penX >= w) break;          // rest of the line is off the right
            if (penX + gw > 0)             // skip glyphs off the left
            {
                char ch = s[k];
                for (int gy = 0; gy < gh; gy++)
                    for (int gx = 0; gx < gw; gx++)
                        if (font.pixel(ch, gx, gy))
                        {
                            long long absX = penX + gx, absY = y + gy;
                            if (absX < 0 || absY < 0 || absX >= w || absY >= h) continue;
                            float u = total > 1 ? (float)(absX - x) / (total - 1) : 0;
                            float v = gh > 1 ? (float)gy / (gh - 1) : 0;
                            blend((int)absX, (int)absY, cs.at(u, v));
                        }
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

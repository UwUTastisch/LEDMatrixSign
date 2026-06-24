// color.h — RGBA color + ColorSpec (solid or linear-gradient).
// Parses CSS-ish color strings used in anim.json:
//   "#rgb", "#rrggbb", "#rrggbbaa", "linear-gradient(<deg>deg, #a, #b[, ...])".
#pragma once
#include <Arduino.h>
#include <vector>
#include <math.h>

struct Rgba
{
    uint8_t r = 0, g = 0, b = 0, a = 0; // a=0 → transparent (used for overlay)
    Rgba() {}
    Rgba(uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) : r(R), g(G), b(B), a(A) {}
    bool opaque() const { return a == 255; }
};

namespace ColorParse
{
    inline int hexNib(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // Parse one "#..." token into Rgba. Returns false on malformed input.
    inline bool parseHex(const String &in, Rgba &out)
    {
        String s = in;
        s.trim();
        if (s.length() < 4 || s[0] != '#') return false;
        String h = s.substring(1);
        auto nib = [&](int i) { return hexNib(h[i]); };
        if (h.length() == 3 || h.length() == 4)
        {
            int r = nib(0), g = nib(1), b = nib(2);
            int a = (h.length() == 4) ? nib(3) : 0xF;
            if ((r | g | b | a) < 0) return false;
            out = Rgba(r * 17, g * 17, b * 17, a * 17);
            return true;
        }
        if (h.length() == 6 || h.length() == 8)
        {
            int v[8];
            for (uint8_t i = 0; i < h.length(); i++)
            {
                v[i] = nib(i);
                if (v[i] < 0) return false;
            }
            uint8_t a = (h.length() == 8) ? (v[6] * 16 + v[7]) : 255;
            out = Rgba(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5], a);
            return true;
        }
        return false;
    }
}

// A color that may vary across an object's bounding box.
// For solid colors only `stops[0]` is used. For gradients we evaluate along
// `angleDeg` (0deg = left→right, 90deg = bottom→top, matching CSS convention).
class ColorSpec
{
public:
    std::vector<Rgba> stops;
    float angleDeg = 0.0f;
    bool gradient = false;

    ColorSpec() { stops.push_back(Rgba(255, 255, 255, 255)); }

    static ColorSpec solid(const Rgba &c)
    {
        ColorSpec s;
        s.stops.clear();
        s.stops.push_back(c);
        return s;
    }

    // Parse either a hex color or a linear-gradient(...) expression.
    // Always succeeds: on malformed input it falls back to opaque white so a
    // bad color never aborts a render (spec: "try not to throw any error").
    static ColorSpec parse(const String &raw)
    {
        ColorSpec spec;
        String s = raw;
        s.trim();
        if (s.startsWith("linear-gradient"))
        {
            int lp = s.indexOf('(');
            int rp = s.lastIndexOf(')');
            if (lp >= 0 && rp > lp)
            {
                String inner = s.substring(lp + 1, rp);
                spec.gradient = true;
                spec.stops.clear();
                spec.angleDeg = 0.0f;
                // split on commas that are not inside a token; tokens here are
                // simple (#hex or "<n>deg"), so a plain comma split is safe.
                int start = 0;
                while (start <= inner.length())
                {
                    int comma = inner.indexOf(',', start);
                    String tok = (comma < 0) ? inner.substring(start)
                                             : inner.substring(start, comma);
                    tok.trim();
                    if (tok.endsWith("deg"))
                    {
                        spec.angleDeg = tok.substring(0, tok.length() - 3).toFloat();
                    }
                    else if (tok.startsWith("#"))
                    {
                        Rgba c;
                        if (ColorParse::parseHex(tok, c)) spec.stops.push_back(c);
                    }
                    if (comma < 0) break;
                    start = comma + 1;
                }
                if (spec.stops.empty()) spec.stops.push_back(Rgba(255, 255, 255));
                if (spec.stops.size() == 1) spec.gradient = false;
                return spec;
            }
        }
        Rgba c;
        if (ColorParse::parseHex(s, c))
        {
            spec.stops.clear();
            spec.stops.push_back(c);
        }
        return spec;
    }

    // Evaluate the color at fractional position (u,v) within the object's box,
    // u,v ∈ [0,1] (u = x axis, v = y axis, origin top-left).
    Rgba at(float u, float v) const
    {
        if (!gradient || stops.size() < 2) return stops.empty() ? Rgba() : stops[0];

        // Project (u,v) onto the gradient axis. CSS: 0deg points "up", angle
        // increases clockwise. We map to a t ∈ [0,1] along that direction.
        float rad = angleDeg * (float)M_PI / 180.0f;
        // direction vector (CSS 0deg = +y up). In our top-left coords y grows
        // downward, so flip the y component.
        float dx = sinf(rad);
        float dy = -cosf(rad);
        // center the coordinates so the gradient spans the box symmetrically
        float t = ((u - 0.5f) * dx + (v - 0.5f) * dy) + 0.5f;
        if (t < 0) t = 0;
        if (t > 1) t = 1;

        float scaled = t * (stops.size() - 1);
        int i = (int)floorf(scaled);
        if (i >= (int)stops.size() - 1) return stops.back();
        float f = scaled - i;
        const Rgba &a = stops[i];
        const Rgba &b = stops[i + 1];
        auto lerp = [&](uint8_t x, uint8_t y) { return (uint8_t)(x + (y - x) * f); };
        return Rgba(lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a));
    }
};

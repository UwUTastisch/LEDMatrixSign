// composition.h — the anim.json object tree.
//
// An Animation is { default_params, frames[] }. Each Frame is an ordered set of
// typed sub-objects (scrolling_text / text / line / rectangle / asset) plus the
// control entries clear / load_anim, and a duration. Drawables are numbered
// 1-based by their order inside the frame; frames are referenced 1-based, so a
// reference "f1:2" means frame #1, drawable #2 (see clear.obj).
//
// Objects are retained-mode: applying a frame *adds* its drawables to a layer's
// live composition where they persist until a clear removes them. This file
// owns parsing, (de)serialisation and rasterisation; timing/recursion live in
// player.h.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <memory>
#include "../gfx/framebuffer.h"
#include "../gfx/asset.h"

using Params = std::map<String, String>;

// ——— {placeholder} substitution ———
// Replace {key} with params[key]; if a key has no value (and no default was
// folded into params), leave the literal token untouched and never throw.
inline String substituteParams(const String &tmpl, const Params &params)
{
    String out;
    out.reserve(tmpl.length() + 8);
    int i = 0, n = tmpl.length();
    while (i < n)
    {
        char c = tmpl[i];
        if (c == '{')
        {
            int close = tmpl.indexOf('}', i + 1);
            if (close > i)
            {
                String key = tmpl.substring(i + 1, close);
                auto it = params.find(key);
                if (it != params.end())
                    out += it->second;          // substitute
                else
                    out += tmpl.substring(i, close + 1); // ignore: keep literal
                i = close + 1;
                continue;
            }
        }
        out += c;
        i++;
    }
    return out;
}

// Render-time environment handed to each object.
struct RenderEnv
{
    FrameBuffer &fb;
    const Params &params; // effective params (defaults + overrides)
    const String &animDir; // e.g. "/anim/de.uwutastisch.blahaj"
};

// ——— base drawable ———
class CObj
{
public:
    String id;      // "frame:index", both 1-based, e.g. "1:2"
    String objname; // optional user handle
    virtual ~CObj() {}
    virtual const char *type() const = 0;
    virtual void render(RenderEnv &env) = 0;
    virtual void advance(float /*dt*/) {} // animated objects override
    // Freeze param substitution / asset path against the context active when
    // this object is added to a layer, so it stays correct even after a nested
    // sub-animation switches params or directory.
    virtual void bind(const Params & /*params*/, const String & /*animDir*/) {}
    virtual void toJson(JsonObject frameObj) = 0; // re-emit into a frame object

    bool matches(const String &ref) const
    {
        // ref like "f1:2" or an objname
        if (ref.length() && (ref[0] == 'f' || ref[0] == 'F'))
            if (ref.substring(1) == id) return true;
        return objname.length() && ref == objname;
    }
};

// ——— text ———
class TextObj : public CObj
{
public:
    int x = 0, y = 0;
    String tmpl, fontName = "5x7";
    ColorSpec color;
    String bound; bool isBound = false;
    const char *type() const override { return "text"; }
    void bind(const Params &p, const String &) override
    {
        bound = substituteParams(tmpl, p); isBound = true;
    }
    void render(RenderEnv &env) override
    {
        String s = isBound ? bound : substituteParams(tmpl, env.params);
        env.fb.text(Fonts::byName(fontName), s, x, y, color);
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = f["text"].to<JsonObject>();
        o["x"] = x; o["y"] = y; o["text"] = tmpl; o["font"] = fontName;
    }
};

// ——— scrolling text ———
class ScrollTextObj : public CObj
{
public:
    int x = 0, y = 0, dx = 0, dy = 0; // viewport box
    String tmpl, fontName = "5x7";
    ColorSpec color;
    float speed = 0;       // px/s
    bool horizontal = true;
    float phase = 0;       // px scrolled so far
    String bound; bool isBound = false;

    const char *type() const override { return "scrolling_text"; }

    void advance(float dt) override { phase += speed * dt; }
    void bind(const Params &p, const String &) override
    {
        bound = substituteParams(tmpl, p); isBound = true;
    }

    void render(RenderEnv &env) override
    {
        const Font &font = Fonts::byName(fontName);
        String s = isBound ? bound : substituteParams(tmpl, env.params);
        int textW = font.measure(s);
        int textH = font.glyphH();
        int span = horizontal ? (textW + dx) : (textH + dy); // wrap distance
        if (span <= 0) span = 1;
        float ph = fmodf(phase, (float)span);

        int penX = x, penY = y;
        if (horizontal) penX = x + dx - (int)ph; // enter from right, exit left
        else            penY = y + dy - (int)ph; // enter from bottom, exit top

        int cx = penX;
        for (uint16_t k = 0; k < s.length(); k++)
        {
            char ch = s[k];
            int gw = font.glyphW(), gh = font.glyphH();
            for (int gy = 0; gy < gh; gy++)
                for (int gx = 0; gx < gw; gx++)
                    if (font.pixel(ch, gx, gy))
                    {
                        int ax = (horizontal ? cx + gx : x + gx);
                        int ay = (horizontal ? y + gy : penY + gy + (int)0);
                        if (!horizontal) { ax = x + gx; ay = penY + gy; }
                        // clip to viewport box
                        if (ax < x || ax >= x + dx || ay < y || ay >= y + dy)
                            continue;
                        float u = textW > 1 ? (float)((horizontal ? cx + gx - penX : gx)) / (textW - 1) : 0;
                        float v = gh > 1 ? (float)gy / (gh - 1) : 0;
                        env.fb.blend(ax, ay, color.at(u, v));
                    }
            if (horizontal) cx += font.advance();
            else            penY += font.advance();
        }
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = f["scrolling_text"].to<JsonObject>();
        o["x"] = x; o["y"] = y; o["dx"] = dx; o["dy"] = dy;
        o["text"] = tmpl; o["font"] = fontName; o["scroll_speed"] = speed;
        o["scroll_direction"] = horizontal ? "horizontal" : "vertical";
    }
};

// ——— line ———
class LineObj : public CObj
{
public:
    int x = 0, y = 0, dx = 0, dy = 0, thickness = 1;
    ColorSpec color;
    const char *type() const override { return "line"; }
    void render(RenderEnv &env) override
    {
        env.fb.line(x, y, x + dx, y + dy, thickness, color);
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = f["line"].to<JsonObject>();
        o["x"] = x; o["y"] = y; o["dx"] = dx; o["dy"] = dy;
        o["thickness"] = thickness;
        if (objname.length()) o["objname"] = objname;
    }
};

// ——— rectangle ———
class RectObj : public CObj
{
public:
    int x = 0, y = 0, dx = 0, dy = 0, border = 0;
    ColorSpec color;
    const char *type() const override { return "rectangle"; }
    void render(RenderEnv &env) override
    {
        env.fb.rect(x, y, dx, dy, border, color);
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = f["rectangle"].to<JsonObject>();
        o["x"] = x; o["y"] = y; o["dx"] = dx; o["dy"] = dy; o["border"] = border;
    }
};

// ——— asset ———
class AssetObj : public CObj
{
public:
    int x = 0, y = 0;
    String name;
    bool hasTint = false;
    Rgba tint;
    String boundPath;
    const char *type() const override { return "asset"; }
    void bind(const Params &, const String &animDir) override
    {
        boundPath = animDir + "/assets/" + name;
    }
    void render(RenderEnv &env) override
    {
        String path = boundPath.length() ? boundPath
                                          : env.animDir + "/assets/" + name;
        Asset a = AssetLoader::load(path);
        if (!a.ok)
        {
            env.fb.missingPattern(x, y); // spec: 4x4 purple/black on miss
            return;
        }
        env.fb.blitBGRA(a.bgra.data(), a.w, a.h, x, y, hasTint ? &tint : nullptr);
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = f["asset"].to<JsonObject>();
        o["x"] = x; o["y"] = y; o["name"] = name;
    }
};

// ——— control entries (not drawables) ———
struct ClearDirective
{
    bool all = true;
    std::vector<String> targets; // when !all
};

struct LoadAnimDirective
{
    String name;
    Params params;
    int cycles = 1;
    float speed = 1.0f;
};

// ——— a frame ———
struct Frame
{
    std::vector<std::shared_ptr<CObj>> drawables;
    std::vector<ClearDirective> clears;
    std::vector<LoadAnimDirective> loads;
    uint32_t duration = 0; // ms
    bool hasLoad() const { return !loads.empty(); }
};

// ——— the animation ———
class Animation
{
public:
    Params defaultParams;
    std::vector<Frame> frames;
    String dir; // base directory of this animation on the FS

    void clear() { defaultParams.clear(); frames.clear(); }

    // Merge default params with caller overrides → effective param map.
    Params effectiveParams(const Params &overrides) const
    {
        Params p = defaultParams;
        for (auto &kv : overrides) p[kv.first] = kv.second;
        return p;
    }

    bool parse(JsonObjectConst root, const String &animDir);
    void serialize(JsonObject root) const;
};

// ——— free-standing parsing helpers (also used by the API buffer) ———
inline ColorSpec parseColorVar(JsonVariantConst v)
{
    if (v.is<const char *>()) return ColorSpec::parse(String(v.as<const char *>()));
    return ColorSpec(); // default white
}

inline std::shared_ptr<CObj> parseDrawableObj(const String &key, JsonObjectConst o,
                                              const String &id)
{
    if (key == "text")
    {
        auto t = std::make_shared<TextObj>();
        t->id = id; t->x = o["x"] | 0; t->y = o["y"] | 0;
        t->tmpl = (const char *)(o["text"] | "");
        t->fontName = (const char *)(o["font"] | "5x7");
        t->color = parseColorVar(o["color"]);
        if (o["objname"].is<const char *>()) t->objname = (const char *)o["objname"];
        return t;
    }
    if (key == "scrolling_text")
    {
        auto t = std::make_shared<ScrollTextObj>();
        t->id = id; t->x = o["x"] | 0; t->y = o["y"] | 0;
        t->dx = o["dx"] | 0; t->dy = o["dy"] | 0;
        t->tmpl = (const char *)(o["text"] | "");
        t->fontName = (const char *)(o["font"] | "5x7");
        t->color = parseColorVar(o["color"]);
        t->speed = o["scroll_speed"] | 0.0f;
        String dir = (const char *)(o["scroll_direction"] | "horizontal");
        t->horizontal = !(dir == "vertical");
        if (o["objname"].is<const char *>()) t->objname = (const char *)o["objname"];
        return t;
    }
    if (key == "line")
    {
        auto l = std::make_shared<LineObj>();
        l->id = id; l->x = o["x"] | 0; l->y = o["y"] | 0;
        l->dx = o["dx"] | 0; l->dy = o["dy"] | 0;
        l->thickness = o["thickness"] | 1;
        l->color = parseColorVar(o["color"]);
        if (o["objname"].is<const char *>()) l->objname = (const char *)o["objname"];
        return l;
    }
    if (key == "rectangle")
    {
        auto r = std::make_shared<RectObj>();
        r->id = id; r->x = o["x"] | 0; r->y = o["y"] | 0;
        r->dx = o["dx"] | 0; r->dy = o["dy"] | 0;
        r->border = o["border"] | 0;
        r->color = parseColorVar(o["color"]);
        if (o["objname"].is<const char *>()) r->objname = (const char *)o["objname"];
        return r;
    }
    if (key == "asset")
    {
        auto a = std::make_shared<AssetObj>();
        a->id = id; a->x = o["x"] | 0; a->y = o["y"] | 0;
        a->name = (const char *)(o["name"] | "");
        if (o["color"].is<const char *>())
        {
            ColorSpec cs = ColorSpec::parse((const char *)o["color"]);
            a->hasTint = true;
            a->tint = cs.stops.empty() ? Rgba(255, 255, 255) : cs.stops[0];
        }
        if (o["objname"].is<const char *>()) a->objname = (const char *)o["objname"];
        return a;
    }
    return nullptr;
}

inline Frame parseFrameObject(JsonObjectConst fo, int frameNo)
{
    Frame frame;
    frame.duration = fo["duration"] | 0;
    int objNo = 0;
    for (JsonPairConst kv : fo)
    {
        String key = kv.key().c_str();
        if (key == "duration") continue;

        if (key == "clear")
        {
            ClearDirective cd;
            JsonObjectConst co = kv.value();
            JsonArrayConst objs = co["obj"];
            if (!objs.isNull())
            {
                cd.all = false;
                for (JsonVariantConst v : objs)
                    cd.targets.push_back(String(v.as<const char *>()));
            }
            frame.clears.push_back(cd);
            continue;
        }
        if (key == "load_anim")
        {
            LoadAnimDirective ld;
            JsonObjectConst lo = kv.value();
            ld.name = (const char *)(lo["name"] | "");
            ld.cycles = lo["cycles"] | 1;
            ld.speed = lo["speed"] | 1.0f;
            JsonObjectConst lp = lo["params"];
            if (!lp.isNull())
                for (JsonPairConst pkv : lp)
                    ld.params[String(pkv.key().c_str())] =
                        pkv.value().is<const char *>()
                            ? String(pkv.value().as<const char *>())
                            : String(pkv.value().as<float>());
            frame.loads.push_back(ld);
            continue;
        }

        objNo++;
        String id = String(frameNo) + ":" + String(objNo);
        auto obj = parseDrawableObj(key, kv.value(), id);
        if (obj) frame.drawables.push_back(obj);
    }
    return frame;
}

inline bool Animation::parse(JsonObjectConst root, const String &animDir)
{
    clear();
    dir = animDir;

    JsonObjectConst dp = root["default_params"];
    if (!dp.isNull())
        for (JsonPairConst kv : dp)
            defaultParams[String(kv.key().c_str())] =
                kv.value().is<const char *>() ? String(kv.value().as<const char *>())
                                              : String(kv.value().as<float>());

    JsonArrayConst arr = root["frames"];
    if (arr.isNull()) return false;

    int frameNo = 0;
    for (JsonObjectConst fo : arr)
    {
        frameNo++;
        frames.push_back(parseFrameObject(fo, frameNo));
    }
    return true;
}

inline void Animation::serialize(JsonObject root) const
{
    JsonObject dp = root["default_params"].to<JsonObject>();
    for (auto &kv : defaultParams) dp[kv.first] = kv.second;

    JsonArray arr = root["frames"].to<JsonArray>();
    for (const Frame &f : frames)
    {
        JsonObject fo = arr.add<JsonObject>();
        for (auto &d : f.drawables) d->toJson(fo);
        for (auto &c : f.clears)
        {
            JsonObject co = fo["clear"].to<JsonObject>();
            if (!c.all)
            {
                JsonArray objs = co["obj"].to<JsonArray>();
                for (auto &t : c.targets) objs.add(t);
            }
        }
        for (auto &l : f.loads)
        {
            JsonObject lo = fo["load_anim"].to<JsonObject>();
            lo["name"] = l.name; lo["cycles"] = l.cycles; lo["speed"] = l.speed;
            JsonObject lp = lo["params"].to<JsonObject>();
            for (auto &kv : l.params) lp[kv.first] = kv.second;
        }
        if (f.duration) fo["duration"] = f.duration;
    }
}

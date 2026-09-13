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

inline JsonObject addDrawableJsonObject(JsonObject frameObj, const char *type)
{
    JsonArray arr = frameObj["drawables"].as<JsonArray>();
    if (arr.isNull())
        arr = frameObj["drawables"].to<JsonArray>();
    JsonObject item = arr.add<JsonObject>();
    return item[type].to<JsonObject>();
}

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
    // Structural equality test (ignore `id`). Used to avoid duplicate
    // stacking when re-applying frames that already exist in a layer.
    virtual bool equals(const CObj &other) const { return false; }

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
        env.fb.text(Fonts::byName(fontName), s, (long long)x, (long long)y, color);
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = addDrawableJsonObject(f, "text");
        o["x"] = x; o["y"] = y; o["text"] = tmpl; o["font"] = fontName;
        if (objname.length()) o["objname"] = objname;
        o["color"] = color.toString();
    }
    bool equals(const CObj &other) const override
    {
        if (other.type() != type()) return false;
        const TextObj &o = static_cast<const TextObj &>(other);
        return x == o.x && y == o.y && tmpl == o.tmpl && fontName == o.fontName &&
               objname == o.objname && color.toString() == o.color.toString();
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
        long long span = horizontal ? ((long long)textW + dx)
                                    : ((long long)textH + dy); // wrap distance
        if (span <= 0) span = 1;
        float ph = fmodf(phase, (float)span);

        long long penX = x, penY = y;
        if (horizontal) penX = (long long)x + dx - (long long)ph; // in from right
        else            penY = (long long)y + dy - (long long)ph; // in from bottom

        // All of this runs in 64-bit: x/y/dx/dy come from JSON and a window of
        // 2^31-1 used to overflow these sums (undefined behaviour) before the
        // clip below could reject anything.
        long long winX = x, winY = y;
        long long winW = dx, winH = dy;
        long long cx = penX, cy = penY;
        for (uint16_t k = 0; k < s.length(); k++)
        {
            char ch = s[k];
            int gw = font.glyphW(), gh = font.glyphH();
            // Skip glyphs that have already left the window, and stop once the
            // pen is past its far edge.
            long long gx0 = horizontal ? cx : winX;
            long long gy0 = horizontal ? winY : cy;
            if (horizontal && gx0 >= winX + winW) break;
            if (!horizontal && gy0 >= winY + winH) break;
            if ((horizontal && gx0 + gw > winX) || (!horizontal && gy0 + gh > winY))
            {
                for (int gy = 0; gy < gh; gy++)
                    for (int gx = 0; gx < gw; gx++)
                        if (font.pixel(ch, gx, gy))
                        {
                            long long ax = horizontal ? cx + gx : winX + gx;
                            long long ay = horizontal ? winY + gy : cy + gy;
                            // clip to the viewport box, then to the buffer
                            if (ax < winX || ax >= winX + winW ||
                                ay < winY || ay >= winY + winH)
                                continue;
                            if (ax < 0 || ay < 0 || ax >= env.fb.w || ay >= env.fb.h)
                                continue;
                            float u = textW > 1
                                ? (float)(horizontal ? cx + gx - penX : gx) / (textW - 1)
                                : 0;
                            float v = gh > 1 ? (float)gy / (gh - 1) : 0;
                            env.fb.blend((int)ax, (int)ay, color.at(u, v));
                        }
            }
            if (horizontal) cx += font.advance();
            else            cy += font.advance();
        }
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = addDrawableJsonObject(f, "scrolling_text");
        o["x"] = x; o["y"] = y; o["dx"] = dx; o["dy"] = dy;
        o["text"] = tmpl; o["font"] = fontName; o["scroll_speed"] = speed;
        o["scroll_direction"] = horizontal ? "horizontal" : "vertical";
        if (objname.length()) o["objname"] = objname;
        o["color"] = color.toString();
    }
    bool equals(const CObj &other) const override
    {
        if (other.type() != type()) return false;
        const ScrollTextObj &o = static_cast<const ScrollTextObj &>(other);
        return x == o.x && y == o.y && dx == o.dx && dy == o.dy && tmpl == o.tmpl &&
               fontName == o.fontName && speed == o.speed && horizontal == o.horizontal &&
               objname == o.objname && color.toString() == o.color.toString();
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
        // 64-bit so x + dx cannot overflow; FrameBuffer::line clips and caps.
        env.fb.line((long long)x, (long long)y,
                    (long long)x + dx, (long long)y + dy, thickness, color);
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = addDrawableJsonObject(f, "line");
        o["x"] = x; o["y"] = y; o["dx"] = dx; o["dy"] = dy;
        o["thickness"] = thickness;
        if (objname.length()) o["objname"] = objname;
        o["color"] = color.toString();
    }
    bool equals(const CObj &other) const override
    {
        if (other.type() != type()) return false;
        const LineObj &o = static_cast<const LineObj &>(other);
        return x == o.x && y == o.y && dx == o.dx && dy == o.dy && thickness == o.thickness &&
               objname == o.objname && color.toString() == o.color.toString();
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
        env.fb.rect((long long)x, (long long)y, (long long)dx, (long long)dy,
                    (long long)border, color);
    }
    void toJson(JsonObject f) override
    {
        JsonObject o = addDrawableJsonObject(f, "rectangle");
        o["x"] = x; o["y"] = y; o["dx"] = dx; o["dy"] = dy; o["border"] = border;
        if (objname.length()) o["objname"] = objname;
        o["color"] = color.toString();
    }
    bool equals(const CObj &other) const override
    {
        if (other.type() != type()) return false;
        const RectObj &o = static_cast<const RectObj &>(other);
        return x == o.x && y == o.y && dx == o.dx && dy == o.dy && border == o.border &&
               objname == o.objname && color.toString() == o.color.toString();
    }
};

// ——— asset path resolution ———
// "file.bmp"          → <animDir>/assets/file.bmp        (the drawing animation's own assets)
// "<animid>/file.bmp" → /anim/<animid>/assets/file.bmp   (any animation's assets)
// The second form is the only way the API overlay can reach an asset: it has
// no animation directory, so a bare name would resolve to /assets/<name>,
// which nothing ever writes to. It also lets one animation reuse another's
// assets.
inline String resolveAssetPath(const String &name, const String &animDir)
{
    int slash = name.indexOf('/');
    if (slash > 0)
        return "/anim/" + name.substring(0, slash) + "/assets/" + name.substring(slash + 1);
    return animDir + "/assets/" + name;
}

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
        boundPath = resolveAssetPath(name, animDir);
    }
    void render(RenderEnv &env) override
    {
        String path = boundPath.length() ? boundPath
                                          : resolveAssetPath(name, env.animDir);
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
        JsonObject o = addDrawableJsonObject(f, "asset");
        o["x"] = x; o["y"] = y; o["name"] = name;
        if (objname.length()) o["objname"] = objname;
        if (hasTint) o["color"] = ColorSpec::solid(tint).toString();
    }
    bool equals(const CObj &other) const override
    {
        if (other.type() != type()) return false;
        const AssetObj &o = static_cast<const AssetObj &>(other);
        if (x != o.x || y != o.y || name != o.name || objname != o.objname) return false;
        if (hasTint != o.hasTint) return false;
        if (hasTint && (tint.r != o.tint.r || tint.g != o.tint.g || tint.b != o.tint.b || tint.a != o.tint.a))
            return false;
        return true;
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
    // Backwards-compatible parsing: prefer a `drawables` array in which each
    // element is an object like `{ "text": {...} }`. If that's present,
    // parse it and return. Otherwise fall back to the older per-key format
    // (keys are drawable types, possibly arrays of entries).
    JsonArrayConst drawArr = fo["drawables"];
    if (!drawArr.isNull())
    {
        for (JsonVariantConst v : drawArr)
        {
            JsonObjectConst item = v.as<JsonObjectConst>();
            // find the first key in the item (the drawable type)
            for (JsonPairConst kv : item)
            {
                String key = kv.key().c_str();
                objNo++;
                String id = String(frameNo) + ":" + String(objNo);
                auto obj = parseDrawableObj(key, kv.value().as<JsonObjectConst>(), id);
                if (obj) frame.drawables.push_back(obj);
                break; // only one key per array element
            }
        }
        return frame;
    }

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

        // Support either a single object value or an array of objects for the
        // same key type.
        if (kv.value().is<JsonArrayConst>())
        {
            for (JsonObjectConst e : kv.value().as<JsonArrayConst>())
            {
                objNo++;
                String id = String(frameNo) + ":" + String(objNo);
                auto obj = parseDrawableObj(key, e, id);
                if (obj) frame.drawables.push_back(obj);
            }
        }
        else
        {
            objNo++;
            String id = String(frameNo) + ":" + String(objNo);
            auto obj = parseDrawableObj(key, kv.value(), id);
            if (obj) frame.drawables.push_back(obj);
        }
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

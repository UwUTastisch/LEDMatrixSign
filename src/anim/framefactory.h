// framefactory.h — two overlaid layers + the animation timeline player.
//
// Buffer 1 (primary) is driven by the Player from anim.json on the FS.
// Buffer 0 (api) is the overlay written by /framebuffer/draw. The FrameFactory
// composites 0 over 1 and hands the result to the matrix driver. Both layers
// are retained-mode: drawables persist until a `clear` removes them.
#pragma once
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#include <mutex>
#endif
#include <Arduino.h>
#include <functional>
#include "composition.h"

// A retained composition rasterised into one FrameBuffer.
class Layer
{
public:
    FrameBuffer fb;
    std::vector<std::shared_ptr<CObj>> live;

    void init(uint16_t w, uint16_t h) { fb.init(w, h); }
    void reset()
    {
        live.clear();
        fb.clearAll();
    }

    void applyClear(const ClearDirective &cd)
    {
        if (cd.all) { live.clear(); return; }
        for (const String &ref : cd.targets)
        {
            for (auto it = live.begin(); it != live.end();)
                if ((*it)->matches(ref)) it = live.erase(it);
                else ++it;
        }
    }

    // Apply one frame's static content (clears + drawables) to the composition.
    // load_anim is handled by the Player, not here.
    void applyFrame(const Frame &f, const Params &params, const String &animDir)
    {
        for (const auto &c : f.clears) applyClear(c);
        for (const auto &d : f.drawables)
        {
            d->bind(params, animDir);
            // Avoid adding duplicates: if an equivalent object already
            // exists in `live`, skip pushing this one to prevent stacking on
            // repeated frame applications (common for single-frame anims).
            bool found = false;
            for (const auto &ex : live)
            {
                if (ex->type() == d->type() && ex->equals(*d)) { found = true; break; }
            }
            if (!found) live.push_back(d);
        }
    }

    void advance(float dt)
    {
        for (auto &o : live) o->advance(dt);
    }

    void rasterize(const Params &params, const String &animDir)
    {
        fb.clearAll();
        RenderEnv env{fb, params, animDir};
        for (auto &o : live) o->render(env);
    }
};

// Loader injected by the storage/api layer: resolve an animation id ("anim1",
// "de.uwutastisch.blahaj") to a parsed Animation. Returns false if missing.
using AnimLoader = std::function<bool(const String &id, Animation &out)>;

class Player
{
public:
    struct Context
    {
        std::shared_ptr<Animation> anim;
        Params params;   // effective params for this run
        float speed = 1; // cumulative; durations are divided by this
        int cyclesLeft;  // -1 = infinite
        int frameIdx = 0;
    };

    void setLoader(AnimLoader l) { loader = l; }

    bool running() const { return !stack.empty(); }
    bool isPaused() const { return paused; }

    // Pausing has to hold the *remaining* time of the current frame, not just
    // stop calling tick(): millis() keeps running, so a plain flag would make
    // the frame expire while paused and advance the moment playback resumes.
    void pause()
    {
        if (paused) return;
        paused = true;
        pausedAtMs = lastNowMs;
    }

    void resume()
    {
        if (!paused) return;
        paused = false;
        if (waiting) frameDeadline += (lastNowMs - pausedAtMs);
    }
    void setSpeedScale(float s) { userSpeed = (s > 0) ? s : 1.0f; }
    float speedScale() const { return userSpeed; }
    const String &currentId() const { return currentAnimId; }

    void stop(Layer &primary)
    {
        stack.clear();
        primary.reset();
        waiting = false;
        paused = false;
        currentAnimId = "";
    }

    // Begin playing a parsed animation. cycles<=0 → loop forever.
    void start(std::shared_ptr<Animation> anim, const Params &overrides,
               int cycles, Layer &primary)
    {
        stack.clear();
        primary.reset();
        Context c;
        c.anim = anim;
        c.params = anim->effectiveParams(overrides);
        c.speed = 1.0f;
        c.cyclesLeft = (cycles > 0) ? cycles : -1;
        c.frameIdx = 0;
        stack.push_back(c);
        waiting = false;
        paused = false;
        currentAnimId = anim->dir;
    }

    // Drive the timeline. Returns the params/dir of the active context so the
    // factory can rasterise with the right fallback context.
    void tick(unsigned long now, Layer &primary)
    {
        // Track the clock even while paused, so pause()/resume() can measure
        // the standstill and shift the frame deadline by it.
        if (paused) { lastNowMs = now; return; }
        lastNowMs = now;
        if (stack.empty()) return;

        if (waiting)
        {
            // scaled remaining handled by stored deadline
            if ((long)(now - frameDeadline) < 0) return;
            waiting = false;
        }

        int processedThisTick = 0;
        while (!stack.empty() && processedThisTick < kMaxFramesPerTick)
        {
            Context &top = stack.back();

            if (top.frameIdx >= (int)top.anim->frames.size())
            {
                // An animation with no frames can never consume a slot below,
                // so an infinite cycle count used to spin here forever and let
                // the task watchdog reset the board. Drop it instead.
                if (top.anim->frames.empty())
                {
                    Serial.println("⚠️ animation has no frames — stopping it");
                    stack.pop_back();
                    continue;
                }
                // finished a pass
                if (top.cyclesLeft > 0) top.cyclesLeft--;
                if (top.cyclesLeft == 0) { stack.pop_back(); continue; }
                top.frameIdx = 0; // -1 (infinite) or remaining cycles
                processedThisTick++; // a full pass counts, so a zero-duration
                                     // loop yields instead of spinning
                continue;
            }

            const Frame &frame = top.anim->frames[top.frameIdx++];
            primary.applyFrame(frame, top.params, top.anim->dir);

            // handle load_anim: push sub-context(s); they run before we resume
            bool pushed = false;
            for (const auto &ld : frame.loads)
            {
                if (stack.size() >= kMaxStackDepth) break;
                auto sub = std::make_shared<Animation>();
                if (loader && loader(ld.name, *sub))
                {
                    // Clear primary before running a sub-animation so its
                    // drawables don't accumulate across multiple sub-anims.
                    ClearDirective clearAll; clearAll.all = true;
                    primary.applyClear(clearAll);
                    Context c;
                    c.anim = sub;
                    c.params = sub->effectiveParams(ld.params);
                    c.speed = top.speed * (ld.speed > 0 ? ld.speed : 1.0f);
                    c.cyclesLeft = (ld.cycles > 0) ? ld.cycles : 1;
                    c.frameIdx = 0;
                    stack.push_back(c);
                    pushed = true;
                }
                else
                {
                    // dependency not found → leave a marker top-left (spec)
                    primary.fb.missingPattern(0, 0);
                }
            }
            if (pushed) { processedThisTick++; continue; } // run sub-anim next

            // wait out this frame's (speed-scaled) duration
            float scale = top.speed * userSpeed;
            uint32_t dur = (scale > 0) ? (uint32_t)(frame.duration / scale)
                                       : frame.duration;
            if (dur == 0) { processedThisTick++; continue; } // clear/instant
            frameDeadline = now + dur;
            waiting = true;
            return;
        }
    }

    // expose active context for rasterisation fallback
    bool activeContext(Params &outParams, String &outDir) const
    {
        if (stack.empty()) return false;
        outParams = stack.back().params;
        outDir = stack.back().anim->dir;
        return true;
    }

private:
    static const int kMaxStackDepth = 8;
    static const int kMaxFramesPerTick = 64;
    std::vector<Context> stack;
    AnimLoader loader;
    bool waiting = false;
    bool paused = false;
    unsigned long lastNowMs = 0;  // most recent tick(), paused or not
    unsigned long pausedAtMs = 0; // when pause() was called
    unsigned long frameDeadline = 0;
    float userSpeed = 1.0f; // live /anim/setspeed multiplier
    String currentAnimId;
};

// Owns the two layers, the player, and the composite output.
// Serialises access to the compositor.
//
// The HTTP handlers run on the AsyncTCP task while loop() renders on the
// Arduino task. Without a lock, apiApplyFrame() reallocating a layer's object
// vector while rasterize() iterates it is a use-after-free — the usual
// symptom being a LoadProhibited panic after a burst of draw requests.
// Recursive so a handler may hold the lock and still call a helper that takes
// it.
class CompositorLock
{
public:
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    CompositorLock() { handle = xSemaphoreCreateRecursiveMutex(); }
    void lock() { if (handle) xSemaphoreTakeRecursive(handle, portMAX_DELAY); }
    void unlock() { if (handle) xSemaphoreGiveRecursive(handle); }
private:
    SemaphoreHandle_t handle = nullptr;
#else
    // Host builds (tools/host-test) use the standard library.
    void lock() { m.lock(); }
    void unlock() { m.unlock(); }
private:
    std::recursive_mutex m;
#endif
};

// RAII guard.
class CompositorGuard
{
public:
    explicit CompositorGuard(CompositorLock &l) : lk(l) { lk.lock(); }
    ~CompositorGuard() { lk.unlock(); }
    CompositorGuard(const CompositorGuard &) = delete;
    CompositorGuard &operator=(const CompositorGuard &) = delete;

private:
    CompositorLock &lk;
};

class FrameFactory
{
public:
    Layer api;     // buffer 0 — overlay
    Layer primary; // buffer 1 — FS animations
    Player player;
    // Anything touching api/primary/player/out from an HTTP handler must hold
    // this: `CompositorGuard g(factory.lock);`
    CompositorLock lock;
    FrameBuffer out;

    void init(uint16_t w, uint16_t h)
    {
        api.init(w, h);
        primary.init(w, h);
        out.init(w, h);
        lastTickMs = millis();
    }

    void setLoader(AnimLoader l) { player.setLoader(l); }

    // Apply a single API-buffer frame (from /framebuffer/draw).
    void apiApplyFrame(const Frame &f)
    {
        CompositorGuard g(lock);
        apiApplyFrameLocked(f);
    }

    void apiApplyFrameLocked(const Frame &f)
    {
        static const Params empty;
        api.applyFrame(f, empty, "");
    }
    // Advance time, rasterise both layers, composite 0 over 1 into `out`.
    void render(unsigned long now)
    {
        CompositorGuard g(lock);
        float dt = (now - lastTickMs) / 1000.0f;
        if (dt < 0) dt = 0;
        lastTickMs = now;

        player.tick(now, primary);

        // Advance animated objects (scrolling text). The animation layer holds
        // still while the player is paused; the overlay is not part of the
        // timeline, so it keeps moving.
        primary.advance(player.isPaused() ? 0.0f : dt);
        api.advance(dt);

        // rasterise
        Params p;
        String dir;
        player.activeContext(p, dir);
        primary.rasterize(p, dir);
        static const Params empty;
        api.rasterize(empty, "");

        // composite: start from primary, overlay api
        out.px = primary.fb.px; // copy
        out.composite(api.fb);
    }

private:
    unsigned long lastTickMs = 0;
};

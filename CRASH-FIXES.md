# Crash hunt

You mentioned crashes while interacting with the firmware. I went looking, and
found seven problems — one of which explains "it crashes while I'm clicking
around" on its own. Every one is reproduced on the host with sanitizers, and
`crash-fixes.patch` fixes all seven. The evidence and the reproductions are
below so you can check the reasoning rather than take my word for it.

Apply after `ui-support.patch`:

```bash
patch -p1 < ui-support.patch
patch -p1 < crash-fixes.patch
```

(Both are already applied in the firmware folder I shipped.)

---

## 1. The API and the render loop share the compositor with no lock

**The likely cause of your crashes.** `loop()` calls `factory.render()` on the
Arduino task 60 times a second. The HTTP handlers run on the **AsyncTCP task**.
Both touch the same `std::vector<std::shared_ptr<CObj>>`: `/framebuffer/draw`
pushes into it while `rasterize()` iterates it. When `push_back` reallocates,
the render loop is left walking freed memory — which on an ESP32 is a
`LoadProhibited` panic. Nothing was synchronised: not the layers, not the
player stack, not `factory.out`.

The symptom fits: it needs a draw request to land in the same instant as a
render pass, so it looks random, and it gets much more likely the more you
interact — exactly when the UI is sending draws, or polling
`/framebuffer/getcomposition` while the animation layer is being rewritten.

Reproduced with ThreadSanitizer (`tools/host-test/race_probe.cpp`, two threads
doing what the two tasks do):

```
WARNING: ThreadSanitizer: data race
  Write of size 8 by thread T2:
    vector<shared_ptr<CObj>>::_M_realloc_insert(...)
    vector<shared_ptr<CObj>>::push_back(...)
  Previous read of size 8 by main thread:
    vector<shared_ptr<CObj>>::begin() ...
```

**Fix:** a recursive mutex in `FrameFactory` (FreeRTOS
`xSemaphoreCreateRecursiveMutex`, `std::recursive_mutex` on the host), taken by
`render()`, `apiApplyFrame()`, `apiClear()`, and by every handler that reads or
writes the layers or the player — `/framebuffer/get`,
`/framebuffer/getcomposition`, `/framebuffer/savetostorage`, `/anim/start`,
`/anim/stop`, `/anim/setspeed`, `/anim/status`. After the fix ThreadSanitizer
reports **no races**.

## 2. Hostile geometry hangs `render()` until the watchdog resets the board

`FrameBuffer::rect` loops `dx * dy` times and `line` walks
`max(|dx|,|dy|)` steps, with no clipping — `blend()` discards the off-screen
pixels, but only after the loop has visited them. One request is enough:

```json
{"rectangle": {"x": 0, "y": 0, "dx": 2000000000, "dy": 2000000000}}
{"line": {"x": 0, "y": 0, "dx": 2000000000, "dy": 0}}
{"line": {"x": 0, "y": 0, "dx": 10, "dy": 10, "thickness": 100000}}
```

Each of those still hadn't returned from a single `render()` after 20 seconds
of host CPU. On the device `loop()` never comes back, so the task watchdog
fires: `rst:0x8 TG1WDT_SYS_RST`.

This is reachable by accident, not just by malice: a typo in a hand-written
`anim.json`, or a `dx` that was meant to be an end coordinate. It is also
**persistent** — put such an object in an animation and every boot that plays
it resets the board again.

A fuzzer over random malformed frames (`tools/host-test/fuzz_frames.py`) hit it
in **35 of 225 inputs** — roughly one in six.

**Fix:** every primitive now clips its loop to the buffer before iterating, in
64-bit arithmetic, and the gradient still samples across the object's full
extent so nothing about the visible output changes. An oversized brush is
capped at 64 px; a line longer than 8192 steps is refused with a log line
(clipping its endpoints would change which pixels Bresenham visits, and a line
that long cannot be meaningful on a panel).

After the fix the worst of those cases renders in **0.08 ms**, and the same
fuzzer ran **25,354 inputs with no hangs and no crashes** in the same 180
seconds.

## 3. A malformed BMP aborts the firmware

`AssetLoader::load` trusts the width and height in the header:

```cpp
a.bgra.assign((size_t)bmpW * absH * 4, 0);
```

Nothing validates them, and `/file/uploadasset` stores whatever bytes it is
given. Under ASan, with the real loader:

| Header | Result |
|---|---|
| width `-4` | `std::length_error` → **abort** |
| height `INT32_MIN` | undefined behaviour in `abs()`, then abort |
| 1000000 × 1000000 | 4 TB allocation → abort |
| width `0x20000000` | 4 GB allocation → abort |

On the ESP32, built without exceptions, `std::length_error` is `abort()` — a
panic reset. A truncated upload is a realistic way to get there: the header
lands, the pixel data doesn't.

There's a related resource problem even with a valid header: assets are decoded
**from storage on every frame**, so a large one means a multi-hundred-KB
allocate-and-free 60 times a second, which fragments the heap until something
fails.

**Fix:** reject width ≤ 0, height 0, any dimension over 4096, or more than
512 KB of decoded pixels, and treat a short file as broken (it now shows the
missing-asset marker instead of silently rendering black rows). All seven
hostile headers now return cleanly.

**Also fixed in the UI:** it now caps uploads at 44 KB of BMP regardless of the
"shrink" checkbox — a 1800×1200 photo is resized to 128×85 with a note, rather
than sent to a device that can't hold it.

## 4. `/framebuffer/get` allocates about six times the panel in one go

The old handler held, simultaneously: the raw BGRA copy, the base64 buffer, a
`JsonDocument` with its own copy of that string, and the serialised response.
For a 128×64 panel that's ~200 KB of heap for one request — more than an ESP32
usually has free, and the UI polls this endpoint every 1.2 s. Allocation
failure in `std::vector` is another `abort()`.

**Fix:** snapshot the pixels under the lock, then base64 straight into an
`AsyncResponseStream` in 256-byte chunks. Peak extra memory is now the pixel
copy plus 256 bytes. I checked the new encoder against Python's `base64` for
every length remainder and at the chunk boundary — identical output.

## 5. An aborted upload leaks its whole buffer

`onBody` did `req->_tempObject = new std::vector<uint8_t>()`. The web server's
request destructor does `free(_tempObject)`. If the request completes, the
firmware deletes it properly — but if the client disconnects mid-upload
(browser navigation, Wi-Fi blip, a reload while an asset is going up), the
destructor `free()`s the vector object and **never frees the buffer inside it**.
Repeat that a few times with asset-sized bodies and the heap is gone.

**Fix:** the buffer is plain `malloc`'d memory, so the server's `free()` is
both correct and complete. Bodies over 64 KB are refused with 413 instead of
being accumulated, and out-of-range chunk offsets are dropped.

## 6. A POST with no body never gets an answer

`onBody` registers an empty `onRequest` handler, so a request that arrives with
`Content-Length: 0` runs no body callback and never sends a reply — the client
waits for its timeout. Not a crash, but it looks like a hang.

**Fix:** the `onRequest` handler answers `400 {"error":"empty body"}` when the
content length is zero.

## 7. `{"frames": []}` with `cycles: 0` spins forever

Already reported in `openapi/API-AUDIT.md`; fixed here. `Player::tick` didn't
count a cycle rollover against `kMaxFramesPerTick`, so an animation with
nothing to play looped inside `tick()` until the watchdog fired. An empty
animation is now dropped with a log line, and a completed pass counts against
the per-tick budget so a zero-duration loop yields instead of spinning.

Also fixed: `animname` and `filename` are now validated on all three write
paths (`/file/uploadanim`, `/file/uploadasset`,
`/framebuffer/savetostorage`) — no `/`, no `\`, no `..`, 64 characters max.

---

## What I checked after fixing

- **ThreadSanitizer**: no data races in the render/HTTP race probe.
- **ASan + UBSan fuzzing**: 25,354 malformed frame and animation documents,
  no crashes, no hangs (before: 35 hangs in 225 inputs).
- **Hostile BMP headers**: all seven rejected cleanly.
- **Rendering is unchanged**: the differential test against the Python
  simulator still matches on every pixel — now 33 scenes and 71 frames,
  including four new scenes of hostile geometry, so both implementations are
  compared on the out-of-range cases too.
- **`tools/host-test/run.sh`**: passes.
- **API conformance**: 87/87 against the simulator, which mirrors the new
  bounds.
- **Patch order**: `ui-support.patch` then `crash-fixes.patch` applies cleanly
  to the original zip and reproduces the shipped tree exactly.

## What this does not cover

None of it has run on an ESP32 — these are host reproductions of ESP32
failure modes, and the fixes are host-verified. Finding 1 in particular needs a
real device to confirm it was *your* crash: after flashing, the thing to watch
is whether the panics stop while you're clicking around the UI. If they don't,
the serial log's panic type and backtrace would narrow it down, and
`esp_reset_reason()` is already printed at boot.

One thing I deliberately left alone: **assets are still re-read from storage on
every frame**. A cache is the right fix, but it changes memory behaviour
enough that I didn't want to bundle it with crash fixes. If you see frame-rate
problems with several assets on an SD card, that's the next thing to do.

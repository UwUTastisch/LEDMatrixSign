# LED Matrix Sign 2.0

A ground-up rewrite of the sign firmware around a **compositor** and an
**object-tree animation format**. Instead of pushing whole BMP frames over HTTP
and chaining them by filename, 2.0 keeps a live scene of drawable objects on the
device and renders it every loop, while a small REST API mutates that scene.

This document covers the architecture and the full endpoint reference. For
moving an existing 1.0 device to 2.0, see [`MIGRATION.md`](MIGRATION.md).

> **Build status:** the 2.0 engine is written and internally consistent, and the
> hardware-independent rendering core (color, fonts, framebuffer) has been
> compiled and exercised off-target. It has **not** been compiled against the
> ESP32 Arduino toolchain in this environment (the platform package isn't
> reachable here), so treat the first on-device `pio run` as the real
> integration test. Known approximations are called out in
> [Limitations](#limitations).

## Architecture

```
                      ┌──────────────────────────────────────────┐
   REST API  ───────► │ FrameFactory                             │
   (api_v2.h)         │                                          │
                      │   api Layer      (buffer 0, overlay)     │
                      │   primary Layer  (buffer 1, FS anims)    │
                      │   Player         (timeline state machine)│
                      │                                          │
                      │   render(now): tick player → advance     │
                      │   both layers → composite api OVER       │
                      │   primary → `out` framebuffer            │
                      └───────────────────┬──────────────────────┘
                                          │ out (RGBA)
                                          ▼
                              MatrixDriver::showFrameBuffer  ──►  panel
```

**Two layers, one output.** `primary` plays animations loaded from the
filesystem. `api` is an overlay you draw onto directly over HTTP. Each frame the
factory composites `api` over `primary` into a single `out` buffer and the
driver pushes that to the LEDs.

**Retained mode.** Objects you draw persist until something clears them. A
`clear` directive removes everything (or specific objects by `frame:index`
handle). This is what lets an animation update one object per frame without
redrawing the rest.

**Non-blocking player.** `Player::tick(now)` is a state machine driven by
`millis()` — no `delay()`. It supports nested animations via `load_anim`
(bounded stack depth), per-animation speed scaling that multiplies down the
stack, and a global live speed multiplier from `/anim/setspeed`.

### Source layout

```
src/
  main.cpp              wiring: config → driver → FrameFactory → ApiV2 → loop()
  config.h              FS mount (SD→LittleFS), config.json parsing, Wi-Fi/AP
  matrix_driver.h       WS2812 RMT driver + showFrameBuffer(FrameBuffer)
  storage.h             /anim/<id> paths, load/save anim.json + assets, fs stats
  gfx/
    color.h             Rgba, hex + linear-gradient color specs
    font.h              Font metrics + Fonts::byName registry
    font5x7.h           generated 5x7 glyph table (tools/gen_font.py)
    framebuffer.h       RGBA buffer: blend, line, rect, text, blit, composite
    asset.h             32-bit BGRA8888 BMP loader
  anim/
    composition.h       object tree (text, scrolling_text, line, rectangle,
                        asset, clear, load_anim), parse + serialize
    framefactory.h      Layer, Player, FrameFactory
  api/
    api_v2.h            all REST routes
tools/
    gen_font.py         regenerate gfx/font5x7.h
    bmp_bgra.py         BMP ↔ 32-bit BGRA8888 helper
    migrate_fs.py       1.0 filesystem → 2.0 /anim layout
    validate_anim.py    anim.json schema checker
```

## Filesystem layout

```
/config.json                         hardware + Wi-Fi config (unchanged from 1.0)
/anim/<id>/anim.json                 one animation
/anim/<id>/assets/<file>.bmp         its assets, 32-bit BGRA8888
```

`<id>` is a reverse-DNS-ish handle, e.g. `de.uwutastisch.blahaj`. A worked
example ships in [`data/anim/de.uwutastisch.blahaj/`](data/anim).

## Animation format

An `anim.json` is `default_params` plus an ordered list of `frames`. Each frame
holds one or more drawable objects and/or control directives, and a `duration`
in milliseconds (omitted/0 for pure `clear` and `load_anim` frames).

Objects are addressed **1-based** as `frame:index` (e.g. `f2:1` = first object
of the second frame) for use in `clear`.

### Drawable objects

| type | key fields | notes |
|------|-----------|-------|
| `text` | `x,y,text,font,color` | static string |
| `scrolling_text` | `x,y,dx,dy,text,font,color,scroll_speed,scroll_direction` | scrolls within the `dx`×`dy` viewport; speed in px/s; direction `horizontal`\|`vertical` |
| `line` | `x,y,dx,dy,thickness,color` | `dx,dy` are deltas; endpoint is `(x+dx, y+dy)` |
| `rectangle` | `x,y,dx,dy,border,color` | `border` 0 = filled, else outline thickness |
| `asset` | `x,y,name,color` | draws `assets/<name>`; `color` tints monochrome assets |

`color` is `#rgb`, `#rrggbb`, `#rrggbbaa`, or
`linear-gradient(<deg>deg, <stop>, <stop>, …)`. A malformed color falls back to
white rather than failing.

### Control directives

- `clear`: `{}` clears the whole buffer; `{"obj":["f1:2","f3:1"]}` clears
  specific objects. Duration 0.
- `load_anim`: `{name, params, cycles, speed}` — plays another animation inline.
  Durations in the loaded animation are divided by `speed` (e.g. `speed:2.0`
  halves them). Bounded recursion depth.

### Parameters

`default_params` supplies values for `{placeholder}` tokens in text. `/anim/start`
and `load_anim` can override them. An unresolved placeholder with no default is
left **literal** — substitution never throws.

Validate any `anim.json` before uploading:

```bash
python3 tools/validate_anim.py data/anim/de.uwutastisch.blahaj/
```

## REST API

All bodies are JSON unless noted. Errors return `{"error":"<reason>"}` with a 4xx/5xx code.

### `/framebuffer` — the API overlay layer

| method | path | body / params | effect |
|--------|------|---------------|--------|
| POST | `/framebuffer/draw` | a single frame object (same shape as one `frames[]` entry) | applies it to the API buffer |
| POST | `/framebuffer/savetostorage` | `{animname, filename?}` | saves the current API buffer as a one-frame `anim.json` under `/anim/<animname>/` |
| GET | `/framebuffer/get` | — | `{width,height,format:"BGRA8888",data:<base64>}` of the composited output |
| GET | `/framebuffer/getcomposition` | — | live object trees: `{width,height,primary:[…],api:[…]}` |
| GET | `/framebuffer/size` | — | `{width,height}` |

### `/file` — storage

| method | path | body / params | effect |
|--------|------|---------------|--------|
| POST | `/file/uploadanim` | `{animname, anim:{…}}` | writes `/anim/<animname>/anim.json` |
| POST | `/file/uploadasset` | `{animname, filename, data:<base64 BMP>}` | writes `/anim/<animname>/assets/<filename>` |
| GET | `/file/ls` | — | `{anims:[{id,assets:[…]}]}` |
| GET | `/file/download` | `?path=/anim/<id>/anim.json` | streams the file (restricted to `/anim/…` and `/config.json`) |
| GET | `/file/fsstatus` | — | `{fs,total,used,free}` |

### `/anim` — the primary player

| method | path | body / params | effect |
|--------|------|---------------|--------|
| POST | `/anim/start` | `{animname, params?, cycles?}` | loads + plays `/anim/<animname>/`; `cycles` 0 = loop forever |
| POST | `/anim/setspeed` | `{speed}` | global live speed multiplier |
| POST | `/anim/stop` | — | stops and clears the primary layer |
| GET | `/anim/status` | — | `{running,paused,speed,animname}` |

### `/display`

| method | path | body / params | effect |
|--------|------|---------------|--------|
| POST | `/display/brightness` | `{brightness}` (0–255) | sets brightness |
| GET | `/display/brightness` | — | `{brightness}` |
| GET | `/display/gpiopins` | — | `{data_pin, sd:{cs,mosi,miso,sck}}` |

### Example session

```bash
# upload an animation + its asset
curl -X POST sign.local/file/uploadanim \
  -d '{"animname":"de.uwutastisch.hello","anim":{"frames":[{"text":{"x":0,"y":0,"text":"hi"},"duration":1000}]}}'

# play it, looping, at 1.5x
curl -X POST sign.local/anim/start    -d '{"animname":"de.uwutastisch.hello","cycles":0}'
curl -X POST sign.local/anim/setspeed -d '{"speed":1.5}'

# draw a one-off overlay on top
curl -X POST sign.local/framebuffer/draw \
  -d '{"rectangle":{"x":0,"y":0,"dx":16,"dy":16,"border":1,"color":"#ff0000"}}'
```

## Endpoints removed from 1.0

`/api/img`, `/api/listimg`, `/api/imgchain`, `/api/imgspec`, `/api/display`,
`/api/fssource`, and the bundled web UI (`/index.html`). Their roles are covered
by `/file/*`, `/framebuffer/*`, and `/anim/*`. See [`MIGRATION.md`](MIGRATION.md).

## Limitations

- **Not yet compiled on-target.** The rendering core compiles and runs
  off-device; the Arduino/ESP32 build hasn't been exercised here. Expect to
  shake out include/library details on the first `pio run`.
- **Fonts.** Only `5x7` is a real glyph set (`tools/gen_font.py`). `10x14` and
  `12x16_serif` are currently 2×-scaled renderings of 5x7 with adjusted spacing —
  placeholders until proper glyph tables are authored. Unknown font names fall
  back to `5x7`.
- **`savetostorage` semantics.** The spec was ambiguous about exactly what gets
  persisted; this build serializes the current API-buffer composition as a
  single-frame `anim.json`. Adjust `registerFramebuffer()` if you intended
  something else.
- Assets are uncompressed 32-bit BGRA8888 BMP only — no PNG decode on this path.

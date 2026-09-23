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
/config.json                         hardware + Wi-Fi config (WLED-inspired; 1.0 files still work)
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

| type             | key fields                                                | notes                                                                                                                                                                       |
| ---------------- | --------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `text`           | `x,y,text,font,color`                                     | static string                                                                                                                                                               |
| `scrolling_text` | `x,y,dx,dy,text,font,color,scroll_speed,scroll_direction` | scrolls within the `dx`×`dy` viewport; speed in px/s; direction `horizontal`\|`vertical`                                                                                    |
| `line`           | `x,y,dx,dy,thickness,color`                               | `dx,dy` are deltas; endpoint is `(x+dx, y+dy)`                                                                                                                              |
| `rectangle`      | `x,y,dx,dy,border,color`                                  | `border` 0 = filled, else outline thickness                                                                                                                                 |
| `asset`          | `x,y,name,color`                                          | draws `assets/<name>`, or `<animid>/<file>` for another animation's asset (required on the API overlay, which has no animation of its own); `color` tints monochrome assets |

Every drawable also takes `objname` (a handle for `clear`) and `visible`
(`true`/`false`, or `"{param}"`, optionally negated with `!`) — an object
whose condition is false is skipped entirely.

`{param}` placeholders work in `text`, in `color` and in an asset's `name`.
Parameter values keep their JSON type: `7` renders as `7`, `1.5` as `1.5`,
booleans as `true`/`false`, and booleans are what `visible` tests.

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

| method | path                          | body / params                                              | effect                                                                            |
| ------ | ----------------------------- | ---------------------------------------------------------- | --------------------------------------------------------------------------------- |
| POST   | `/framebuffer/draw`           | a single frame object (same shape as one `frames[]` entry) | applies it to the API buffer                                                      |
| POST   | `/framebuffer/savetostorage`  | `{animname, filename?}`                                    | saves the current API buffer as a one-frame `anim.json` under `/anim/<animname>/` |
| GET    | `/framebuffer/get`            | —                                                          | `{width,height,format:"BGRA8888",data:<base64>}` of the composited output         |
| GET    | `/framebuffer/getcomposition` | —                                                          | live object trees: `{width,height,primary:[…],api:[…]}`                           |
| GET    | `/framebuffer/size`           | —                                                          | `{width,height}`                                                                  |

### `/file` — storage

| method | path                | body / params                             | effect                                                        |
| ------ | ------------------- | ----------------------------------------- | ------------------------------------------------------------- |
| POST   | `/file/uploadanim`  | `{animname, anim:{…}}`                    | writes `/anim/<animname>/anim.json`                           |
| POST   | `/file/uploadasset` | `{animname, filename, data:<base64 BMP>}` | writes `/anim/<animname>/assets/<filename>`                   |
| GET    | `/file/ls`          | —                                         | `{anims:[{id,assets:[…]}]}`                                   |
| GET    | `/file/download`    | `?path=/anim/<id>/anim.json`              | streams the file (restricted to `/anim/…` and `/config.json`) |
| GET    | `/file/fsstatus`    | —                                         | `{fs,total,used,free}`                                        |

### `/anim` — the primary player

| method | path             | body / params                  | effect                                                        |
| ------ | ---------------- | ------------------------------ | ------------------------------------------------------------- |
| POST   | `/anim/start`    | `{animname, params?, cycles?}` | loads + plays `/anim/<animname>/`; `cycles` 0 = loop forever  |
| POST   | `/anim/setspeed` | `{speed}`                      | global live speed multiplier                                  |
| POST   | `/anim/pause`    | —                              | hold the timeline; the current frame keeps its remaining time |
| POST   | `/anim/resume`   | —                              | continue a paused animation                                   |
| POST   | `/anim/stop`     | —                              | stops and clears the primary layer                            |
| GET    | `/anim/status`   | —                              | `{running,paused,speed,animname}`                             |

### `/display`

| method | path                  | body / params          | effect                               |
| ------ | --------------------- | ---------------------- | ------------------------------------ |
| POST   | `/display/brightness` | `{brightness}` (0–255) | sets brightness                      |
| GET    | `/display/brightness` | —                      | `{brightness, applied, pwr, maxpwr}` |

Brightness and the power limit are configured in `config.json`, see
[Brightness & power](#brightness--power-configjson) below.

#### Brightness & power (`config.json`)

These settings follow [WLED](https://github.com/wled/WLED): the same keys,
the same meaning and the same current model, so a WLED `cfg.json` (or the
values from WLED's _LED Preferences_ page) can be copied over unchanged.
All keys are optional.

| Key                    | Meaning                                                                                                     | Default |
| ---------------------- | ----------------------------------------------------------------------------------------------------------- | ------- |
| `hw.led.maxpwr`        | current budget in mA for LEDs + ESP32; each frame is dimmed just enough to stay under it. `0` = limiter off | `0`     |
| `hw.led.ins[0].maxpwr` | per-output budget, used when `hw.led.maxpwr` is `0`                                                         | `0`     |
| `hw.led.ins[0].ledma`  | mA per LED at full white (WS2812B ≈ 55)                                                                     | `55`    |
| `light.scale-bri`      | brightness scale in percent, applied to every brightness request                                            | `100`   |
| `def.bri`              | brightness after boot, until `/display/brightness` sets one                                                 | `128`   |

```json
{
  "hw": {
    "led": {
      "total": 4608,
      "maxpwr": 10000,
      "ins": [{ "start": 0, "len": 4608, "pin": [10], "ledma": 55 }]
    }
  },
  "light": { "scale-bri": 100 },
  "def": { "bri": 128 }
}
```

How the limiter works (as in WLED's automatic brightness limiter, "ABL"):
every frame the firmware estimates the draw as
`colour sum × ledma / (3 × 255) + 1 mA standby per LED + 120 mA for the ESP32`,
and if that exceeds `maxpwr` it lowers the brightness of that frame until it
fits. Two differences from WLED: only the colour part is scaled when solving
for the brightness (the standby current doesn't dim), and WLED's special
`ledma: 255` value for WS2815 strips is not supported.

It is an estimate, not a measurement. Set `maxpwr` below what the supply
really delivers (~20 % margin is a common rule of thumb), and note that
every LED draws about 1 mA even when black: with 4608 LEDs that is ~4.6 A
before anything lights up, and a budget below that dims everything to the
minimum. `GET /display/brightness` reports `applied` (the brightness the
last frame actually used) and `pwr` (the estimated draw in mA), like
`info.leds.pwr` in WLED's JSON API.

This is an independent implementation of the same model — no WLED source
code is included (WLED is EUPL-1.2 licensed, this project is MIT).

Reference — where WLED defines this, for comparison:

- config keys: [`wled00/cfg.cpp`](https://github.com/wled/WLED/blob/main/wled00/cfg.cpp)
  (`hw.led.maxpwr`, `ins[].ledma`, `ins[].maxpwr`, `light.scale-bri`, `def.bri`)
- current estimate and limiter: [`wled00/bus_manager.cpp`](https://github.com/wled/WLED/blob/main/wled00/bus_manager.cpp)
  (`BusDigital::estimateCurrent()`, `BusManager::applyABL()`)
- ESP32 share and defaults: [`wled00/bus_manager.h`](https://github.com/wled/WLED/blob/main/wled00/bus_manager.h) (`MA_FOR_ESP`),
  [`wled00/const.h`](https://github.com/wled/WLED/blob/main/wled00/const.h) (`LED_MILLIAMPS_DEFAULT`)
- WLED documentation: [kno.wled.ge](https://kno.wled.ge)

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

`/api/img`, `/api/listimg`, `/api/imgchain`, `/api/imgspec`, `/api/display`
and `/api/fssource`. Their roles are covered by `/file/*`, `/framebuffer/*`
and `/anim/*`. See [`MIGRATION.md`](MIGRATION.md).

The web UI is no longer compiled into the firmware, but it is still served:
drop an `index.html` on the filesystem (one ships in
`example-sd-card-content/`) and `GET /` returns it.

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

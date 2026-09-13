# API documentation audit

What I checked, what was wrong, and what someone building a client or a
simulator needs to know that the old document didn't say.

Reference read line by line: `src/api/api_v2.h`, `src/anim/composition.h`,
`src/anim/framefactory.h`, `src/gfx/{color,font,framebuffer,asset}.h`,
`src/storage.h`.

## Was the old spec valid?

Yes, structurally: `ledmatrixsign-api-2.0.yaml` at version 2.0 passed
`openapi-spec-validator` as OpenAPI 3.0.3. Every one of the 16 routes in the
firmware was present, with no invented ones.

Complete, though, it was not. It described the request bodies and little else:
no error responses anywhere, almost no response bodies, no defaults, and none
of the behaviour a client has to know. Two entries were actively wrong, and
the `Frame` schema rejected documents the firmware accepts and the firmware
itself writes.

## Wrong

1. **`Frame` had `additionalProperties: false` and "zero-or-one of each
   drawable key".** The parser also accepts a `drawables` array of single-key
   objects, and accepts an array under any drawable key. Both forms are in
   active use — `/framebuffer/savetostorage` writes the `drawables` form — so
   a client validating against the old schema would reject the device's own
   output. `drawables` was missing from the schema entirely.

2. **`ClearDirective` documented an `all: boolean` field.** The firmware never
   reads it. It decides by whether `obj` is present, so `{"all": false}`
   clears the whole layer — the opposite of what the field suggests. Now
   documented as the quirk it is, and covered by a conformance check.

3. **The server default was `192.168.4.1`.** `src/config.h` puts the access
   point on `4.3.2.1`.

## Missing

Documented now, all of it verified against the source:

- **The rendering model.** Two retained layers, overlay above animation, and
  the fact that drawing is not a blit: objects persist until cleared. Nothing
  in the old document said this, and it's the first thing a client gets wrong.
- **Object ids and `clear` references.** Ids are `<frame>:<index>`, frame 0
  for the overlay, and `clear.obj` takes either an `objname` or the id with an
  `f` prefix (`f0:2`). Unnamed objects from one request share a handle and are
  cleared together.
- **No partial update, and no way to read geometry back.**
  `/framebuffer/getcomposition` returns type, id and objname only, so a client
  that wants to edit an object must remember what it drew.
- **Duplicate suppression.** A structurally identical drawable (including
  `objname`) is dropped instead of stacked, which is why re-applying a frame
  is idempotent.
- **Error responses.** Every `{"error": "..."}` case with its status: 400 on
  bad JSON or missing fields, 404 for an unknown animation or file, 403 for a
  path outside `/anim` and `/config.json`, 500 on a write failure.
- **Success bodies.** `{status, animname}`, `{status, bytes}`, `{status, path}`,
  `{status, speed}`, `{status, brightness}` — none were documented.
- **`/framebuffer/get` byte layout.** Base64 of `width*height*4` bytes,
  row-major from the top-left, **B, G, R, A** per pixel. Also that brightness
  is not applied to it.
- **`/anim/status.animname` is a directory**, `/anim/hello`, not the id passed
  to `/anim/start`, and it keeps its value after an animation ends by itself —
  so `running` has to be checked too. During a nested animation it still names
  the outermost one. `paused` is always false: the player supports pausing but
  no endpoint reaches it.
- **Defaults and clamping.** `cycles` 0 means forever; `load_anim.cycles` 0
  means once, not forever; `speed` ≤ 0 becomes 1.0; brightness clamps to
  0–255 and defaults to 255 when absent; `duration` defaults to 0.
- **Colour and font grammar**, including that both fail silently — an
  unparseable colour renders opaque white, an unknown font falls back to 5x7.
- **Asset naming and format.** The two name forms, that the file must be a
  32-bit BGRA8888 BMP, that nothing validates it on upload, and that a bad or
  missing file draws a 4×4 purple-and-black marker.
- **Parameter substitution**, including that unresolved placeholders stay in
  the text literally and that parameters never apply to the overlay layer.
- **`load_anim` semantics**: layer cleared first, parent resumes afterwards,
  8-level nesting cap, `duration` not waited out, `speed` multiplies.
- **That playback is driven by rendering.** On a headless device the timeline
  advances when `/framebuffer/get` or `/anim/status` is polled.
- **No auth, no rate limiting, no CSRF protection.**
- **`GET /` serves `index.html`** from storage — the only non-API route.

## Firmware issues found while auditing

1. **`{"frames": []}` with `cycles: 0` hangs the device.** `Player::tick`
   doesn't count a cycle rollover against `kMaxFramesPerTick`, so with no
   frames to consume the `while` loop never terminates and the watchdog
   resets the board. Reproduced on the host: the process has to be killed. `/file/uploadanim` accepts such a document
   happily. A one-line guard (pop the context when `frames` is empty, or count
   the rollover) would fix it.

2. **`animname` and `filename` are never validated.** `Storage::saveAsset`
   concatenates them into a path, so `/file/uploadasset` and
   `/file/uploadanim` can address anything the filesystem allows — including
   `..` segments. `/file/download` *is* guarded, so this is write-only, but on
   a device with no auth on the API it's still worth a check.

3. **The missing-dependency marker in `load_anim` never appears.** `tick()`
   draws it into the layer's framebuffer, then `rasterize()` clears that
   framebuffer before the panel sees it. Harmless, but the documented
   behaviour ("leave a marker top-left") doesn't happen; a failed `load_anim`
   is silent. The spec now says what actually happens.

4. **Assets are re-read from storage every frame.** No cache in
   `AssetObj::render`. Fine for small assets on LittleFS; on an SD card with
   several assets on screen it will cost frame rate.

5. **`paused` is dead weight in the API.** `Player::pause()`/`resume()` exist
   and `/anim/status` reports the flag, but nothing can set it. Either add
   `/anim/pause` and `/anim/resume` or drop the field.

Items 1 and 2 are the ones worth fixing before other people point clients at
this. Neither is in the `ui-support` patch, since both are firmware-behaviour
changes rather than things the UI needs.

## How the new document was checked

- `openapi-spec-validator` accepts it as OpenAPI 3.0.3 (16 paths, 17
  operations, 19 schemas).
- The frontend project's `dev/conformance.py` exercises every documented
  endpoint and behavioural claim against a running implementation, and
  validates the responses against the schemas in the spec. 87 checks, all
  passing against the simulator.
- Its `dev/diff_firmware.py` renders 33 scenes through this firmware's
  compositor (compiled on the host) and through the simulator and compares
  every pixel: 71 frames, all identical.

The conformance suite is the honest way to confirm the document against real
hardware — it was written to run against either, and the parts that only read
are available with `--read-only`. It hasn't been run on a device here.

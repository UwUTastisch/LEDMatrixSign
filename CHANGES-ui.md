# Changes in this drop

**Start with [CRASH-FIXES.md](CRASH-FIXES.md)** if you're here about the
crashes: seven bugs, one of which (no locking between the HTTP task and the
render loop) explains random panics while using the UI. `crash-fixes.patch`
holds those changes; apply it after `ui-support.patch`.

Based on the `LEDMatrixSign-fix.zip` you sent (src dated 2026-08-23), not on
the `feature/2.0-compositor` branch — see the warning at the bottom.

## src/anim/composition.h — asset cross-referencing

`AssetObj` resolved every name against the drawing animation's own directory,
so the API overlay (which has no animation, `animDir` is empty) looked for
`/assets/<name>`. Nothing ever writes there: uploads land in
`/anim/<id>/assets/`. Every asset drawn through `/framebuffer/draw` therefore
rendered as the purple/black missing marker, whatever name was used.

New `resolveAssetPath()` handles both forms:

    "file.bmp"           -> <animDir>/assets/file.bmp       (unchanged)
    "<animid>/file.bmp"  -> /anim/<animid>/assets/file.bmp  (new)

Existing `anim.json` files keep working. The second form is what lets the
overlay reach an asset at all, and also lets one animation reuse another's
assets.

## src/anim/composition.h — objname on rectangles and assets

`RectObj::toJson`/`equals` and `AssetObj::toJson`/`equals` ignored `objname`.
Two consequences: saving the overlay lost the names, and because
`Layer::applyDrawables` skips structurally equal objects, two identical
rectangles with different names collapsed into one — so clearing one by name
did nothing.

## src/main.cpp — CORS header (optional)

One line, only needed if the UI is opened from a local file or hosted
elsewhere. Irrelevant when the sign serves `/index.html` itself.

## tools/validate_anim.py — caught up with the parser

The validator was behind `parseFrameObject()` and reported false errors on
files the firmware accepts:

- `"drawables": [...]` was flagged as an unknown key and its contents went
  unchecked. It now validates the list, and warns when `clear`/`load_anim`
  sit next to it, since the firmware ignores them there.
- `"rectangle": [{...}, {...}]` (array per type) was an error. Now accepted.
- `clear.obj` demanded `frame:index` strings; objnames are valid too.
- asset presence now follows `resolveAssetPath()`, so `<animid>/<file>`
  references are checked in the right animation folder.

## example-sd-card-content/index.html

Replaced with the new web UI (single self-contained file), which now also has
a select-and-move tool: every overlay object drawn from the page gets a
hitbox and can be dragged, nudged with the arrow keys, duplicated or deleted. `data/` is a
symlink to this folder, so `pio run --target uploadfs` picks it up.

## tools/host-test/

New: compiles the compositor on your computer and checks the above, with no
ESP32 involved. `run.sh --probes` additionally builds the crash probes
(ThreadSanitizer race check, hostile BMP headers, render-time measurement, and
an ASan fuzzer) described in CRASH-FIXES.md. `render_dump.cpp` additionally renders scene files through the
real compositor, which is how the Python simulator next door is verified
pixel for pixel. See its README.

## openapi/

New: the API specification (OpenAPI 3.0.3) and `API-AUDIT.md`. The audit found
two firmware issues worth fixing, neither included in this patch because both
change device behaviour rather than anything the UI needs:

- An animation document with `"frames": []` played with `cycles: 0` hangs the
  render loop — `Player::tick` doesn't count a cycle rollover against
  `kMaxFramesPerTick`, so the loop never exits and the watchdog resets the
  board. `/file/uploadanim` accepts such a document without complaint.
- `animname` and `filename` from `/file/uploadanim`, `/file/uploadasset` and
  `/framebuffer/savetostorage` are concatenated into filesystem paths with no
  validation, so they can contain `..`. Reads are guarded; writes are not.

Three more, noted but harmless: the missing-dependency marker for a failed
`load_anim` is drawn and then overwritten before it reaches the panel; assets
are re-read from storage on every frame; and `paused` is reported by
`/anim/status` but no endpoint can set it.

---

## Note on the upstream branch

`feature/2.0-compositor` (cc2099a, 2026-09-09) is **behind** the zip you sent.
Upstream's `composition.h` has no `drawables` array, no `equals()`, no
`objname` serialisation, and `color.h` has no `ColorSpec::toString()`; the
zip has all of it. So the patch does not apply to a fresh clone — three of
four hunks fail — and re-cloning would undo that work.

This folder is the zip plus the changes above. If you want the changes on
upstream instead, push the zip's state first.

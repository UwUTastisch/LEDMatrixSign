# Migrating from 1.0 to 2.0

2.0 changes both the on-device filesystem layout and the HTTP API. This guide
covers moving your data and updating any clients.

## TL;DR

```bash
# 1. copy the device filesystem to a local folder (however you normally pull it)
#    you should end up with: littlefs_dump/{config.json,images/,imgchain/,...}

# 2. convert it to the 2.0 layout
python3 tools/migrate_fs.py littlefs_dump/ out_fs/ --manifest report.json

# 3. inspect what it produced
cat report.json
python3 tools/validate_anim.py out_fs/anim/<some-id>/

# 4. upload out_fs/ to the device (uploadfs, or copy onto the SD card)
```

## Filesystem changes

| 1.0 | 2.0 |
|-----|-----|
| `/config.json` | `/config.json` (unchanged — copied verbatim) |
| `/images/<name>.bmp` (24-bit) | `/anim/<id>/assets/<name>.bmp` (32-bit BGRA8888) |
| `/imgchain/<n>.chain` | `/anim/<id>/anim.json` |
| `/index.html`, `/favicon.ico` | dropped (no bundled UI) |

### What `migrate_fs.py` does

- **Each `.chain`** becomes one animation `de.uwutastisch.chain<n>` whose
  `anim.json` plays the chained images as a slideshow (a `clear` + `asset` pair
  per frame, so each slide starts clean and loops cleanly). The chain's frame
  duration is preserved.
- **Each referenced image** is converted from 24-bit BMP to the 2.0
  32-bit BGRA8888 asset format and written into that animation's `assets/`.
- **Loose images** not used by any chain are each wrapped in a one-frame static
  animation `de.uwutastisch.<name>` (disable with `--no-orphans`).
- **`config.json`** is copied unchanged.
- Missing or unreadable images are reported; the firmware draws a 4×4
  purple/black checker in their place at runtime, so the animation still plays.

Options:

```
--prefix <ns>     reverse-DNS namespace for generated ids (default de.uwutastisch)
--no-orphans      skip wrapping un-chained images as static animations
--manifest PATH   also write the migration report as JSON
```

The converter (`bmp_bgra.py`) is dependency-free and also usable standalone:

```bash
python3 tools/bmp_bgra.py old.bmp new_bgra8888.bmp
```

## API changes

The 1.0 image/chain endpoints are gone. Map old calls to new ones:

| 1.0 | 2.0 |
|-----|-----|
| `POST /api/img` (upload BMP) | `POST /file/uploadasset` `{animname,filename,data}` |
| `GET /api/img?file=` | `GET /file/download?path=/anim/<id>/assets/<file>` |
| `GET /api/listimg` | `GET /file/ls` |
| `POST /api/imgchain` | `POST /file/uploadanim` `{animname,anim}` then `POST /anim/start` |
| `GET /api/imgchain?num=` | `GET /file/download?path=/anim/<id>/anim.json` |
| `GET /api/imgspec` | assets are 32-bit BGRA8888 BMP; see README |
| `POST /api/display` (push raw frame) | `POST /framebuffer/draw` (object) |
| `POST /api/brightness` | `POST /display/brightness` |
| `GET /api/fssource` | `GET /file/fsstatus` (`fs` field) |

The biggest conceptual shift: 1.0 pushed **pixels** (whole BMP frames); 2.0
pushes **objects** (text, shapes, asset references) into a retained scene. A
client that used to render a frame and POST it can instead POST a small JSON
description and let the device render it — or, to keep a pixel-pushing workflow,
upload a BMP asset once and reference it from a one-object `anim.json`.

## Suggested rollout

1. Convert and validate your filesystem locally (steps above) — no device needed.
2. Flash the 2.0 firmware and upload the converted `out_fs/`.
3. `GET /file/ls` to confirm the animations and assets are present.
4. `POST /anim/start {"animname":"<id>","cycles":0}` to play one.
5. Update clients to the new endpoints using the table above.

## Notes / caveats

- Generated animation ids are derived from chain numbers and image names; rename
  them in `out_fs/anim/` before uploading if you want friendlier handles.
- `migrate_fs.py` reads a **local copy** of the filesystem; it doesn't talk to
  the device. Pull the FS off the device first (or work from your source
  `data/` folder).
- Fonts other than `5x7` are placeholders in this build (see README
  Limitations), but migration only emits `asset` slideshows, so text fonts don't
  affect migrated content.

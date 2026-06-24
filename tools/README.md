# tools/

Host-side helpers for LED Matrix Sign 2.0. All are plain Python 3 (3.8+) with
**no third-party dependencies**.

| script | purpose |
|--------|---------|
| `migrate_fs.py` | Convert a 1.0 device filesystem to the 2.0 `/anim` layout. See [`../MIGRATION.md`](../MIGRATION.md). |
| `bmp_bgra.py` | Read 24/32-bit BMPs and write the 32-bit BGRA8888 BMP that `src/gfx/asset.h` expects. Importable library + CLI. |
| `validate_anim.py` | Check an `anim.json` (or an `anim/<id>/` folder) against the schema the firmware parses. |
| `gen_font.py` | Regenerate `src/gfx/font5x7.h` from the glyph grids defined inside the script. |

## Common commands

```bash
# migrate a pulled filesystem
python3 migrate_fs.py littlefs_dump/ out_fs/ --manifest report.json

# convert a single image to the asset format
python3 bmp_bgra.py logo.bmp logo_bgra8888.bmp

# validate an animation before uploading
python3 validate_anim.py ../data/anim/de.uwutastisch.blahaj/

# regenerate the 5x7 font header after editing glyphs in gen_font.py
# (writes ../src/gfx/font5x7.h directly)
python3 gen_font.py
```

`migrate_fs.py` imports `bmp_bgra.py`, so keep them in the same folder.

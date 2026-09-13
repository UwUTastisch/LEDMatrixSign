# example-sd-card-content

Copy the contents of this folder to the root of an SD card, or flash it as the
LittleFS image (`data` symlinks here, so `pio run --target uploadfs` picks it
up).

    anim/<id>/anim.json        an animation
    anim/<id>/assets/*.bmp     its assets, 32-bit BGRA8888
    config.json                symlink to example-configs/
    index.html                 the web UI

**`index.html` is generated — don't edit it here.** It is the single-file build
of the frontend project (`python3 build.py` there, which inlines its `src/`
into one file). Edits made in this copy are lost on the next build.

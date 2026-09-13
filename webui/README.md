# webui

Sources for the control panel the firmware serves at `http://<sign>/`. The
build inlines them into the single self-contained `index.html` that ships in
`example-sd-card-content/`.

```bash
python3 webui/build.py --install     # rebuild the copy the device ships
python3 webui/build.py               # -> webui/dist/index.html (scratch)
python3 webui/build.py -o /media/SD/index.html
```

Python 3 standard library only — no npm, no bundler, no minifier.

## Layout

    src/index.html    the page: layout, tabs, tool panels, dialogs
    src/style.css     all styling
    src/app.js        everything else: API client, canvas editor, renderer
    build.py          swaps the <link> and <script> tags for inline blocks

That is the entire build. The output is the input with two files pasted in, so
it stays readable in devtools and `git diff` on `src/` shows real changes.

**Don't edit `example-sd-card-content/index.html`** — it is generated, and the
next `--install` overwrites it.

## Notes

- `src/app.js` carries a copy of the 5x7 glyph table so the canvas preview
  matches the panel. After changing the font, re-sync it with
  `python3 webui/build.py --font src/gfx/font5x7.h --install`.
- The page uses relative URLs, so a copy served by the sign needs no
  configuration. Opened from disk it defaults to `http://4.3.2.1` and can be
  pointed elsewhere in Settings or with `?api=http://…`, which relies on the
  `Access-Control-Allow-Origin` header the firmware sends.
- Asset names are sent as `<animid>/<file>`, the form `resolveAssetPath()` in
  `src/anim/composition.h` resolves from any layer.
- The simulator, the API conformance suite and the pixel-for-pixel comparison
  against this firmware's compositor live in the separate frontend project;
  none of them are needed to build or ship the page.

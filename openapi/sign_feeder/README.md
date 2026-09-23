# Sign feeder

Feeds a LEDMatrixSign 2.0 from small, independent packages.

```
pip install -r requirements.txt
python3 main.py --list      # show the playlist
python3 main.py --dry-run   # run without touching the sign
python3 main.py             # run
```

## How a round works

1. **Upload**: each package's `anim.json` + `assets/` go to the sign once at
   start (retried every round until it works; `UPLOAD_ON_START=false` to skip).
2. **Preload**: every slide's `preload(ctx)` runs at the same time — all API
   calls happen here. A package that fails or takes longer than
   `PRELOAD_TIMEOUT` is skipped for that round; the others carry on.
3. **Display**: slide after slide, the animation starts with the preloaded
   params and stays up for `DURATION` seconds.

## A package

A folder in `packages/`, named like the animation on the sign:

```
packages/de.hackhro.project/
  skript.py    required: preload(ctx) — optional: display(ctx, params), register(add, env)
  .env         this package's settings, on top of the global .env
  anim.json    optional, uploaded to /anim/de.hackhro.project/
  assets/*     optional, uploaded next to it
```

Start from `packages/_template/`: copy it, drop the leading `_`, edit.

`preload(ctx)` returns what to show: `None` skips the slide this round, a dict
is one slide with those params, a list of dicts is one slide each.
It can be a plain function (it runs in a thread) or `async def`.

`ctx` gives you `name`, `label`, `env`, `args`, `http` (requests session),
`sign` (`start_anim`, `brightness`, `post`) and `log`.

### Settings every package understands (`.env`)

| Key | Meaning | Default |
|---|---|---|
| `DURATION` | seconds to hold each slide; `0` = side effect only | `10` |
| `CACHE_SECONDS` | reuse the last preload result for this long | `0` |
| `ENABLED` | `false` skips the package | `true` |
| `INSTANCES` | JSON list → one slide per entry, keys land in `ctx.args` (`label`, `duration` optional) | one slide |

The same package twice with different parameters: two `INSTANCES` entries
(see `de.hackhro.rsag/.env`), or a `register(add, env)` function in
`skript.py` that calls `add(...)` twice.

### Global `.env`

`DEVICE_HOST`, `ORDER` (package names first in the playlist, the rest follows
alphabetically), `PRELOAD_TIMEOUT`, `IDLE_SECONDS`, `UPLOAD_ON_START`.

## Included packages

| Package | Shows |
|---|---|
| `de.hackhro.brightness` | nothing — sets the brightness along the sun's curve |
| `de.hackhro.rsag` | departures, one slide per station |
| `de.hackhro.open` | the open animation while `switch_state` is `2` |
| `de.hackhro.event` | one slide per upcoming event from an iCal feed (set `EVENTS_URL`) |

`de.hackhro.open` and `de.hackhro.event` have no `anim.json` here — add
yours to the package folder and it gets uploaded on start.

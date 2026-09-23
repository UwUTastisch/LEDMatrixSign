"""The main loop. Every round has three stages:

  0. upload   anim.json + assets of packages not yet on the sign (retried)
  1. preload  all slides at once, concurrently — the API calls
  2. display  slide after slide: start the anim, hold for its duration

preload(ctx) returns what to show:
  None          skip this slide this round
  {...}         one slide with these params
  [{...}, ...]  several slides (e.g. one per event), each held for `duration`
"""
import asyncio
import time
from dataclasses import dataclass

import requests

from .packages import is_async


@dataclass
class Ctx:
    """Everything a skript gets handed."""
    name: str       # package = animation name on the sign
    label: str      # this instance's label
    env: dict       # the package's .env (with the global .env underneath)
    args: dict      # this instance's args from register / INSTANCES
    http: requests.Session
    sign: object    # feeder.sign.Sign
    log: object     # log(message)


async def _call(fn, *args):
    """Skripts may be plain functions (run in a thread) or async."""
    if is_async(fn):
        return await fn(*args)
    return await asyncio.to_thread(fn, *args)


def _as_list(result):
    if result is None:
        return []
    return result if isinstance(result, list) else [result]


class Runner:
    def __init__(self, playlist, packages, sign, env, log=print):
        self.playlist = playlist
        self.packages = packages
        self.sign = sign
        self.log = log
        self.http = requests.Session()
        self.preload_timeout = env.float("PRELOAD_TIMEOUT", 15)
        self.idle_seconds = env.float("IDLE_SECONDS", 10)
        self.upload = env.bool("UPLOAD_ON_START", True)

    def _ctx(self, slide):
        pkg = slide.package
        return Ctx(pkg.name, slide.label, pkg.env, slide.args, self.http, self.sign,
                   lambda msg, _l=slide.label: self.log(f"[{_l}] {msg}"))

    # -- stage 0 ---------------------------------------------------------
    async def upload_stage(self):
        if not self.upload:
            return
        for pkg in self.packages:
            if pkg.uploaded or not (pkg.anim_path or pkg.asset_paths):
                continue
            try:
                await asyncio.to_thread(pkg.upload, self.sign)
                self.log(f"⬆️ {pkg.name}: anim.json + {len(pkg.asset_paths)} assets uploaded")
            except Exception as e:
                self.log(f"⚠️ {pkg.name}: upload failed, retrying next round — {e}")

    # -- stage 1 ---------------------------------------------------------
    async def _preload(self, slide):
        now = time.monotonic()
        if slide.cache_seconds and slide.cached_at and now - slide.cached_at < slide.cache_seconds:
            return slide.cached
        try:
            result = await asyncio.wait_for(_call(slide.package.module.preload, self._ctx(slide)),
                                            self.preload_timeout)
        except asyncio.TimeoutError:
            self.log(f"⚠️ {slide.label}: preload timed out")
            return None
        except Exception as e:  # one broken source must not stop the sign
            self.log(f"⚠️ {slide.label}: preload failed — {e}")
            return None
        slide.cached, slide.cached_at = result, now
        return result

    async def preload_stage(self):
        return await asyncio.gather(*(self._preload(s) for s in self.playlist))

    # -- stage 2 ---------------------------------------------------------
    async def _display(self, slide, params):
        ctx = self._ctx(slide)
        display = getattr(slide.package.module, "display", None)
        try:
            if display:
                await _call(display, ctx, params)
            else:
                await asyncio.to_thread(self.sign.start_anim, ctx.name, params)
            return True
        except Exception as e:
            self.log(f"⚠️ {slide.label}: display failed — {e}")
            return False

    async def display_stage(self, results):
        shown = 0
        for slide, result in zip(self.playlist, results):
            for params in _as_list(result):
                if await self._display(slide, params) and slide.duration > 0:
                    self.log(f"▶ {slide.label} ({slide.duration:g}s)")
                    shown += 1
                    await asyncio.sleep(slide.duration)
        return shown

    # -- loop ------------------------------------------------------------
    async def round(self):
        await self.upload_stage()
        results = await self.preload_stage()
        return await self.display_stage(results)

    async def run(self, once=False):
        while True:
            shown = await self.round()
            if once:
                return
            if shown == 0:
                # sign or every source unreachable — don't spin
                await asyncio.sleep(self.idle_seconds)

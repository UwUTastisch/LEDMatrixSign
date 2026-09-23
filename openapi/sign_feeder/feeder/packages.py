"""Finding packages, loading their skript.py and registering slides.

A package is a folder under packages/, named like the animation on the sign:

    packages/de.hackhro.example/
        skript.py     preload(ctx) [, display(ctx, params)] [, register(add, env)]
        .env          settings for this package (inherits the global .env)
        anim.json     optional — uploaded to /anim/<folder name>/ on start
        assets/*      optional — uploaded next to it

Folders starting with "_" or "." are ignored (e.g. packages/_template).
"""
import importlib.util
import inspect
import json
import os
from dataclasses import dataclass, field

from .env import load_env


@dataclass
class Package:
    name: str
    dir: str
    env: dict
    module: object
    uploaded: bool = False

    @property
    def anim_path(self):
        p = os.path.join(self.dir, "anim.json")
        return p if os.path.exists(p) else None

    @property
    def asset_paths(self):
        d = os.path.join(self.dir, "assets")
        if not os.path.isdir(d):
            return []
        return sorted(os.path.join(d, f) for f in os.listdir(d)
                      if os.path.isfile(os.path.join(d, f)) and not f.startswith("."))

    def upload(self, sign):
        """Push anim.json + assets to the sign."""
        if self.anim_path:
            with open(self.anim_path, encoding="utf-8") as f:
                sign.upload_anim(self.name, json.load(f))
        for path in self.asset_paths:
            with open(path, "rb") as f:
                sign.upload_asset(self.name, os.path.basename(path), f.read())
        self.uploaded = True


@dataclass
class Slide:
    """One registered instance of a package — what the playlist is made of."""
    package: Package
    label: str
    args: dict = field(default_factory=dict)
    duration: float = 10
    cache_seconds: float = 0
    # runtime state
    cached: object = None
    cached_at: float = 0


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(f"pkg_{name.replace('.', '_').replace('-', '_')}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if not hasattr(mod, "preload"):
        raise AttributeError(f"{path} has no preload(ctx)")
    return mod


def load_packages(root, global_env, log=print):
    packages = []
    for entry in sorted(os.listdir(root)):
        pdir = os.path.join(root, entry)
        skript = os.path.join(pdir, "skript.py")
        if entry.startswith(("_", ".")) or not os.path.isfile(skript):
            continue
        env = load_env(os.path.join(pdir, ".env"), parent=global_env)
        if not env.bool("ENABLED", True):
            log(f"  {entry}: disabled")
            continue
        try:
            packages.append(Package(entry, pdir, env, _load_module(entry, skript)))
        except Exception as e:  # a broken package must not stop the others
            log(f"⚠️ {entry}: not loaded — {e}")
    return packages


def register_slides(pkg):
    """Instances of one package, in this order of preference:
    1. skript.register(add, env) calls add(...) as often as it likes
    2. INSTANCES in .env: a JSON list, one slide per object
    3. otherwise: one slide without args
    Each instance may set "label" and "duration" next to its own args."""
    slides = []
    default_duration = pkg.env.float("DURATION", 10)
    cache = pkg.env.float("CACHE_SECONDS", 0)

    def add(label=None, duration=None, **args):
        slides.append(Slide(
            package=pkg,
            label=str(label or args.get("name") or pkg.name),
            args=args,
            duration=default_duration if duration is None else float(duration),
            cache_seconds=cache,
        ))

    register = getattr(pkg.module, "register", None)
    if register:
        register(add, pkg.env)
    elif pkg.env.get("INSTANCES"):
        for inst in pkg.env.json("INSTANCES"):
            add(**inst)
    else:
        add()
    return slides


def build_playlist(packages, order, log=print):
    """ORDER from the global .env first, then everything else alphabetically."""
    by_name = {p.name: p for p in packages}
    ordered = [by_name[n] for n in order if n in by_name]
    ordered += [p for p in packages if p.name not in order]
    playlist = []
    for pkg in ordered:
        try:
            playlist += register_slides(pkg)
        except Exception as e:
            log(f"⚠️ {pkg.name}: register failed — {e}")
    return playlist


def is_async(fn):
    return inspect.iscoroutinefunction(fn)

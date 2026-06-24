#!/usr/bin/env python3
"""
validate_anim.py — sanity-check a 2.0 anim.json against the schema the firmware
understands (see src/anim/composition.h).

The firmware is deliberately lenient: unknown keys are ignored, missing fields
fall back to defaults, and unresolved "{param}" placeholders are left literal.
This validator surfaces the things that would still bite you — unknown object
types, malformed colors, missing asset names, frames without a duration, and
asset files that aren't present on disk.

    python3 validate_anim.py path/to/anim.json
    python3 validate_anim.py path/to/anim/<id>/      # validates anim.json + assets

Exit code 0 = no errors (warnings allowed), 1 = errors found, 2 = bad usage.
"""
from __future__ import annotations

import json
import os
import re
import sys
from typing import List, Tuple

DRAWABLE_TYPES = {"text", "scrolling_text", "line", "rectangle", "asset"}
CONTROL_TYPES = {"clear", "load_anim"}
KNOWN_FRAME_KEYS = DRAWABLE_TYPES | CONTROL_TYPES | {"duration"}

HEX_RE = re.compile(r"^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")
GRAD_RE = re.compile(r"^linear-gradient\s*\(", re.IGNORECASE)


def is_valid_color(value) -> bool:
    if not isinstance(value, str):
        return False
    return bool(HEX_RE.match(value.strip()) or GRAD_RE.match(value.strip()))


def validate_color(value, where: str, warns: List[str]) -> None:
    if value is None:
        return
    if not is_valid_color(value):
        # firmware falls back to white, so this is a warning not an error
        warns.append(f"{where}: color {value!r} is not #rgb/#rrggbb/#rrggbbaa "
                     f"or linear-gradient(...) — firmware will fall back to white")


def validate_frame(frame: dict, idx: int, assets_dir: str,
                   errors: List[str], warns: List[str]) -> None:
    if not isinstance(frame, dict):
        errors.append(f"frame {idx}: must be an object")
        return

    keys = [k for k in frame.keys()]
    has_clear = "clear" in keys
    has_load = "load_anim" in keys
    drawables = [k for k in keys if k in DRAWABLE_TYPES]

    for k in keys:
        if k not in KNOWN_FRAME_KEYS:
            warns.append(f"frame {idx}: unknown key {k!r} (firmware ignores it)")

    # duration rules: required unless the frame is purely a clear or load_anim
    is_control_only = (has_clear or has_load) and not drawables
    if "duration" in frame:
        if not isinstance(frame["duration"], (int, float)):
            errors.append(f"frame {idx}: duration must be a number")
    elif not is_control_only:
        warns.append(f"frame {idx}: no duration (firmware treats it as 0 ms — "
                     f"frame will flash by)")

    # per-type checks
    for k in drawables:
        obj = frame[k]
        if not isinstance(obj, dict):
            errors.append(f"frame {idx}.{k}: must be an object")
            continue
        validate_color(obj.get("color"), f"frame {idx}.{k}", warns)
        if k == "asset" and not obj.get("name"):
            errors.append(f"frame {idx}.asset: missing 'name'")
        if k == "asset":
            check_asset_present(obj.get("name"), assets_dir, idx, warns)
        if k in ("text", "scrolling_text") and "text" not in obj:
            warns.append(f"frame {idx}.{k}: no 'text' field (renders empty)")
        if k == "scrolling_text":
            sd = obj.get("scroll_direction", "horizontal")
            if sd not in ("horizontal", "vertical"):
                warns.append(f"frame {idx}.scrolling_text: scroll_direction "
                             f"{sd!r} — firmware treats non-'vertical' as horizontal")

    if has_load:
        lo = frame["load_anim"]
        if not isinstance(lo, dict) or not lo.get("name"):
            errors.append(f"frame {idx}.load_anim: missing 'name'")
        if isinstance(lo, dict) and "speed" in lo:
            if not isinstance(lo["speed"], (int, float)) or lo["speed"] <= 0:
                warns.append(f"frame {idx}.load_anim: speed should be > 0 "
                             f"(firmware falls back to 1.0)")

    if has_clear:
        co = frame["clear"]
        if isinstance(co, dict) and "obj" in co:
            if not isinstance(co["obj"], list):
                errors.append(f"frame {idx}.clear.obj: must be a list of "
                              f"'frame:index' strings")
            else:
                for ref in co["obj"]:
                    if not isinstance(ref, str) or ":" not in ref:
                        warns.append(f"frame {idx}.clear.obj: {ref!r} is not in "
                                     f"'frame:index' form")


def check_asset_present(name, assets_dir: str, idx: int,
                        warns: List[str]) -> None:
    if not name or not assets_dir:
        return
    path = os.path.join(assets_dir, name)
    if not os.path.isfile(path):
        warns.append(f"frame {idx}.asset: '{name}' not found in {assets_dir} — "
                     f"firmware will draw the 4x4 missing-asset pattern")


def validate_anim(doc: dict, assets_dir: str) -> Tuple[List[str], List[str]]:
    errors: List[str] = []
    warns: List[str] = []

    if not isinstance(doc, dict):
        return ["top level must be an object"], warns

    dp = doc.get("default_params", {})
    if not isinstance(dp, dict):
        errors.append("default_params must be an object")
    else:
        for k, v in dp.items():
            if not isinstance(v, (str, int, float)):
                warns.append(f"default_params.{k}: should be a string")

    frames = doc.get("frames")
    if not isinstance(frames, list):
        errors.append("'frames' must be a list")
        return errors, warns
    if not frames:
        warns.append("'frames' is empty (nothing will render)")

    for i, frame in enumerate(frames, start=1):  # 1-based, matches firmware
        validate_frame(frame, i, assets_dir, errors, warns)

    return errors, warns


def resolve_paths(arg: str) -> Tuple[str, str]:
    if os.path.isdir(arg):
        return os.path.join(arg, "anim.json"), os.path.join(arg, "assets")
    # a file: assets dir is the sibling assets/ folder if present
    base = os.path.dirname(os.path.abspath(arg))
    return arg, os.path.join(base, "assets")


def main(argv: List[str]) -> int:
    if len(argv) != 1:
        print("usage: validate_anim.py <anim.json | anim_dir>", file=sys.stderr)
        return 2
    anim_path, assets_dir = resolve_paths(argv[0])
    if not os.path.isfile(anim_path):
        print(f"error: {anim_path} not found", file=sys.stderr)
        return 2
    try:
        doc = json.load(open(anim_path, encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"error: invalid JSON: {exc}", file=sys.stderr)
        return 1

    if not os.path.isdir(assets_dir):
        assets_dir = ""  # skip asset-presence checks

    errors, warns = validate_anim(doc, assets_dir)

    for w in warns:
        print(f"  warning: {w}")
    for e in errors:
        print(f"  ERROR:   {e}")

    if errors:
        print(f"\n{anim_path}: {len(errors)} error(s), {len(warns)} warning(s)")
        return 1
    print(f"\n{anim_path}: OK ({len(warns)} warning(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

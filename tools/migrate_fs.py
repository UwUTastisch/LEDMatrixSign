#!/usr/bin/env python3
"""
migrate_fs.py — migrate a LED Matrix Sign 1.0 filesystem to the 2.0 layout.

1.0 on-device layout
    /config.json
    /index.html              (bundled web UI — dropped in 2.0)
    /images/<name>.bmp        (24-bit BMP frames)
    /imgchain/<n>.chain       (line 1 = frame duration ms, then frame basenames)

2.0 on-device layout
    /config.json              (copied verbatim)
    /anim/<id>/anim.json      (object-tree animation)
    /anim/<id>/assets/*.bmp   (32-bit BGRA8888 assets)

Each 1.0 image chain becomes one 2.0 animation that plays its frames as a
slideshow of `asset` objects. Loose images (not referenced by any chain) are,
by default, each wrapped in a one-frame static animation.

Workflow
    1. Copy the device filesystem to a local folder (e.g. `littlefs_dump/`).
    2. python3 migrate_fs.py littlefs_dump/ out_fs/
    3. Upload the contents of `out_fs/` to the device (or flash the image).

No third-party dependencies. Pairs with bmp_bgra.py (same folder).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bmp_bgra  # noqa: E402


def sanitize_id_component(name: str) -> str:
    """Make a string safe for use as a path/id component."""
    s = re.sub(r"[^A-Za-z0-9_.-]", "_", name.strip())
    return s or "unnamed"


def strip_bmp(name: str) -> str:
    n = name.strip()
    if n.lower().endswith(".bmp"):
        n = n[:-4]
    return n


def parse_chain_file(path: str) -> Tuple[int, List[str]]:
    """Return (frame_duration_ms, [frame_basenames]) from a .chain file."""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        lines = [ln.strip() for ln in fh.readlines()]
    lines = [ln for ln in lines if ln != ""]
    if not lines:
        return 1000, []
    try:
        duration = int(float(lines[0]))
    except ValueError:
        # Some chains may omit the duration header; assume 1 fps.
        duration = 1000
        frames = [strip_bmp(ln) for ln in lines]
        return duration, frames
    frames = [strip_bmp(ln) for ln in lines[1:]]
    if duration <= 0:
        duration = 1000
    return duration, frames


def build_slideshow_anim(frames: List[str], duration_ms: int) -> dict:
    """Build an anim.json dict that plays `frames` as a clear+asset slideshow."""
    out_frames: List[dict] = []
    for base in frames:
        # Clear everything (duration 0) so each slide starts from a clean buffer,
        # including after the animation loops back to the first frame.
        out_frames.append({"clear": {}})
        out_frames.append({
            "asset": {"x": 0, "y": 0, "name": base + ".bmp"},
            "duration": duration_ms,
        })
    return {"default_params": {}, "frames": out_frames}


def build_static_anim(image_base: str, duration_ms: int = 1000) -> dict:
    return {
        "default_params": {},
        "frames": [{
            "asset": {"x": 0, "y": 0, "name": image_base + ".bmp"},
            "duration": duration_ms,
        }],
    }


def convert_asset(src_images_dir: str, base: str, dst_assets_dir: str,
                  report: dict) -> bool:
    """Convert /images/<base>.bmp -> <dst_assets>/<base>.bmp (BGRA8888)."""
    src = os.path.join(src_images_dir, base + ".bmp")
    if not os.path.isfile(src):
        # try case-insensitive / alternate extension match
        alt = _find_ci(src_images_dir, base + ".bmp")
        if alt:
            src = alt
        else:
            report["missing_images"].append(base + ".bmp")
            return False
    os.makedirs(dst_assets_dir, exist_ok=True)
    dst = os.path.join(dst_assets_dir, base + ".bmp")
    try:
        bmp_bgra.convert_file(src, dst)
        report["converted_assets"] += 1
        return True
    except Exception as exc:  # noqa: BLE001
        report["failed_images"].append(f"{base}.bmp: {exc}")
        return False


def _find_ci(directory: str, filename: str) -> Optional[str]:
    if not os.path.isdir(directory):
        return None
    want = filename.lower()
    for entry in os.listdir(directory):
        if entry.lower() == want:
            return os.path.join(directory, entry)
    return None


def write_anim(out_root: str, anim_id: str, anim: dict) -> None:
    anim_dir = os.path.join(out_root, "anim", anim_id)
    os.makedirs(os.path.join(anim_dir, "assets"), exist_ok=True)
    with open(os.path.join(anim_dir, "anim.json"), "w", encoding="utf-8") as fh:
        json.dump(anim, fh, indent=2)


def migrate(src_root: str, out_root: str, prefix: str,
            include_orphans: bool) -> dict:
    report = {
        "animations": [],
        "converted_assets": 0,
        "missing_images": [],
        "failed_images": [],
        "skipped": [],
    }

    images_dir = os.path.join(src_root, "images")
    imgchain_dir = os.path.join(src_root, "imgchain")

    os.makedirs(out_root, exist_ok=True)

    # config.json — copy verbatim
    src_cfg = os.path.join(src_root, "config.json")
    if os.path.isfile(src_cfg):
        shutil.copy2(src_cfg, os.path.join(out_root, "config.json"))
    else:
        report["skipped"].append("config.json not found in source")

    used_images = set()

    # 1) chains -> slideshow animations
    if os.path.isdir(imgchain_dir):
        chain_files = sorted(
            f for f in os.listdir(imgchain_dir) if f.lower().endswith(".chain")
        )
        for cf in chain_files:
            num = strip_ext(cf)
            anim_id = sanitize_id_component(f"{prefix}.chain{num}")
            duration, frames = parse_chain_file(os.path.join(imgchain_dir, cf))
            anim = build_slideshow_anim(frames, duration)
            write_anim(out_root, anim_id, anim)
            assets_dir = os.path.join(out_root, "anim", anim_id, "assets")
            for base in dict.fromkeys(frames):  # de-dup, keep order
                used_images.add(base.lower())
                convert_asset(images_dir, base, assets_dir, report)
            report["animations"].append(
                {"id": anim_id, "frames": len(frames), "duration_ms": duration}
            )

    # 2) loose images -> static one-frame animations
    if include_orphans and os.path.isdir(images_dir):
        for entry in sorted(os.listdir(images_dir)):
            if not entry.lower().endswith(".bmp"):
                continue
            base = strip_bmp(entry)
            if base.lower() in used_images:
                continue
            anim_id = sanitize_id_component(f"{prefix}.{base}")
            anim = build_static_anim(base)
            write_anim(out_root, anim_id, anim)
            assets_dir = os.path.join(out_root, "anim", anim_id, "assets")
            convert_asset(images_dir, base, assets_dir, report)
            report["animations"].append(
                {"id": anim_id, "frames": 1, "duration_ms": 1000, "orphan": True}
            )

    return report


def strip_ext(name: str) -> str:
    return name.rsplit(".", 1)[0] if "." in name else name


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Migrate a 1.0 LED Matrix Sign filesystem to the 2.0 layout."
    )
    ap.add_argument("src", help="source folder (a copy of the 1.0 device FS)")
    ap.add_argument("out", help="destination folder for the 2.0 FS tree")
    ap.add_argument(
        "--prefix", default="de.uwutastisch",
        help="reverse-DNS namespace prefix for generated animation ids",
    )
    ap.add_argument(
        "--no-orphans", action="store_true",
        help="do not wrap loose (un-chained) images as static animations",
    )
    ap.add_argument(
        "--manifest", metavar="PATH",
        help="also write the migration report as JSON to PATH",
    )
    args = ap.parse_args(argv)

    if not os.path.isdir(args.src):
        print(f"error: source folder not found: {args.src}", file=sys.stderr)
        return 2

    report = migrate(args.src, args.out, args.prefix, not args.no_orphans)

    print(f"Migration complete -> {args.out}")
    print(f"  animations written : {len(report['animations'])}")
    print(f"  assets converted   : {report['converted_assets']}")
    if report["missing_images"]:
        print(f"  missing images     : {len(report['missing_images'])} "
              f"(firmware will draw the missing-asset pattern)")
        for m in report["missing_images"][:10]:
            print(f"      - {m}")
    if report["failed_images"]:
        print(f"  failed conversions : {len(report['failed_images'])}")
        for m in report["failed_images"][:10]:
            print(f"      - {m}")
    if report["skipped"]:
        for s in report["skipped"]:
            print(f"  note: {s}")

    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
        print(f"  manifest written   : {args.manifest}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

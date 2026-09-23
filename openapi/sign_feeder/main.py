"""
Sign feeder for LEDMatrixSign 2.0.

Loads every package in packages/ and plays them round after round:
upload (once) → preload all (concurrently) → display one after another.
See README.md for how to write a package.

    python3 main.py             run forever
    python3 main.py --list      show the playlist and exit
    python3 main.py --once      one round, then exit
    python3 main.py --dry-run   don't touch the sign, print what would be sent
"""
import argparse
import asyncio
import os
import time

from openapi.sign_feeder.feeder import load_env
from openapi.sign_feeder.feeder.packages import build_playlist, load_packages
from openapi.sign_feeder.feeder.runner import Runner
from openapi.sign_feeder.feeder.sign import Sign

HERE = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="show the playlist and exit")
    ap.add_argument("--once", action="store_true", help="run a single round")
    ap.add_argument("--dry-run", action="store_true", help="print sign calls instead of sending")
    ap.add_argument("--packages", default=os.path.join(HERE, "packages"), help="packages folder")
    a = ap.parse_args()

    env = load_env(os.path.join(HERE, ".env"))
    sign = Sign(env.str("DEVICE_HOST", "http://4.3.2.1"), dry_run=a.dry_run, log=log)
    packages = load_packages(a.packages, env, log=log)
    playlist = build_playlist(packages, env.list("ORDER"), log=log)

    log(f"{len(packages)} packages, {len(playlist)} slides → {sign.host}")
    for s in playlist:
        extra = f" args={s.args}" if s.args else ""
        log(f"  • {s.label} [{s.package.name}] {s.duration:g}s{extra}")
    if a.list:
        return
    try:
        asyncio.run(Runner(playlist, packages, sign, env, log=log).run(once=a.once))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

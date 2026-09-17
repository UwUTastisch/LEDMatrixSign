"""
push_departures.py
-------------------
Fetches live departures from abfahrten-rsag.de and pushes them to a
LEDMatrixSign 2.0 device as params for the anim.json
animation (see README_2.0.md's `/anim` and `/file` endpoints).

One-time setup (uploads anim.json + icon assets to the device):
    python3 push_departures.py --setup

Then run continuously (fetches every REFRESH_SECONDS and re-starts the
animation with fresh params — cheap, since it's a single static frame):
    python3 push_departures.py

Config below — set DEVICE_HOST to your sign's address (e.g. "sign.local"
or an IP), and STATION_URL to the stop you generated the anim for.
"""

import base64
import json
import os
import sys
import time

import requests
from bs4 import BeautifulSoup

# =========================================================
# Config
# =========================================================
DEVICE_HOST = "http://fenster-matrix.hackhro" #"http://4.3.2.1"          # <-- set to your device's address
ANIM_NAME = "de.hack-hro.rsag_monitor"          # <-- the /anim/<id>/ this gets uploaded to
STATIONS = [
    ("S Parkstraße", "https://abfahrten-rsag.de/dfi/_/7", "#0a2814"),
    ("Doberaner Platz", "https://abfahrten-rsag.de/dfi/_/14", "#0a1432"),
]
REFRESH_SECONDS = 30
MAX_ROWS = 5

HERE = os.path.dirname(os.path.abspath(__file__))
ANIM_JSON_PATH = os.path.join(HERE, "anim.json")
ASSET_DIR = os.path.join(HERE, "assets")
ICON_FILES = ["icon_tram.bmp", "icon_bus.bmp", "icon_sbahn.bmp"]

session = requests.Session()

# =========================================================
# Device API helpers
# =========================================================
def api_post(path, payload):
    r = session.post(DEVICE_HOST.rstrip("/") + path, json=payload, timeout=10)
    r.raise_for_status()
    return r.json() if r.content else {}

def upload_anim():
    with open(ANIM_JSON_PATH) as f:
        anim = json.load(f)
    print(f"uploading {ANIM_JSON_PATH} as /anim/{ANIM_NAME}/anim.json ...")
    api_post("/file/uploadanim", {"animname": ANIM_NAME, "anim": anim})

def upload_assets():
    for filename in ICON_FILES:
        path = os.path.join(ASSET_DIR, filename)
        with open(path, "rb") as f:
            data = base64.b64encode(f.read()).decode("ascii")
        print(f"uploading asset {filename} ...")
        api_post("/file/uploadasset", {
            "animname": ANIM_NAME,
            "filename": filename,
            "data": data,
        })

def setup():
    """One-time push of the animation + its icon assets to the device."""
    upload_anim()
    upload_assets()
    print("setup done.")

# =========================================================
# Fetching departures (same logic as rsag_abfahrtsmonitor.py)
# =========================================================
def classify_kind(icon_classes):
    classes = " ".join(icon_classes).lower()
    if "tram" in classes:
        return "tram"
    if "comm_t" in classes or "sbahn" in classes or "s-bahn" in classes or "suburban" in classes:
        return "sbahn"
    return "bus"

def fetch_departures(station_url):
    r = session.get(station_url, timeout=5)
    r.raise_for_status()
    soup = BeautifulSoup(r.text, "html.parser")

    deps = []
    for e in soup.select("div.entry"):
        icon_classes = e.select_one(".icon")["class"]
        kind = classify_kind(icon_classes)
        line = e.select_one(".line").get_text(strip=True)
        dest = e.select_one(".direction").get_text(strip=True)
        dep = e.select_one(".departure").get_text(strip=True)
        if dep == "sofort":
            dep = "now"
        deps.append({"type": kind, "line": line, "dest": dest, "dep": dep})
    return deps

# =========================================================
# Build the params payload for /anim/start
# =========================================================
def replace_german_chars(value):
    return value.translate(str.maketrans({
        "ä": "ae",
        "ö": "oe",
        "ü": "ue",
        "ß": "ss",
    }))


def build_params(station_name, departures, bar_color):
    params = {"station": replace_german_chars(station_name), "bar_color": bar_color}
    rows = departures[:MAX_ROWS]
    for i in range(1, MAX_ROWS + 1):
        n = i - 1
        active = n < len(rows)
        params[f"active{i}"] = active
        d = rows[n] if active else {"type": "tram", "line": "", "dest": "", "dep": ""}
        params[f"type{i}"] = d["type"]
        params[f"line{i}"] = replace_german_chars(d["line"])
        # The compositor's scrolling_text never stops scrolling, even for
        # short strings, so destination always goes through the scroll
        # field; the static "dest" field is left blank (see anim.json).
        params[f"dest{i}"] = ""
        params[f"scroll{i}"] = replace_german_chars(d["dest"])
        params[f"dep{i}"] = replace_german_chars(d["dep"])
    return params

def push_once():
    station_name, station_url, bar_color = STATIONS[push_once.station_index]
    departures = fetch_departures(station_url)
    params = build_params(station_name, departures, bar_color)
    api_post("/anim/start", {"animname": ANIM_NAME, "params": params, "cycles": 0})
    print(f"pushed {len(departures)} departures for {station_name} ({time.strftime('%H:%M:%S')})")
    push_once.station_index = (push_once.station_index + 1) % len(STATIONS)


push_once.station_index = 0

# =========================================================
# Main
# =========================================================
def main():
    if "--setup" in sys.argv:
        setup()
        return

    while True:
        try:
            push_once()
        except requests.RequestException as e:
            print(f"⚠️ push failed: {e}")
        time.sleep(REFRESH_SECONDS)

if __name__ == "__main__":
    main()

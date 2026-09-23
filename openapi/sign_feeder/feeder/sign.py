"""The LEDMatrixSign 2.0 REST API, as far as the feeder needs it."""
import base64
import json

import requests


class Sign:
    def __init__(self, host, dry_run=False, log=print):
        self.host = host.rstrip("/")
        self.dry_run = dry_run
        self.log = log
        self.http = requests.Session()

    def post(self, path, payload):
        if self.dry_run:
            self.log(f"[dry-run] POST {path} {json.dumps(payload, ensure_ascii=False)[:300]}")
            return {}
        r = self.http.post(self.host + path, json=payload, timeout=10)
        r.raise_for_status()
        return r.json() if r.content else {}

    def start_anim(self, name, params=None, cycles=0):
        return self.post("/anim/start", {"animname": name, "params": params or {}, "cycles": cycles})

    def brightness(self, value):
        return self.post("/display/brightness", {"brightness": int(value)})

    def upload_anim(self, name, anim):
        return self.post("/file/uploadanim", {"animname": name, "anim": anim})

    def upload_asset(self, name, filename, data: bytes):
        return self.post("/file/uploadasset", {
            "animname": name,
            "filename": filename,
            "data": base64.b64encode(data).decode("ascii"),
        })

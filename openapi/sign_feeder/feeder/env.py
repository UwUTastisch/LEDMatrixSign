"""`.env` files with typed getters. A package's .env inherits the global one."""
import json
import os

from dotenv import dotenv_values


class Env(dict):
    def str(self, key, default=""):
        v = self.get(key)
        return default if v is None or v == "" else v

    def int(self, key, default=0):
        return int(self.str(key, default))

    def float(self, key, default=0.0):
        return float(self.str(key, default))

    def bool(self, key, default=False):
        v = self.get(key)
        if v is None or v == "":
            return default
        return v.strip().lower() in ("1", "true", "yes", "on")

    def list(self, key, default=None):
        """Comma-separated → list of stripped strings."""
        v = self.str(key, "")
        return [x.strip() for x in v.split(",") if x.strip()] if v else (default or [])

    def json(self, key, default=None):
        v = self.str(key, "")
        return json.loads(v) if v else default


def load_env(path, parent=None):
    values = dict(parent or {})
    if os.path.exists(path):
        values.update({k: v for k, v in dotenv_values(path).items() if v is not None})
    return Env(values)

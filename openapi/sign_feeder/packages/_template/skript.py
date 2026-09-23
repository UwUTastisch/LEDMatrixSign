"""Template — copy this folder to packages/<anim name>/ (no leading "_").

Only preload() is required.

ctx.name   package folder = animation name on the sign
ctx.label  this instance's label
ctx.env    this package's .env on top of the global .env
           (ctx.env.str / .int / .float / .bool / .list / .json)
ctx.args   this instance's args (from INSTANCES in .env or register())
ctx.http   shared requests.Session for your API calls
ctx.sign   the sign: start_anim(name, params), brightness(v), post(path, json)
ctx.log    log(message)
"""
from openapi.sign_feeder.feeder import to_sign_text


def preload(ctx):
    """Stage 1: fetch what you need. Runs concurrently with all other
    packages. Return None (skip this round), a params dict (one slide)
    or a list of params dicts (one slide each)."""
    return {"text": to_sign_text(ctx.env.str("TEXT", "Hallo Welt"))}


# Optional. Stage 2: default is ctx.sign.start_anim(ctx.name, params),
# then the slide is held for DURATION seconds.
# def display(ctx, params):
#     ctx.sign.start_anim(ctx.name, params)


# Optional. Default: one slide per INSTANCES entry, else a single slide.
# def register(add, env):
#     add(label="first", duration=10, some_arg=1)
#     add(label="second", some_arg=2)

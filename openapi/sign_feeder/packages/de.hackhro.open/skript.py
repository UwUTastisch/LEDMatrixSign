"""Shows the "open" animation while the door switch says the space is open."""


def preload(ctx):
    r = ctx.http.get(ctx.env.str("SWITCH_URL"), timeout=5)
    r.raise_for_status()
    state = int(str(r.json().get("switch_state", "0")).strip() or 0)
    return {} if state == ctx.env.int("OPEN_STATE", 2) else None

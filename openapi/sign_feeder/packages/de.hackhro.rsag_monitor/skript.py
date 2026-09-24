"""Live departures from abfahrten-rsag.de. One slide per station in .env."""
from bs4 import BeautifulSoup

from feeder import to_sign_text


def kind(icon_classes):
    c = " ".join(icon_classes).lower()
    if "tram" in c:
        return "tram"
    if any(k in c for k in ("comm_t", "sbahn", "s-bahn", "suburban")):
        return "sbahn"
    return "bus"


def preload(ctx):
    r = ctx.http.get(ctx.args["url"], timeout=5)
    r.raise_for_status()
    rows = []
    for e in BeautifulSoup(r.text, "html.parser").select("div.entry"):
        icon, line, dest, dep = (e.select_one(s) for s in (".icon", ".line", ".direction", ".departure"))
        if icon and line and dest and dep:
            d = dep.get_text(strip=True)
            rows.append((kind(icon.get("class", [])), line.get_text(strip=True),
                         dest.get_text(strip=True), "now" if d == "sofort" else d))

    params = {"station": to_sign_text(ctx.args["name"]), "bar_color": ctx.args.get("color", "#000000")}
    for i in range(1, ctx.env.int("MAX_ROWS", 5) + 1):
        typ, line, dest, dep = rows[i - 1] if i <= len(rows) else ("tram", "", "", "")
        params.update({
            f"active{i}": i <= len(rows),
            f"type{i}": typ,
            f"line{i}": to_sign_text(line),
            f"dest{i}": "",                     # static field unused, see anim.json
            f"scroll{i}": to_sign_text(dest),   # destination always scrolls
            f"dep{i}": to_sign_text(dep),
        })
    return params

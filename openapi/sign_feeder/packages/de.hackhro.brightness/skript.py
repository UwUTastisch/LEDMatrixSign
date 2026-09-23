"""Sets the sign brightness from the sun: a sine curve from sunrise to
sunset between MIN and MAX, MIN at night. No animation (DURATION=0)."""
import datetime as dt
import math

_last = None


def sun_times_utc(day, lat, lon):
    """(sunrise, sunset) in UTC, NOAA approximation. None = polar night,
    "day" = midnight sun."""
    g = 2 * math.pi / 365 * (day.timetuple().tm_yday - 1)
    eqtime = 229.18 * (0.000075 + 0.001868 * math.cos(g) - 0.032077 * math.sin(g)
                       - 0.014615 * math.cos(2 * g) - 0.040849 * math.sin(2 * g))
    decl = (0.006918 - 0.399912 * math.cos(g) + 0.070257 * math.sin(g)
            - 0.006758 * math.cos(2 * g) + 0.000907 * math.sin(2 * g)
            - 0.002697 * math.cos(3 * g) + 0.00148 * math.sin(3 * g))
    lat = math.radians(lat)
    x = math.cos(math.radians(90.833)) / (math.cos(lat) * math.cos(decl)) - math.tan(lat) * math.tan(decl)
    if x > 1:
        return None
    if x < -1:
        return "day"
    ha = math.degrees(math.acos(x))
    midnight = dt.datetime(day.year, day.month, day.day, tzinfo=dt.timezone.utc)
    return (midnight + dt.timedelta(minutes=720 - 4 * (lon + ha) - eqtime),
            midnight + dt.timedelta(minutes=720 - 4 * (lon - ha) - eqtime))


def level(now_utc, env):
    lo, hi = env.int("MIN", 10), env.int("MAX", 255)
    sun = sun_times_utc(now_utc.date(), env.float("LATITUDE"), env.float("LONGITUDE"))
    if sun is None:
        return lo
    if sun == "day":
        return hi
    rise, set_ = sun
    if not rise <= now_utc <= set_:
        return lo
    return round(lo + (hi - lo) * math.sin(math.pi * (now_utc - rise) / (set_ - rise)))


def preload(ctx):
    return {"brightness": level(dt.datetime.now(dt.timezone.utc), ctx.env)}


def display(ctx, params):
    global _last
    if params["brightness"] != _last:          # only send changes
        ctx.sign.brightness(params["brightness"])
        ctx.log(f"brightness → {params['brightness']}")
        _last = params["brightness"]

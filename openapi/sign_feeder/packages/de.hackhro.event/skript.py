"""Upcoming events from an iCal feed — one slide per event."""
import datetime as dt

import icalendar
import recurring_ical_events

from openapi.sign_feeder.feeder import to_sign_text

WEEKDAYS = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]
LOCAL_TZ = dt.datetime.now().astimezone().tzinfo


def when(value):
    if isinstance(value, dt.datetime):
        start = (value if value.tzinfo else value.replace(tzinfo=LOCAL_TZ)).astimezone(LOCAL_TZ)
        return start, f"{WEEKDAYS[start.weekday()]} {start:%d.%m. %H:%M}"
    start = dt.datetime.combine(value, dt.time(0), LOCAL_TZ)   # all-day event
    return start, f"{WEEKDAYS[start.weekday()]} {start:%d.%m.}"


def preload(ctx):
    url = ctx.env.str("EVENTS_URL")
    if not url:
        return None
    r = ctx.http.get(url, timeout=10)
    r.raise_for_status()
    now = dt.datetime.now(LOCAL_TZ)
    until = now + dt.timedelta(days=ctx.env.int("LOOKAHEAD_DAYS", 14))

    events = []
    for ev in recurring_ical_events.of(icalendar.Calendar.from_ical(r.content)).between(now, until):
        start, date = when(ev.get("DTSTART").dt)
        events.append((start, {
            ctx.env.str("PARAM_TEXT", "event_text"): to_sign_text(ev.get("SUMMARY", "")),
            ctx.env.str("PARAM_DATE", "date"): date,
            ctx.env.str("PARAM_WHERE", "where"): to_sign_text(ev.get("LOCATION", "")),
        }))
    if len(events) < ctx.env.int("MIN_EVENTS", 1):
        return None
    events.sort(key=lambda e: e[0])
    return [params for _, params in events[:ctx.env.int("MAX_SHOWN", 3)]]

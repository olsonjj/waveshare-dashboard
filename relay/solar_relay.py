#!/usr/bin/env python3
"""solar_relay.py — LAN relay for the Waveshare ESP32-S3 dashboard.

Why this exists: the Envoy's local API is slow (~29 s per exchange phase) when
served directly to the ESP32 board (see NOTES.md, 2026-11-15 session), keeping
the WiFi radio busy ~56 s/min, which visibly disturbs the RGB panel. The same
exchange takes ~0.3 s from this Mac. So the relay fetches the Envoy on the fast
path and serves a tiny cached JSON to the board over plain LAN HTTP.

Contract (for dashboard_data.cpp):
    GET /solar          -> {"watts_now": <uint W>, "wh_today": <uint Wh>,
                           "hourly_kwh": [24 floats, -1.0 = no data yet],
                           "age_s": <seconds since upstream fetch>}
    Header: X-Relay-Key: <RELAY_KEY>       (else 403)
    Upstream failure     -> last good data with an "age_s" that keeps growing
                            ("epoch" is re-stamped at serve time so the
                            board's clock keeps tracking real time)
    Never-upstream-good  -> 503

A background sampler thread folds one wh_today sample into per-hour kWh
buckets every 60 s (bucket h = wh_today(end) - wh_today(hour start); the
current hour is a live partial). State survives restarts via
relay/solar_history.json (atomic write, last 14 days kept). The Envoy local
API has no hourly time-series endpoint, so history starts at deploy time.

Config via environment or defaults below:
    ENVOY_HOST, ENVOY_TOKEN  (falls back to ~/Code/enphase-test/.env)
    RELAY_PORT   (default 8420)
    RELAY_KEY    (required, no default: set via env and put the same value
                           in secrets.h)

Run:  python3 solar_relay.py            (binds 0.0.0.0:8420)
"""
import json
import os
import ssl
import sys
import time
import threading
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------- config ----
def load_env_file(path):
    env = {}
    try:
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
    except FileNotFoundError:
        pass
    return env

_file_env = load_env_file(os.path.expanduser("~/Code/enphase-test/.env"))
ENVOY_HOST  = os.environ.get("ENVOY_HOST", _file_env.get("ENVOY_HOST", "")).removeprefix("https://").rstrip("/")
ENVOY_TOKEN = os.environ.get("ENVOY_TOKEN", _file_env.get("ENVOY_TOKEN", ""))
RELAY_PORT  = int(os.environ.get("RELAY_PORT", "8420"))
RELAY_KEY   = os.environ.get("RELAY_KEY", "")  # empty = refuse to start
UPSTREAM_TTL_S  = 30          # board polls 60 s; upstream fetched at most this often
UPSTREAM_TIMEOUT = 8          # seconds; never block the board's request long
SAMPLE_INTERVAL_S = 60       # wh_today sampler cadence for the hourly buckets
HISTORY_DAYS   = 14          # days of hourly history kept in solar_history.json
HISTORY_PATH   = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "solar_history.json")

if not ENVOY_HOST or not ENVOY_TOKEN:
    sys.exit("solar_relay: ENVOY_HOST/ENVOY_TOKEN missing (env or ~/Code/enphase-test/.env)")
if not RELAY_KEY:
    sys.exit("solar_relay: RELAY_KEY env var not set; refusing to serve "
             "readings unauthenticated on all interfaces")

# ----------------------------------------------------------------- cache ----
_lock = threading.Lock()
_fetch_lock = threading.Lock()      # serializes Envoy fetches
_cache = {"ok": False, "payload": {}, "ts": 0.0}

def fetch_upstream():
    """One Envoy fetch; returns the compact dict for the board or raises."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False            # Envoy serves a self-signed cert
    ctx.verify_mode = ssl.CERT_NONE
    def get(path):
        req = urllib.request.Request(f"https://{ENVOY_HOST}{path}", headers={
            "Authorization": f"Bearer {ENVOY_TOKEN}",
            "Accept": "application/json",
        })
        with urllib.request.urlopen(req, timeout=UPSTREAM_TIMEOUT, context=ctx) as r:
            return json.load(r)

    pcu = get("/ivp/pdm/energy")["production"]["pcu"]
    # today's system peak = sum of each micro's daily max (they don't peak
    # simultaneously, but it's the closest the API offers)
    inverters = get("/api/v1/production/inverters")
    peak = sum(int(i.get("maxReportWatts", 0)) for i in inverters)
    return {
        "watts_now": int(pcu.get("wattsNow", 0)),
        "wh_today": int(pcu.get("wattHoursToday", 0)),
        "watt_hours_lifetime": int(pcu.get("wattHoursLifetime", 0)),
        "watts_peak_today": peak,
        "epoch": int(time.time()),   # wall clock for the board (SNTP wedges its loop)
    }

def get_payload():
    """Cached upstream fetch: at most one Envoy request per UPSTREAM_TTL_S.
    Overlapping requesters coalesce on _fetch_lock (re-check the cache inside
    it, so only the thread that actually misses fetches)."""
    with _lock:
        if _cache["ok"] and (time.time() - _cache["ts"]) < UPSTREAM_TTL_S:
            return _cache["payload"], 0
    with _fetch_lock:                      # one Envoy fetch at a time
        with _lock:
            if _cache["ok"] and (time.time() - _cache["ts"]) < UPSTREAM_TTL_S:
                return _cache["payload"], 0   # another thread just refreshed
        try:
            payload = fetch_upstream()
        except Exception:
            with _lock:
                if _cache["ok"]:
                    return _cache["payload"], time.time() - _cache["ts"]   # stale-but-served
                raise
        with _lock:
            _cache.update({"ok": True, "payload": payload, "ts": time.time()})
        return payload, 0

# --------------------------------------------------------------- history ---
# One entry per local day, keyed "YYYY-MM-DD". Buckets are kWh; -1.0 means
# "no sample ever landed in that hour" (the Envoy API has no backfill).
# "wh_at_hour_start" is the wh_today reading when the hour opened, so a
# restart mid-day resumes the live bucket without corrupting closed ones.
_hist_lock = threading.Lock()
_hist = None                    # today's entry (dict below), set by _load_history
_days = {}                      # all persisted days: date -> entry

def _now_local():
    t = time.localtime()
    return time.strftime("%Y-%m-%d", t), t.tm_hour

def _new_day(date):
    return {"date": date, "hourly_kwh": [-1.0] * 24, "wh_at_hour_start": [-1] * 24,
            "last_wh": -1, "last_hour": -1}

def _apply_sample(state, date, hour, wh):
    """Fold one wh_today sample into the day state; returns (new) state."""
    if date != state["date"]:      # local-date rollover: old day is already saved
        state = _new_day(date)
    prev = state["last_hour"]
    if hour != prev:               # hour boundary: close the previous hour
        if prev >= 0 and state["wh_at_hour_start"][prev] >= 0 and state["last_wh"] >= 0:
            state["hourly_kwh"][prev] = round(
                (state["last_wh"] - state["wh_at_hour_start"][prev]) / 1000.0, 3)
        # start the new hour at the previous sample's wh_today, so the
        # straddle energy across the boundary lands in the closed bucket
        state["wh_at_hour_start"][hour] = state["last_wh"] if state["last_wh"] >= 0 else wh
        state["last_hour"] = hour
    if state["wh_at_hour_start"][hour] >= 0:                     # live partial
        state["hourly_kwh"][hour] = round(
            (wh - state["wh_at_hour_start"][hour]) / 1000.0, 3)
    state["last_wh"] = wh
    return state

def _prune_locked():
    """Keep only the newest HISTORY_DAYS days (caller holds _hist_lock)."""
    for d in sorted(_days)[:-HISTORY_DAYS]:
        del _days[d]

def _save_locked():
    """Atomic write of the whole history (caller holds _hist_lock)."""
    tmp = HISTORY_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"version": 1, "days": _days}, f)
    os.replace(tmp, HISTORY_PATH)          # atomic on POSIX

def _load_history():
    global _hist
    try:
        with open(HISTORY_PATH) as f:
            data = json.load(f)
    except (FileNotFoundError, ValueError):
        data = {}
    for d, e in (data.get("days") or {}).items():
        if isinstance(e, dict) and isinstance(e.get("hourly_kwh"), list) \
                and len(e["hourly_kwh"]) == 24 \
                and isinstance(e.get("wh_at_hour_start"), list) \
                and len(e["wh_at_hour_start"]) == 24:
            e["hourly_kwh"] = [float(x) for x in e["hourly_kwh"]]
            e["wh_at_hour_start"] = [int(x) for x in e["wh_at_hour_start"]]
            e["last_wh"] = int(e.get("last_wh", -1))
            e["last_hour"] = int(e.get("last_hour", -1))
            _days[d] = e
    date, _ = _now_local()
    _hist = _days.get(date) or _new_day(date)
    _hist["date"] = date

def _record_sample(wh):
    """Sampler entry point: update buckets, then persist atomically."""
    date, hour = _now_local()
    with _hist_lock:
        global _hist
        _hist = _apply_sample(_hist, date, hour, wh)
        _days[date] = _hist
        _prune_locked()
        _save_locked()

def get_hourly_kwh():
    """Snapshot of today's 24 buckets for the /solar response."""
    with _hist_lock:
        if _hist["date"] != time.strftime("%Y-%m-%d"):
            return [-1.0] * 24          # pre-rollover state: don't leak yesterday
        return list(_hist["hourly_kwh"])

def _sampler_loop():
    """Every SAMPLE_INTERVAL_S fold one fresh wh_today sample into the
    buckets. Reuses the request cache (so a board poll and a sampler tick
    share one Envoy fetch); only a fresh fetch (age 0) counts as a sample,
    so an upstream failure just keeps the last state untouched."""
    while True:
        try:
            payload, age = get_payload()
            if age == 0:
                _record_sample(int(payload["wh_today"]))
        except Exception as e:
            sys.stderr.write("[relay] %s sampler: %s\n"
                             % (time.strftime("%H:%M:%S"), e))
        time.sleep(SAMPLE_INTERVAL_S)

# ------------------------------------------------------------------ HTTP ----
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/solar":
            self.send_error(404); return
        if self.headers.get("X-Relay-Key") != RELAY_KEY:
            self.send_error(403); return
        try:
            payload, age = get_payload()
            body = json.dumps({**payload, "epoch": int(time.time()),   # serve-time clock
                               "hourly_kwh": get_hourly_kwh(),
                               "age_s": round(age, 1)}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except Exception as e:
            self.send_error(503, str(e))

    def log_message(self, fmt, *args):
        sys.stderr.write("[relay] %s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

if __name__ == "__main__":
    _load_history()
    threading.Thread(target=_sampler_loop, daemon=True, name="sampler").start()
    print(f"solar_relay: https://{ENVOY_HOST} -> http://0.0.0.0:{RELAY_PORT}/solar "
          f"(upstream TTL {UPSTREAM_TTL_S}s, token {len(ENVOY_TOKEN)} chars, "
          f"history {HISTORY_PATH})")
    ThreadingHTTPServer(("0.0.0.0", RELAY_PORT), Handler).serve_forever()

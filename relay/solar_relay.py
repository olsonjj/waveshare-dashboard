#!/usr/bin/env python3
"""solar_relay.py — LAN relay for the Waveshare ESP32-S3 dashboard.

Why this exists: the Envoy's local API is slow (~29 s per exchange phase) when
served directly to the ESP32 board (see NOTES.md, 2026-11-15 session), keeping
the WiFi radio busy ~56 s/min, which visibly disturbs the RGB panel. The same
exchange takes ~0.3 s from this Mac. So the relay fetches the Envoy on the fast
path and serves a tiny cached JSON to the board over plain LAN HTTP.

Contract (for dashboard_data.cpp):
    GET /solar          -> {"watts_now": <uint W>, "wh_today": <uint Wh>,
                           "age_s": <seconds since upstream fetch>}
    Header: X-Relay-Key: <RELAY_KEY>       (else 403)
    Upstream failure     -> last good data with an "age_s" that keeps growing
    Never-upstream-good  -> 503

Config via environment or defaults below:
    ENVOY_HOST, ENVOY_TOKEN  (falls back to ~/Code/enphase-test/.env)
    RELAY_PORT   (default 8420)
    RELAY_KEY    (default: generate one and put the same value in secrets.h)

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
RELAY_KEY   = os.environ.get("RELAY_KEY", "waveshare-relay-1")
UPSTREAM_TTL_S  = 30          # board polls 60 s; upstream fetched at most this often
UPSTREAM_TIMEOUT = 8          # seconds; never block the board's request long

if not ENVOY_HOST or not ENVOY_TOKEN:
    sys.exit("solar_relay: ENVOY_HOST/ENVOY_TOKEN missing (env or ~/Code/enphase-test/.env)")

# ----------------------------------------------------------------- cache ----
_lock = threading.Lock()
_cache = {"ok": False, "payload": {}, "ts": 0.0}

def fetch_upstream():
    """One Envoy fetch; returns the compact dict for the board or raises."""
    url = f"https://{ENVOY_HOST}/ivp/pdm/energy"
    req = urllib.request.Request(url, headers={
        "Authorization": f"Bearer {ENVOY_TOKEN}",
        "Accept": "application/json",
    })
    ctx = ssl.create_default_context()
    ctx.check_hostname = False            # Envoy serves a self-signed cert
    ctx.verify_mode = ssl.CERT_NONE
    with urllib.request.urlopen(req, timeout=UPSTREAM_TIMEOUT, context=ctx) as r:
        doc = json.load(r)
    pcu = doc["production"]["pcu"]
    return {
        "watts_now": int(pcu.get("wattsNow", 0)),
        "wh_today": int(pcu.get("wattHoursToday", 0)),
        "watt_hours_lifetime": int(pcu.get("wattHoursLifetime", 0)),
        "epoch": int(time.time()),   # wall clock for the board (SNTP wedges its loop)
    }

def get_payload():
    """Cached upstream fetch: at most one Envoy request per UPSTREAM_TTL_S."""
    now = time.time()
    with _lock:
        if _cache["ok"] and (now - _cache["ts"]) < UPSTREAM_TTL_S:
            return _cache["payload"], 0
    try:
        payload = fetch_upstream()
        with _lock:
            _cache.update({"ok": True, "payload": payload, "ts": now})
        return payload, 0
    except Exception:
        with _lock:
            if _cache["ok"]:
                return _cache["payload"], now - _cache["ts"]   # stale-but-served
            raise

# ------------------------------------------------------------------ HTTP ----
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/solar":
            self.send_error(404); return
        if self.headers.get("X-Relay-Key") != RELAY_KEY:
            self.send_error(403); return
        try:
            payload, age = get_payload()
            body = json.dumps({**payload, "age_s": round(age, 1)}).encode()
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
    print(f"solar_relay: https://{ENVOY_HOST} -> http://0.0.0.0:{RELAY_PORT}/solar "
          f"(upstream TTL {UPSTREAM_TTL_S}s, token {len(ENVOY_TOKEN)} chars)")
    ThreadingHTTPServer(("0.0.0.0", RELAY_PORT), Handler).serve_forever()

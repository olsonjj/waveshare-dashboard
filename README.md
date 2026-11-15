# Solar Ops Dashboard

A wall-mounted solar-telemetry HMI for the **Waveshare ESP32-S3-Touch-LCD-7**
(1024×600, 7", RGB + touch), built with PlatformIO / arduino-esp32 / LVGL 8.
The board shows live production from an Enphase IQ Gateway — current power,
today's yield, peak, and a 24-hour generation bar graph — over Wi-Fi, with a
wall clock synced without SNTP.

## Architecture

```
                      LAN (plain HTTP, ~90 B JSON)
 Enphase IQ Gateway ──>  solar relay (Mac / any host)  ──>  ESP32-S3 + LVGL
   (slow TLS, self-       polls the Envoy on its fast       (5-min cadence,
    signed, bearer)        path, builds hourly kWh           1-2 s radio
                          buckets, serves the board          per fetch)
```

The relay exists because the Envoy's TLS path is intrinsically slow *from the
board* (~29 s per exchange — that much radio airtime visibly disturbs the RGB
panel), while the same exchanges take ~0.3 s from a host. The board therefore
makes one sub-second plain-HTTP GET per 5 minutes; the relay caches upstream
(30 s) and serves a tiny JSON including the hourly history the Envoy's local
API doesn't provide (it has no time-series endpoint — the relay builds the
per-hour buckets itself, 60-s sampling, persisted across restarts).

## Layout

```
firmware/            PlatformIO project for the board (the "lvgl-demo" dir
                     grew up; renamed in the 2026-11-15 cleanup)
├── platformio.ini   board config: 16 MB flash, OPI PSRAM, 921600-baud console
├── src/             application (main.cpp, dashboard_ui.cpp, dashboard_data.cpp)
├── include/         lv_conf.h + secrets.h (gitignored) + secrets_example.h
├── lib/             vendored: LVGL 8.4, ESP32_Display_Panel, IO expander, CH343
└── hostsim/         macOS SDL2 simulator of dashboard_ui.cpp + snapshot tooling

relay/               solar_relay.py — Envoy poller + hourly-bucket builder +
                     tiny HTTP server (stdlib only; RELAY_KEY auth)
docs/                NOTES.md — the full bring-up record, experiments, gotchas
demos/               Waveshare official examples clone (reference only, untracked)
```

## Quick start

Firmware (macOS):

    brew install platformio
    cp firmware/include/secrets_example.h firmware/include/secrets.h   # fill in
    cd firmware && pio run -t upload          # UART Type-C port (see NOTES.md)

Relay:

    python3 relay/solar_relay.py              # ENVOY_HOST/ENVOY_TOKEN via env
                                              # or ~/Code/enphase-test/.env

`secrets.h` carries Wi-Fi credentials, the Envoy bearer token, and the relay
URL/key — it is gitignored and never committed.

## Status

- [x] Bring-up (screen, touch, flashing) — 2024-11
- [x] LVGL dashboard UI (Solar tab live; GitHub tab designed but hidden until
      it gets a data path)
- [x] Wi-Fi with a radio-quiet profile (`setSleep(false)`, owned connect/backoff
      cadence) — panel-noise saga closed out in docs/NOTES.md
- [x] Live solar telemetry via the relay (current W, today kWh, daily peak)
- [x] 24-hour hourly bar graph (relay-built buckets, persisted daily history)
- [x] Wall clock: TZ-correct, synced from the relay (SNTP wedges the loop —
      see NOTES)
- [ ] Relay as a managed service (launchd/Pi) — currently a nohup process
- [ ] GitHub "Web Stats" tab (blocked on WAN TLS filtering; route via relay)

See [docs/NOTES.md](docs/NOTES.md) for the full record: board gotchas (CAN-muxed
USB, UART slide switch, 16 MB flash override), the noise experiments (E1-E6),
TLS characterization, and the relay design decisions.

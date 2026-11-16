# Solar Ops Dashboard

A wall-mounted solar-telemetry HMI for the **Waveshare ESP32-S3-Touch-LCD-7**
(800×480, 7", RGB + touch), built with PlatformIO / arduino-esp32 / LVGL 8.
The board shows live production from an Enphase IQ Gateway — current power,
today's yield, daily peak, and a 24-hour generation bar graph — over Wi-Fi,
with a wall clock synced without SNTP. **Self-contained: no relay or second
host required.**

## Architecture

```
                    LAN (direct TLS, self-signed, bearer token)
 Enphase IQ Gateway ───────────────────────────────────────────>  ESP32-S3 + LVGL
                                                                   (5-min poll
                                                                    cadence,
                                                                    ~13 s radio
                                                                    per fetch)
```

The board polls the Envoy itself over TLS every 5 minutes and keeps the
hourly bar graph in NVS, so nothing else needs to be running for the panel
to work. An earlier iteration routed data through a relay host (the Envoy's
TLS is slow from the board, and radio airtime perturbs the RGB panel); the
panel-noise root cause was later fixed on the board itself
(RGB PCLK 16→13 MHz plus `WiFi.setSleep(false)` — the full saga is in
docs/NOTES.md), which made the direct path viable. Fetch windows are now
visibly quiet, user-verified.

Hourly buckets are computed on-board as deltas of the Envoy's cumulative
`whToday` counter (hour-start baseline, live partial for the current hour,
gap-gated across outages), persisted as a single versioned NVS snapshot
with converters for all prior formats.

The old relay remains as **dormant, compile-time-optional code**
(`SOLAR_POLL_VIA_RELAY=1` in `src/dashboard_data.h`): a stdlib-only Python
poller + bucket builder + tiny HTTP server, kept as the seed of a possible
future push service. Nothing runs it.

## Layout

```
firmware/            PlatformIO project for the board (the "lvgl-demo" dir
                     grew up; renamed in the 2026-11-15 cleanup)
├── platformio.ini   board config: 16 MB flash, OPI PSRAM, 921600-baud console
├── src/             application (main.cpp, dashboard_ui.cpp, dashboard_data.cpp)
├── include/         lv_conf.h + secrets.h (gitignored) + secrets_example.h
├── lib/             vendored: LVGL 8.4, ESP32_Display_Panel, IO expander, CH343
└── hostsim/         macOS SDL2 simulator of dashboard_ui.cpp + snapshot tooling

relay/               solar_relay.py — dormant, optional (see above)
docs/                NOTES.md — the full bring-up record, experiments, gotchas
demos/               Waveshare official examples clone (reference only, untracked)
```

## Quick start

Firmware (macOS):

    brew install platformio
    cp firmware/include/secrets_example.h firmware/include/secrets.h   # fill in
    cd firmware && pio run -t upload          # UART Type-C port (see NOTES.md)

`secrets.h` carries Wi-Fi credentials and the Envoy host + bearer token
(relay URL/key only if you enable the optional relay path) — it is gitignored
and never committed.

The wall clock seeds from a build stamp regenerated on every build
(`pre_build_stamp.py`); a `TIME` command over the 921600-baud serial console
is the manual fallback.

## Status

- [x] Bring-up (screen, touch, flashing) — 2024-11
- [x] LVGL dashboard UI (Solar tab live; GitHub tab designed but hidden until
      it gets a data path)
- [x] Wi-Fi with a radio-quiet profile (`setSleep(false)`, owned connect/backoff
      cadence, RGB PCLK 13 MHz) — panel-noise saga closed, user-verified
- [x] Live solar telemetry, direct from the Envoy (current W, today kWh,
      daily peak; optional relay path still compiles)
- [x] 24-hour hourly bar graph (on-board buckets, NVS-persisted, versioned
      snapshot with migrations for prior formats)
- [x] Wall clock: TZ-correct, build-stamp-seeded (SNTP wedges the loop —
      see NOTES)
- [x] Host-side unit tests (`pio test -e native`, 22 cases in <1 s):
      hourly buckets + migrations (solar_hist) and wall clock + DST +
      seed policy (wall_clock) — every logic path that can silently corrupt
      the persisted history is pinned
- [ ] GitHub "Web Stats" tab (blocked on WAN TLS filtering from the board)

See [docs/NOTES.md](docs/NOTES.md) for the full record: board gotchas (CAN-muxed
USB, UART slide switch, 16 MB flash override), the noise experiments (E1-E6),
TLS characterization, and the relay→direct-mode design decisions.

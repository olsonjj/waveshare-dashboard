# ESP32-S3-Touch-LCD-7 — Bring-up Notes (macOS)

Board: Waveshare ESP32-S3-Touch-LCD-7 (SKU 27078) — ESP32-S3 N16R8 (16 MB flash / 8 MB OPI PSRAM),
7" 800×480 RGB IPS (ST7262), GT911 capacitive touch, CH422G I/O expander, CH343 USB-UART bridge.
Date: 2025-11-10

## Working project

- **`~/Code/waveshare/lvgl-demo`** — PlatformIO (Arduino framework) project running the LVGL v8
  widgets demo with touch + FPS overlay. Status: ✔ boots, ✔ screen, ✔ touch, ✔ measured ~FPS on overlay.
- **`~/Code/waveshare/demos`** — clone of https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7
  (official examples + prebuilt libraries, referenced but not modified).

## Toolchain (as installed on this Mac)

- PlatformIO Core 6.2.0 via Homebrew (`brew install platformio`), VS Code extension `platformio.platformio-ide`
- Platform `espressif32 @ 8.0.0` (arduino-esp32 3.3.x) — note: Waveshare docs mention Arduino core 3.0.7 in the
  IDE guide, but the shipped `ESP32_Display_Panel v1.0.0` library requires arduino-esp32 **≥ 3.1.0**
- esptool v5.4.0 (PlatformIO-managed)
- `7zz` (brew install 7zip) — used to extract `LVGL_v8.4.0.7z` from the Waveshare example package
- Libraries are vendored in `lvgl-demo/lib/` (copied from the Waveshare repo, so builds are reproducible):

  | Library | Version | Source |
  |---|---|---|
  | ESP32_Display_Panel | 1.0.0 | Waveshare repo `examples/Arduino/libraries` |
  | ESP32_IO_Expander | 1.0.1 | '' |
  | esp-lib-utils | 0.1.2 | '' |
  | lvgl | 8.4.0 | `LVGL_v8.4.0.7z` from the same folder |

## Physical board gotchas (each one cost us an hour!)

1. **Two Type-C ports, very different personalities**
   - `USB` (native ESP32 USB): **disabled by default!** Its data lines are *multiplexed with CAN* and
     selected by CH422G expander pin **EXIO5** (default pulled high = CAN mode → the computer sees no
     USB device at all). Only a running firmware can switch it to USB mode. Do not rely on it for flashing.
   - `UART` (CH343 bridge): **this is the flashing/logging port.** Enumerates on macOS as
     `/dev/cu.usbmodem5B901265491` (WCH VID 0x1A86, "USB Single Serial", CDC mode).
     **It also powers the board** (verified). Single-cable development works: Mac → `UART` port only.
2. **UART1/UART2 slide switch** (silkscreen `UART1  UART2`, bottom edge near the BOOT button —
   actually a black slider, not a labelled DIP block): must be at **UART1** or the CH343 enumerates but
   carries *zero* data. This caused all our "No serial data received" failures. **Leave it at UART1 forever.**
3. **Boot/download mode**: hold **BOOT** → tap **RESET** (or power off/on) → release BOOT.
   Screen goes dark = in download mode. Only needed if auto-reset misbehaves; after the switch fix the
   auto-download via RTS/DTR works and `pio run -t upload` connects by itself.
4. Red herring: `/dev/cu.usbmodem302NTJJ2K2122` is the **LG monitor's** CDC interface, not the board.

## Software/project gotchas

1. **`board_upload.flash_size = 16MB` is the correct override** in PlatformIO espressif32 8.0.0.
   (`board_build.flash_size` is silently ignored → images stamped 8 MB → bootloader rejects the 16 MB
   partition table → boot crash loop: `partition 3 invalid ... exceeds flash chip size 0x800000`.)
   Sanity check the image header: `xxd -l 4 bootloader.bin` → byte 3 `0x4f` = 16 MB @ 80 MHz.
2. `board_build.arduino.memory_type = qio_opi` + `board_build.psram_type = opi` (for the R8 PSRAM).
3. **`lv_conf.h` is found ONLY if it lies at `lib/lv_conf.h`** (LVGL's default relative include from
   `lib/lvgl/src` = two levels up) — `-DLV_CONF_INCLUDE_SIMPLE` + project `include/` did NOT work for
   lib-scope sources in this setup. Both copies exist (`lib/lv_conf.h` + `include/lv_conf.h`), keep them in sync.
4. Two Waveshare conf headers must be present for ESP32_Display_Panel:
   - `esp_panel_board_custom_conf.h` (board: Waveshare:ESP32-S3-Touch-LCD-7, ST7262 RGB 21 MHz, GT911, CH422G)
     → **detected only from `lib/esp_panel_board_custom_conf.h`** (the `__has_include` fallback resolves to
     the `lib/` directory). Symptom if missing: `No default board configuration detected`.
   - Both files currently in `lvgl-demo/src/` AND `lvgl-demo/lib/` (src copies are inert but harmless).
   - PCLK set to **21 MHz** (from 16 MHz) for smoother UI (Waveshare's own ESP-IDF perf-note value).
5. **Console/log routing**: with `ARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` all Serial output goes
   to the native USB port — which is CAN-muxed off → invisible. Use `-DARDUINO_USB_CDC_ON_BOOT=0`
   (current setting) so `Serial` = UART0 = the CH343 port we can read. `CORE_DEBUG_LEVEL=3` (info) is on.
6. LVGL `lv_timer_handler()` is called **only inside the adapter task** (esp_lv_adapter) — do not call it
   from Arduino `loop()` (thread-safety). The `loop()` here only prints a 2-second heartbeat.
7. LVGL perf overlay: `LV_USE_PERF_MONITOR 1` in `lv_conf.h` (bottom-right FPS/CPU readout).

## Command cheat-sheet

```bash
cd ~/Code/waveshare/lvgl-demo
pio run                                            # build
pio run -t upload --upload-port /dev/cu.usbmodem5B901265491   # flash (auto-reset works now)
# if esptool can't connect: hold BOOT, tap RESET, keep holding during the upload attempt

# console (one-shot capture; heartbeat prints every ~2 s)
timeout 20 ~/.platformio/penv/bin/python - <<'EOF'
import serial, time
s = serial.Serial('/dev/cu.usbmodem5B901265491', 115200, timeout=2)
t0=time.time(); buf=b''
while time.time()-t0 < 18: buf += s.read(4096)
print(buf.decode('utf-8','replace'))
EOF

# emergency full-flash at 115200 (when partial states glitch)
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodem5B901265491 --baud 115200 \
  --before default_reset --after hard_reset write_flash \
  0x0000 .pio/build/waveshare-esp32-s3-lcd7/bootloader.bin \
  0x8000 .pio/build/waveshare-esp32-s3-lcd7/partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/waveshare-esp32-s3-lcd7/firmware.bin
```

Note: `pio`'s esptool wrapper sometimes needs the pyserial of the PlatformIO env (`~/.platformio/penv/bin/python`).

## Performance baseline (measured)

- Widgets demo @ PCLK 21 MHz, default adapter task (prio 2, core 1): **~33 FPS, LVGL “CPU” ~99%**
  (CPU% = LVGL busy-time, not system-wide — expected with a demo that animates everything).
- v2 dashboard with `LV_DISP_DEF_REFR_PERIOD` 30 → **20 ms** (2025-11-14, user-verified on device):
  interaction FPS 33 → **50** and noticeably faster tab switching; theoretical refresh cap = 1000/20.
  At rest nothing changes (LVGL only wakes on invalidations). Keep in sync across all lv_conf copies.
- Widgets demo @ priority 4/core 0: regressed to ~3 FPS with tearing (core-0 contention, see above).
  Known-good LVGL task config: priority 2, core = ARDUINO_RUNNING_CORE(1).
- **Custom dashboard (dashboard_ui.cpp): 4–5 FPS, low CPU — this is healthy.** LVGL redraws only on change;
  the 2 s mock-data timer drives a few dirty-region updates/s. FPS spikes during touch/animation.
  Don't chase FPS on static content — judge by *interaction responsiveness* instead.
- Reference: Waveshare ESP-IDF v5.3 LVGL benchmark on this board: 26 FPS avg, 41 FPS interface @ 21 MHz.

## Project state (v1)

- `lvgl-demo/src/main.cpp` — instrumented bring-up (stage markers [s1..s4] + 2 s heartbeat in loop())
- `lvgl-demo/src/dashboard_ui.cpp/.h` — **two-tab dashboard, dark theme**:
  Solar tab (arc gauge + 24 h chart + battery + energy labels; 2 s mock random-walk timer) and
  Web Stats tab (3 status cards + weekly commits bar chart). Fonts enabled: 12/14/16/26/30 + 20/24/32/48.
- Build ~8–30 s, flash ~40 s over single UART cable, no button presses (auto-reset).
- Next: real data sources (Wi-Fi → GitHub REST / MQTT / Home Assistant), then polish + deploy.

## Debug log decode guide (from the CH343 console)

```
LVGL v8 demo start / [s1..s4] stage markers      — our instrumented setup() in src/main.cpp
[E][Panel] No default board configuration …      — esp_panel_board_custom_conf.h not detected (see gotcha 4)
[E][flash_parts] partition … exceeds …0x800000   — image stamped with wrong flash size (see gotcha 1)
rst:0x3 (RTC_SW_SYS_RST) loop + boot errors      — crash loop at bootloader stage
[hb] alive t=…                                   — application loop healthy
[E][esp_lcd_touch_gt911] Unable to initialize…   — benign warning (recovers; touchpad ID read OK)
```

## Ideas / next steps

*(v1-era list, kept for the ones still relevant; stale items pruned 2025-11-14)*

*The truly evergreen ones (still true, kept from v1):*

- [ ] Keep `LV_USE_PERF_MONITOR` while iterating visuals (on screen bottom-right).
- [ ] **Do NOT** move the LVGL adapter task to core 0 or raise its priority: tested priority 4/core 0 →
      screen refresh starves (tearing/jumping, 3 FPS) because the RGB bounce-buffer/refresh path lives on core 0.
      Known-good: priority 2, core = ARDUINO_RUNNING_CORE(1).
- [ ] Decide whether native-USB CDC logs are worth it (would require EXIO5 handling + second cable for power).
- [ ] Serial monitor: VS Code command `PlatformIO: Monitor` or `pio device monitor --port /dev/cu.usbmodem5B901265491 -f esp32_exception_decoder`.
- [x] ~~Replace `lv_demo_widgets()` with first custom UI~~ done in v2 (2025-11-14).

## Simulator gotchas

- `hostsim/build.sh` C++ compiles **must keep `-c`** — without it clang++ compiles *and* attempts to
  link, then fails with undefined `lv_*` symbols (and the object never lands → confusing
  "_main" link errors later).
- `sim` binary + `hostsim/*.raw` / out_tab*.png snapshots are build artifacts — gitignored, not tracked.

## Wall clock (upper-right header)

- Header clock reads the **system time** (`localtime_r`, 1 s tick) with `TZ=PST8PDT,M3.2.0,M11.1.0`
  (DST aware: UTC−8 winter / UTC−7 summer; user in Pacific time).
- No RTC battery and no NTP yet, so `main.cpp` seeds the soft RTC from the **build timestamp**
  (`__DATE__`/`__TIME__`, stamped in host-local Pacific) on every boot — accurate ~1 min after
  flash/power cycle; the soft RTC keeps time across soft resets while powered.
- **Serial sync command** (always active, even with `DBG1_NO_WIFI`): `TIME hh:mm:ss` (keeps today's
  date), `TIME yyyy-mm-dd hh:mm:ss`, `TIME ?` (query). Verified set/query on device 2026-11-14.
- Future: once real WiFi creds exist, add SNTP in `dashboard_data.cpp` (`configTime` + pool) to
  self-correct; the serial command stays as the offline fallback.

## v2 "Solar Ops" design (branch `design-mockup`, 2025-11-14)

Mockup-faithful two-tab dashboard built in `src/dashboard_ui.cpp` (tokens in `src/ui_tokens.h`):

- **Header (52 px)**: hand-rolled tab buttons (not `lv_tabview` — needed badges/pills inside the
  buttons): ☀ SOLAR TELEMETRY + solid-amber `LIVE` pill, GitHub mark + green `2 REPOS` pill, amber
  underline swap on tab select. Right cluster: `Sync: 1s` chip (cyan dot), 3 green signal bars,
  bordered clock chip (1 s timer, starts 14:38:22).
- **Solar tab**: CURRENT POWER card (amber accent bar, `REALTIME` pill, 3.84 kW @48 pt, green ▲
  peak row, 76% LOAD donut `lv_arc`), TOTAL YIELD card (green accent, `+14% vs avg` pill, 21.65 kWh,
  target pill, green→yellow gradient `lv_bar`), chart card (`lv_chart` LINE 48 pts 05:00–20:00,
  under-curve gradient painted into a PSRAM `lv_canvas` sibling placed under the transparent chart,
  dashed grid as canvas children, y/x labels as manual labels, `Max 4.85` marker, dashed cyan
  "now" cursor at 14:38 with tooltip).
- **GitHub tab**: intentionally empty (design only). Tab switching works (touch /
  `dashboard_ui_select_tab()`).

### v2 gotchas (each cost time — read before editing)

1. **Theme padding must be zeroed everywhere** (`pad_all 0` on panels/buttons/chips/canvases) — the
   default theme pads (~16 px) stack per container level and shift/clip everything; layout looks
   fine in `lv_obj_get_coords` but paints off-screen.
2. **`lv_obj_align_to(one-shot)`** computes from current coords (it refreshes layout internally, but
   only at the moment you call it). Safe here because every call site refreshes the screen layout;
   do not move calls before the reference object exists.
3. **Chart series values are in the chart's range units** (0..5000); the mock curve is stored in
   0.01 kW (0..485) — multiply by 10 when filling `y_points`.
4. **No em dash in Montserrat** — `SOLAR GENERATION CURVE — TODAY` with a missing-glyph U+2014 made
   the title label render repeated/looped (treated as scroll-circular). Use `-` / ASCII only, or add
   the glyph via a custom font later.
5. Icons that need glyphs the builtin fonts lack (☀, ▲, octocat) are drawn as tiny
   `LV_IMG_CF_TRUE_COLOR_ALPHA` canvases (`make_sun_icon` / `make_gh_icon` / `make_tri`).
6. `LV_MEM_SIZE` raised 48 KB → **128 KB** in both `lib/lv_conf.h` and `include/lv_conf.h` (still
   in sync, see gotcha 3 in the section above). RAM now 56% (183 KB of 320 KB).

### Glitch diagnosis (2025-11-14, after v2 flash)

**Symptom** (user photo): horizontal streak/ghost rows flickering across the panel — classic RGB
row-phase/tearing.

**Root cause**: `dashboard_data.cpp` failed WiFi attempts and **retried continuously** (driver
`setAutoReconnect(true)` + `WiFi.begin` every 20 s + 10 s blocking delay): scan/handshake bursts starve
the RGB refresh path (core 0, see gotcha list above) while the v2 UI also pushes a 254 KB PSRAM canvas
blit through the same fabric → visible row noise.

**How it was found**: host simulator verified the LVGL layout was pixel-perfect (artifacts only on
device) → one-variable experiments (HITL): E1 = build with the data layer compiled out
(`-DDBG1_NO_WIFI`, guard kept in main.cpp) → **noise gone** (user-confirmed) → cause = data layer.

**Fix**: `dashboard_data.cpp` rewritten state machine: `WIFI_DISABLED` (no credentials in secrets.h →
never touched), `WIFI_CONNECTING` (one 20 s attempt), then `WIFI_BACKOFF` ladder 15 s → 45 s → 2 m →
5 m → 15 m → 30 min with **radio OFF** between attempts (`WiFi.mode(WIFI_OFF)`, driver auto-reconnect
disabled — we own the cadence). Blocking `delay()` removed; heartbeats print again. Verified on serial:
attempt → "retry in 15 s" → quiet.

**Follow-up experiments (same day)**:

- **E2 — interaction stress with radio compiled out**: user confirmed *noise-free* while tap/tab-switching
  rapidly → the render path (PSRAM canvas blit under 20 ms refresh) is healthy; radio bursts are the sole cause.
- **User video (IMG_2622.MOV, 1.5 s @ 30 fps) analyzed with ffmpeg**: per-frame diffs + motion-compensated
  residual show transient **dash-shaped ghost rows** below the tab title = row pixels caught mid-write by the
  panel DMA (classic tearing band artifact — not EMI glitter, not camera noise: global brightness rock-stable,
  residual hugs glyph edges, position flickers frame to frame).
- **Noise returned once during the backoff build**: serial showed attempt 4's connect window active at exactly
  that moment → each radio-ON window still perturbs the panel, even with long quiet gaps in between.

**Final state**: `secrets.h` set to empty SSID/PASS = "not provisioned" → data layer stays fully off (the
proven-quiet E1 state); connect attempt window shortened 20 s → 10 s; the backoff ladder only resumes once
real credentials are entered. When provisioning real WiFi, watch the panel through the first ladder run;
if bursts still bite, mitigations in order: fast-scan, dedicated task pinning, or schedule attempts only at
boot + manual trigger.

### Host simulator (`hostsim/`) — *self-review without hardware*

`hostsim/build.sh snap` compiles the SAME `src/dashboard_ui.cpp` against macOS SDL2 + the vendored
LVGL, runs it in a window, and `hostsim/crop.py` / `raw2png.py` convert framebuffer dumps to PNG.
`hostsim/reference_tab1.png` / `reference_tab2.png` are the final rendered references. Useful because
the chat-image pipeline mangles large/portrait PNGs — verify oddities with `crop.py` + pixel probes
before "fixing" anything. `./sim dump` prints the widget tree with absolute coords.

### Performance learnings from external sources

From reviewing "LVGL Too Slow? Do THIS for Maximum FPS!" (Garage Tinkering,
youtube.com/watch?v=K42VbYoDgjU — ESP32-S3 + LVGL 9 SPI gauge, 5 FPS → ~50 FPS applied):

- ✅ **`LV_DISP_DEF_REFR_PERIOD` 30 → 20 ms** — applied + user-verified (see baseline above); 20 ms was
  his measured sweet spot (~50 FPS); diminishing returns beyond (30 → 100 → 200 FPS cap tested, no visible gain).
- ✅ **Quiet radio cadence** — his ESP-Now bombardment case is the same mechanism as our glitch fix above.
- ⬜ **Frame buffer count 2 → 3** — cheap open experiment if we ever need more; RGB path already DMAs
  full framebuffers from PSRAM (750 KB each, 8 MB PSRAM so it's affordable). Would mainly help during
  full-screen redraws (tab switches). He saw mixed +0–20%.
- ✗ **Full render mode** — rejected: fixes banding/tearing of one moving line but tanks FPS (33 → 2-3).
- ✗ **LVGL 9 migration for scale/meter internals** — real gains (v8 `lv_meter` redraws all ticks when
  the needle moves; v9 scales don't), but we avoid `lv_meter` entirely (plain `lv_arc` + `lv_chart`),
  so the payoff doesn't justify a vendored-stack + adapter migration yet.
- ✗ Revisit if we ever add fast-updating gauge widgets: keep per-screen "active" (fast-changing)
  components few — our dashboard is deliberately passive (clock tick, bounded data polls).

Open ideas, cheapest first:
1. Real WiFi credentials in `include/secrets.h` (gitignored) → connected-state fetches are one brief
   HTTPS transaction/minute; if panel noise ever reappears while connected, that's the thing to bisect.
2. Move fetches to a dedicated low-priority task if that poll ever shows up in the FPS overlay.

## Real-data session (2026-11-15, branch `real-data`)

Goal: real WiFi creds in secrets.h → Enphase solar data on the Solar tab.

### WiFi radio + panel noise — final picture (experiments E3–E6)

* **E3** (radio on, fetches compiled out): quiet ~90 s, then pulses returned → connected-idle radio alone is NOT fully quiet.
* **E4** (`WiFi.setSleep(false)` — modem power-save OFF): **quiet 5+ min, user-verified.** Beacon-wakeup bursts were the connected-idle noise source. `setSleep(false)` is permanent policy now (wall-powered board).
* **E5** (fetches on): noise returns exactly during sustained TLS radio activity. Noise is proportional to **radio airtime**, not print volume.
* **E6** (UART hardening: 921600 baud + 4 KB TX ring + `esp_log_level_set("wifi"/"wifi_init"/"phy", NONE)` + CORE_DEBUG_LEVEL=1): noise **unchanged** → serial/UART theory dead. (User also ran board with UART unplugged, USB-C power: same.)
* Escalation ladder unexercised (fast-scan / task pinning / PCLK down) — keep as levers. PCLK effective value is **16 MHz** in `lib/esp_panel_board_custom_conf.h` (the 21 MHz note refers to the inert `src/` copy — config drift, fix when convenient). Bounce buffers ON (width×10).

### TLS findings (board-side fully exonerated)

* arduino-esp32 3.3 `NetworkClientSecure`: `setHandshakeTimeout` is a watchdog checked *between* handshake records; real per-I/O bound is `setTimeout()` (SO_RCVTIMEO). Worst wall ≈ DNS + connect-select + 1–2 blocking recvs (see /tmp/tls_research.md).
* `lastError` "UNKNOWN ERROR CODE (0050)" after a *successful* socket = fd number artifact, ignore. `CONN_EOF` = peer FIN during handshake.
* **Board TLS proven healthy**: full handshake to a local test server incl. 5.3 KB cert chain, real x509 verification, correct alert on bad cert. Board's 292-B ClientHello captured and verified well-formed (mbedtls TLS1.2, 81 ciphers, SNI).
* **WAN TLS to GitHub/Google fails** (EOF ~40–70 s) from BOTH SSIDs, while Mac TLS works from the same network and even the board's exact ClientHello replayed from the Mac gets a server response. Network-side filtering that singles out this device's flows — **unresolved, deprioritized** (relay path makes it moot for solar; revisit for the GitHub tab).
* Amplifi Alien: main SSID `YapioHome` (192.168.182.x) vs `YapioHome-IoT` (192.168.251.x) are isolated segments (cross-segment TCP refused in ~8 ms; ping passes). Board currently on main.

### Enphase Envoy integration (the Solar tab's data source)

* Gateway: IQ Gateway D8.3.5433 at `192.168.182.102`, self-signed TLS + Bearer token (see `~/Code/enphase-test/ENPHASE-LOCAL-API.md`; creds in `~/Code/enphase-test/.env` and now `lvgl-demo/include/secrets.h`).
* Source of truth: `GET /ivp/pdm/energy` → `production.pcu` → `wattsNow`, `wattHoursToday`.
* **Envoy ↔ board is intrinsically slow**: TLS handshake ~26–29 s, GET phase ~29 s (713-B response!), every attempt, RSSI -44 dBm. Envoy closes the connection within 60 s idle (keep-alive reuse gives nothing at a 60-s poll). Same exchanges from the Mac: ~0.3 s. Board↔Mac TLS: 1.1 s. Suspected Envoy-side client throttling of unknown devices; root cause not chased (relay sidesteps it).
* Direct fetch DID work end-to-end (real data on the panel) but = ~56 s radio/min = constant panel noise.

### Relay architecture (adopted)

`relay/solar_relay.py` — runs on an always-on Mac/Pi: fetches the Envoy (fast path, 30-s cache) and serves `GET /solar` → `{watts_now, wh_today, watt_hours_lifetime, age_s}` over plain LAN HTTP:8420 with `X-Relay-Key` shared secret. Board's poll becomes a ~1-s LAN blip.
* Board wiring TODO: plain-HTTP fetcher in `dashboard_data.cpp` + `RELAY_URL`/`RELAY_KEY` in secrets.h; keep the Envoy TLS fetcher as fallback.

### Serial console changes

* Baud now **921600** (was 115200) — CH343P handles it; 4 KB TX ring set before `Serial.begin`. Update pyserial one-shots accordingly.
* New serial commands: `FETCH` (one solar fetch now), `PROBE host[:port]` (443=TLS probe w/ CA bundle, 80=plain TCP HEAD, `RSSI`=signal), `PROBE RAW:host:port` (connect + minimal TLS-prefix bytes + 60-s connection-state watch), `PROBE BIG:example.com:80` (1.1 KB TX + response timing).
* `dashboard_data_fetch_now()` public API added for FETCH.

### Firmware state on branch `real-data` (uncommitted at time of writing)

* Solar tab: `dashboard_ui_set_solar(watts, wh_today)` wired to CURRENT POWER + TOTAL YIELD cards (was stubbed/mock).
* Poller: Enphase fetch every 60 s (direct, slow) — to be switched to relay.
* SNTP added but **TZ bug**: `configTime(0,0,...)` clobbered TZ → clock shows UTC. Fix: `configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", ...)`.

### Session wrap-up (2026-11-15)

* **SNTP was the once-per-boot loop wedge** (24–40 s freeze right after the first fetch): present in every build with `configTzTime`/`configTime`, absent in every build without. Wall clock now syncs from the **relay's `epoch` field** (relay host is NTP-synced; board calls `settimeofday` when drift > 5 s, TZ from `main.cpp` stays authoritative). SNTP stays disabled; serial `TIME` remains the offline fallback. User-verified: clock correct (`11:03:44 PST`), no stall, clean idle cadence from t=10 s.
* **Relay adopted as the scheduled poll path**: board → `relay/solar_relay.py` (Mac, `http://<mac>:8420/solar`, `X-Relay-Key`) → Envoy. Fetch ~1.7 s every 60 s (~3% radio duty). Direct Envoy TLS fetcher kept in firmware as fallback/diagnostic (unscheduled).
* First relay-era boots: no noise reported during the short polls (panel-quiet confirmation pending user check at wrap-up).
* Uncommitted-work risk noted: commit after this wrap-up.

### Project cleanup (2026-11-15)

`lvgl-demo/` renamed to `firmware/` (the directory outgrew its name), `NOTES.md`
moved to `docs/NOTES.md`, README rewritten to describe the actual system
(board + relay architecture). `relay/solar_history.json` untracked (runtime
state, now gitignored). Board banner now prints "solar dashboard start/ready".
Historical entries above still say `lvgl-demo/` — that's the path at the time;
the hostsim scripts are path-relative and unaffected.

### Direct mode: drop the relay dependency (2026-11-15, evaluation in progress)

User decision: the dashboard should not depend on a second always-on service;
if one ever returns, it will be a push (MQTT) design, not the poll relay.

Firmware now has both poll paths compiled behind SOLAR_POLL_VIA_RELAY
(dashboard_data.h, currently 0 = DIRECT):
- DIRECT: board fetches the Envoy itself (TLS, keep-alive session kept across
  polls); second GET on the same session pulls the inverters endpoint for the
  daily peak. Hourly buckets are built ON-BOARD from the 5-min poll samples
  and persisted in NVS (Preferences, namespace "solarhist") - reboots mid-day
  keep the chart. Wall clock: RTC seeded from build stamp ONLY when behind it
  (main.cpp) so reboots don't rewind time; serial TIME still corrects.
- RELAY: previous behavior intact behind the macro (relay/solar_relay.py
  stays in the repo as the seed of any future push service).

First direct fetch: handshake 8.7 s + GET 17.3 s + inverters 0.35 s = ~26 s
radio window per 5-min poll (better than the 21-29 s/phase measured earlier;
variance unexplained). Panel-noise evaluation at stock PCLK ongoing; next
levers ready: PCLK 16->13, bounce x20, then TLS-slowdown root-cause.

### Greptile round 2 + stale build-stamp discovery (2026-11-15)

Round-2 review flagged second-order consequences of the straddle-carry fix:
long poll outages would dump all missed production into the recovery hour's
bar (both paths). Fixed with a gap gate: last_ts (epoch of last accepted
sample) is persisted; the carry only fires on an adjacent hour boundary with
a fresh sample (<=900 s direct / <=180 s relay), else the hour starts at the
current reading and missed hours stay -1. Plus: one-time migration of the
legacy five-key NVS snapshot into the HistSnap blob (ver 2).

Clock bug found while validating: __DATE__/__TIME__ carry the .o COMPILE time,
so incremental builds re-flashed a stale stamp - the board clock ended up
1.5 h behind after a main.cpp-untouched rebuild. Fixed with
pre_build_stamp.py (PIO extra_script): include/build_stamp.h is regenerated
with the host time every build, forcing a fresh seed stamp. Verified: boot
seeded to within seconds of real time; snapshot loaded across the reflash.

### Noise saga CLOSED: PCLK 13 MHz (2026-11-15, user-verified)

Direct-mode fetches at stock PCLK 16 MHz still showed horizontal glitching
during the ~13-30 s TLS windows. Pulled the documented escalation lever:
RGB PCLK 16 -> 13 MHz (both esp_panel_board_custom_conf.h copies, killing
the known 16-vs-21-MHz drift). Result: three consecutive 5-min poll cycles
(~13-14 s of radio each, serial-timestamped) observed by the user with ZERO
screen corruption. Bonus: fetch GET phases got ~3x faster (~6 s vs ~17 s) -
radio and the PSRAM bus were contending more than we knew.

Final noise model, the whole story:
- noise tracks radio airtime + memory-bus pressure, not print volume
- modem sleep wakeups: fixed permanently by setSleep(false)
- fetch bursts: fixed by PCLK 13 (slack in the PSRAM bus beats fighting it)
- direct mode is now VIABLE: self-contained board, no relay dependency,
  ~13 s radio per 5-min poll, quiet panel, user-verified
The refresh rate at 13 MHz is ~81% of 16 MHz - imperceptible on this
dashboard. Bounce x20 lever remains unexercised (spare, documented).

### Ghosting round 2: instrumented (2026-11-17, session)

New "horizontal glitch lines" reports (board on wall USB-C power only, UART
disconnected). Built a full observation stack this session:

* **On-board serial commands** (main.cpp): `SNAPSHOT` dumps both RGB panel
  framebuffers (B0/B1, what the panel DMA is fed) over UART; `LVSNAP` dumps
  LVGL's own render (`lv_snapshot_take_to_buf`, PSRAM scratch — LVGL's 128 KB
  pool can't hold a frame; LV_USE_SNAPSHOT now 1 in both lv_conf.h copies).
  Frames are base64 + FNV1A-checked. Adapter gained
  `esp_lv_adapter_get_panel_framebuffers()` under `src/`.
* **Host tools** (`firmware/tools/`): `glitch_watch.py` owns the CH343 port —
  logs all serial traffic, polls SNAPSHOT every 90 s + LVSNAP every 5 min,
  diffs vs previous capture (clock mask), PNGs, and cross-triggers on camera
  events. `cam_watch.py` watches the panel with the eMeet C960 webcam
  (30 fps, full-res gray, exposure-gain-normalized row-diff ghost detector;
  on trigger: 8-frame burst + triggers an immediate synchronized buffer dump).
  State lives in `~/.glitchwatch/` (events.log, cam_events.log, serial.log,
  snaps/, cam/). GLITCHWATCH_DIR overrides.
* **UART CDC gotcha**: the CH343/CDC link is NOT binary-safe — raw dumps lose
  0x13 bytes silently (flow-control artifact). Everything binary must travel
  base64. Burned ~30 min; documented here and in tool headers.

Findings of the night:

* **Lens-flare false positive**: with the webcam CLOSE to the panel in a dark
  room, bright displaced translucent "ghost" copies of UI text ("0.00",
  "21.9") appear in the camera and are entirely camera optics (internal lens
  flare) — invisible to the naked eye, absent from framebuffers, gone when
  the camera moved back. Rule: photograph the panel from >= 2 ft, square-on.
* **Real ghost episode caught with full instrumentation (20:52)**: user saw
  dash-ghost rows under the tab title (same signature as 17:14 phone stills).
  Synchronized B0/B1 dumps taken during the episode (panel frozen, buffers
  static) are PIXEL-CLEAN. => artifact is generated BELOW the framebuffer, in
  the RGB scan path (flat cable/connector, RGB timing margin, or noise
  coupling). Firmware write path, LVGL state, and radio exonerated for this
  class. Heap telemetry at every poll: heap 79360->79352 (~2.5 h), psram +
  largest-block constant => heap fragmentation empirically ruled out.
* Ghost occurred during IDLE (poll was 1 min later; fetch ended 20:48) —
  NOT the fetch-window radio noise (that saga stays closed). This is the
  "appears after some time" phenomenon.
* Touch/wiggle of cable: no visible response (inconclusive for connector).

Leading hypotheses for tomorrow: thermal buildup (backlight heating shifts
RGB margin — episode ~3.3 h after boot; watch for onset vs power-on clock
after tonight's 21:03 reboot), or PSU sag/ripple (charger swap still not
done — TODO). Environmental-EMI theory (desk dock/monitor) not fully dead:
if onset follows wall-clock rather than power-on, revisit.

### Session handoff (2026-11-18 morning)

Session ended ~10:30 with the artifact still unexplained but the search space
much reduced. Cold-start summary, evidence index, and next steps live in the
local file
`docs/GHOSTING_INVESTIGATION.md` (INTENTIONALLY UNCOMMITTED: repo-local
working notes, not for the public/documented tree; ask John for it). Watchers left running: glitch_watch.py
(serial+buffer dumps, continuous); cam_watch.py stood down (user covered the
camera). Board rebooted 06:14; ran clean through 10:13 (4h) on the same
charger — PSU swap is the top pending test.

## 2026-11-18 evening: poll → push rearchitecture implemented

Per the §8 root cause (RGB LCD scan-out vs WiFi radio airtime on the shared
Octal-SPI bus + core 0; direct mode unsalvageable — no downward PCLK margin,
×20 bounce buffer insufficient), the dashboard moved from "board polls" to
"host pushes":

- **`server/` (new, Rust)**: polls the Envoy every 60 s (bearer token in a
  gitignored `server/config.toml` — no longer in any firmware file),
  accumulates today's 15-minute buckets (96 per day; direct port of the
  retired `solar_hist` state machine: straddle carry, 180 s gap gate,
  midnight rollover — the analog of the old 900 s gate at the new 60 s
  cadence), persists today's state in SQLite (`server/data/solar.db`, one
  table, restart mid-day resumes without double-counting), and pushes the
  complete screen state as newline-delimited JSON over TCP :8421 every
  second. Contract is versioned (`"v":1`, `server/src/contract.rs`); the
  pushed `epoch` is the board's only clock source.
- **Board**: `dashboard_data.cpp` (Envoy TLS + relay poll + GitHub fetch),
  `solar_hist`, `wall_clock` (build-stamp seed + serial TIME), NVS snapshot
  paths — all deleted. New `push_client.cpp` (~250 lines): radio-quiet
  WiFi connect (same backoff ladder) + TCP line reader + ArduinoJson into
  the existing renderer. Chart is now 96 15-minute bars
  (`dashboard_ui_set_chart`). Peak = max watts observed today, tracked
  server-side.
- **`relay/` deleted** (its runtime history file was already gitignored
  data). The `pio test -e native` Unity harness is retired with `solar_hist`
  / `wall_clock`; the Rust suite (`just test`) mirrors its case list.
- Smoke-tested against the real Envoy and the board: the push-demo build
  board connected to :8421 and the first poll line read
  `0 W, 21121 Wh today, 7d 125721 Wh`.

Radio budget check: board airtime is now ~1 s WiFi connect at boot plus
millisecond receives of ~900 B lines once per second — the state the §8
push-demo experiment proved clean.

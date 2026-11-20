# Screen "Ghosting" Investigation — Status & Record

*Written 2026-11-18 ~10:30 AM, end of session. Companion to `docs/NOTES.md`
(which has the full diary entries); this file is the cold-start summary.*

*(§6-8 below were appended later on 2026-11-18: §6 after the A/B exonerated
the firmware, §7 as the evening onsets falsified each successive trigger
theory, §8 as the evening's one-variable experiments narrowed the cause to
RGB-LCD/WiFi radio-airtime contention and set up the push rearchitecture.)*
## 1. What we built (this session, and reusable)

| Component | What it gives us |
|---|---|
| `SNAPSHOT` serial cmd (main.cpp) | base64+FNV1A-checked dump of **both RGB panel framebuffers (B0/B1)** — the pixels the panel DMA is actually fed |
| `LVSNAP` serial cmd | LVGL's own render of the active screen (`lv_snapshot_take_to_buf`, PSRAM scratch; LV_USE_SNAPSHOT=1 in both lv_conf.h copies) |
| `esp_lv_adapter_get_panel_framebuffers()` (src/esp_lv_adapter_arduino.cpp) | addresses of the two scan buffers |
| `firmware/tools/glitch_watch.py` | owns CH343 UART: logs all serial (timestamped), SNAPSHOT every 90 s, LVSNAP every 5 min, row-wise diffs vs previous capture (clock-region mask), PNG output; auto-reconnects; receives cross-triggers from the camera |
| `firmware/tools/cam_watch.py` | webcam (eMeet C960) at 1920×1080/30 fps grayscale: exposure-gain-normalized per-row diff ghost detector, 8-frame bursts on trigger, writes a trigger file that makes glitch_watch snapshot **synchronously** |
| Data state | `~/.glitchwatch/` — events.log, cam_events.log, serial.log, snaps/, cam/, evidence/. `GLITCHWATCH_DIR` env overrides |

**Triage matrix** (what a simultaneous eyes+camera+buffer capture decides):

| lv_snapshot | B0/B1 dumps | panel shows ghosts | verdict |
|---|---|---|---|
| clean | clean | yes | physics: RGB scan path (cable/timing/EMI) — software exonerated |
| clean | ghosted | yes | firmware write path (blit/DMA) |
| ghosted | ghosted | yes | LVGL draw-state corruption |

**Gotchas found:**
- The CH343/CDC UART is **not binary-safe**: raw dumps silently lose 0x13
  bytes (flow-control artifact). All binary payload must be base64-encoded.
- `lv_snapshot_take()` allocates from LVGL's 128 KB pool → use
  `lv_snapshot_take_to_buf()` with a PSRAM buffer instead.
- **Photograph the panel from ≥2 ft, square-on.** Close-up camera in a dark
  bright-lit room manufactures convincing-but-fake "ghost text" via internal
  lens flare (see §2). Phone stills taken at an angle are suspect for the
  same reason.

## 2. Settled: what the artifact is NOT

Captured 2026-11-17 ~20:52, with the user's eyes + webcam + **frozen
framebuffers dumped byte-verified clean** in the same seconds: ghost dash
rows under the tab title (same signature as the user's 17:14 phone stills).
Verified in B0 *and* B1. Therefore **the artifact is generated below the
framebuffer, in the RGB scan path** (display flat cable/connector, RGB
timing/clocking margin, or noise coupling onto those lines). Evidence:
`~/.glitchwatch/evidence/` (eye_1_f*.jpg camera frames, eye_buf_B0/B1.png,
205207_B0.raw, 205218_B1.raw).

Ruled out by direct measurement:
- **Heap fragmentation / memory state**: telemetry every poll across 9.1 h
  overnight: heap 79,372→79,024 (trivial), PSRAM free + largest-block
  byte-identical. JSON/HTTP/LVGL-churn theories dead.
- **LVGL/draw-state corruption**: B0+B1+LV dumps clean during episodes.
- **Radio activity**: the 20:52 episode happened during IDLE (fetch ended
  20:48, next poll 20:53:44); overnight 9.1 h had zero WiFi
  reconnects/SLOW/FAILs. The original fetch-window noise (NOTES.md saga)
  remains fixed and is a different mechanism.
- **Camera optics (disguised as the artifact)**: the strong displaced
  text-ghost overlays ("0.00"/"21.9" copies, 18:22–18:29) were **internal
  lens flare** of the close-up webcam — invisible to the naked eye, absent
  from framebuffers, gone when the camera moved to ≥2 ft. Everything
  captured up close before 18:33 must be treated as suspect.
- **Cable wiggle/touch**: no visible response (negative-only evidence;
  doesn't fully exonerate the connector — thermal drift vs touch differ).

## 3. The remaining question: what triggers the scan-path artifact

Candidates, with status as of 2026-11-18 10:13:

| Hypothesis | Status | Test |
|---|---|---|
| PSU sag/ripple (wall charger) | **untested** — swap still pending | swap charger + power cycle; compare onset pattern on the new supply |
| Thermal buildup (backlight heat shifting RGB margin) | partial: episode 1 appeared ~3h21m after power-on (17:31→20:52), but the 06:14→10:13 run (4h00m, same charger) had **no recurrence** — simple boot-clock prediction failed once | note onset time vs power-on across runs; morning/evening rooms differ (confound) |
| Environmental EMI (desk dock/monitor — the user's original suspicion) | not fully dead, now with a plausible mechanism (noise on RGB lanes) | revisit only after PSU/thermal are priced in; testable by moving the board elsewhere |
| Board/panel hardware marginality (ST7262 scan logic, FPC) | possible fallback | PCLK 13→10 MHz as *diagnostic* (episode frequency ↓ = timing margin); bounce buffers ×20 lever unexercised |

## 4. Next steps (pick up from here)

1. **Charger swap** (10-s power cycle, never done) → run overnight on the new
   supply; compare onset behavior. If the artifact vanishes on the new
   supply, PSU confirmed.
2. **Redeploy `cam_watch.py` overnight** (it was stood down before this
   morning's onset — that record gap is why today's onset time is unknown):
   `cd firmware && nohup python3 tools/cam_watch.py > /tmp/cam_watch_stdout.log 2>&1 &`
   (camera ≥2 ft from panel, squared-on, whole panel in frame; `latest.png`
   refresh + triggers just work).
3. Keep `glitch_watch.py` running at all times (it survived the night; serial
   + buffer dumps are the continuous baseline).
4. When an onset is timestamped twice, correlate: onset-vs-uptime and
   onset-vs-room-lighting/temperature. Two clean recurrence data points
   discriminate thermal from environmental.
5. If still unresolved: PCLK 13→10 MHz diagnostic build (one config change
   in both `esp_panel_board_custom_conf.h` copies), or bounce buffers ×20.
6. Hardware action if margin confirmed: reseat/replace the RGB cable;
   consider a shorter/shielded cable run; check the charger's 5 V rail
   quality (ripple feeds the RGB lanes directly).

## 5. Quick reference

- Data dir: `~/.glitchwatch/` (env `GLITCHWATCH_DIR` to relocate)
- Serial console: `/dev/cu.usbmodem5B901265491`, CH343, 921600 baud,
  UART1/UART2 slide switch must be at UART1; USB-C also powers the board
- Start watchers:
  - serial/buffers: `nohup python3 firmware/tools/glitch_watch.py … &`
  - camera: `nohup python3 firmware/tools/cam_watch.py … &`
- Manual dump request (while glitch_watch owns the port): append a line to
  `~/.glitchwatch/trig_queue` — glitch_watch sends SNAPSHOT+LVSNAP
  immediately (screen freezes ~40 s during the dump; by construction
  panel-side ghosts never appear in a dump).
- Raw dump decode: 12-byte header ('S','N','A','P' + int32 LE w,h) followed
  by RGB565 LE data; `hostsim/raw2png.py` decodes this format.

## 6. 2026-11-18 afternoon: firmware EXONERATED (A/B); power-event story later overturned in §7

The "us" suspicion was tested head-on with a firmware A/B (fast because
the artifact was onsetting in seconds, not hours):

| Build | Result |
|---|---|
| New 15-min chart fw (11:43, 11:57 flashes) | noise within minutes of boot |
| OLD pre-change fw (12:01 flash) | user saw clean at 14:16 (2h15m later; earlier states unobserved) |
| Bisect: old UI + new data layer (14:17 flash) | noise at 14:20-14:44 |
| **Same bisect fw, untouched** | **noise cleared spontaneously ~14:45-14:50** |

The spontaneous clear on unchanged firmware kills the code-guilt reading:
identical bytes, different behavior. Today's pattern is fully explained by
a **power-event trigger with decay**: every episode began within minutes of
a flash/reset and cleared within ~30-60 min. The earlier "old fw was clean"
observation doesn't discriminate (its noise, if any, would have decayed
before the 14:16 glance).

Consolidated artifact profile after today:
- Below-framebuffer, always (buffer dumps byte-clean during every episode,
  re-verified on three separate builds).
- **Trigger: power events** (flash/reset/power-cycle).
- **Duration: ~30-60 min, self-clearing** (first direct observation
  2026-11-18 ~14:45-14:50).
- Also note: past "after hours" onsets (3h21m on 11-17) do NOT fit this
  profile — either two distinct artifacts exist, or the thermal/timing
  margin is non-monotonic (bad cold, good warm, bad hot). The camera must
  be re-aimed at the panel (it was found pointed at the wrong desk) before
  the next session so onset/decay gets objective timestamps.
- PCLK ladder (closed): 10 MHz black, 12 MHz color-cycling, 13 MHz floor.

The 15-min chart work is back in the tree (restored from the dropped stash
commit 9e9b3241), tests pass; it was never the cause.

## 7. 2026-11-18 ~17:30: overnight isolation run (current plan)

Setup: board disconnected from the Mac (no CH343 UART cable), moved away
from the computer, USB-C power only, left to run overnight. This is the
doc §3 "move the board" test: if the artifact still appears in isolation,
desk/USB-chain EMI is exonerated and the driver is on-board or in the PSU;
if the night is clean, the desk environment becomes the prime suspect.
Serial-side watchers can't see it tonight (no UART) — the verdict is
whatever the user observes walking past, ideally with rough timestamps.
glitch_watch stays running on the Mac and auto-reconnects when the board
returns.

Notebook on the day's trigger theories (all falsified by 17:20):
- code guilt: dead (A/B + spontaneous clear on unchanged fw)
- power-event-trigger-with-decay: dead (spontaneous 16:39 onset)
- ~2¼-hour cycle: dead (17:20 onset, only 41 min after 16:39)
- surviving fact set: sporadic episodes (minutes to ~30 min), onset
  mechanisms unresolved; flash→noise-within-minutes happened 3/3 today
  (11:43, 14:17, 17:03→17:20) PLUS one spontaneous onset — flashes may
  re-arm the mechanism without being the only trigger.

Also today: today's morning chart data (hours 0-11) was lost by the 12:01
old-fw A/B flash (old code discarded the v3 snapshot) and backfilled at
17:05 from the relay's hourly record via a new standing serial command
(BKFL <hour>,<kwh> — fills only missing quarter buckets at hour
resolution; see solar_hist_fill_hour + the BKFL regression test).
Onsets log so far: 11:43, 14:17, 16:39(-16:51), 17:20-...

At 16:39 the noise returned SPONTANEOUSLY — no flash, no reset, no touch; the
board had been running untouched since 14:17. It cleared again within ~10
min. That kills the "power-event trigger with decay" reading from §6 just
as firmly as §6 killed the code-guilt reading. The artifact is episodic and
externally driven:

| Episode onsets (user-observed) | Spacing |
|---|---|
| ~11:43 | — |
| ~14:17 | +2h34m |
| ~16:39 | +2h22m |

A ~2¼-hour cyclical driver is implied. If the rhythm holds, the next
episode lands ~19:00. (The 11:43 and 14:17 onsets coinciding with flashes
now read as phase coincidence — the 16:39 onset had no power event at
all, so the cycle stands on its own.) Note: 2h20m is atypical for HVAC
(15–60 min); candidates include unusual appliance cycles, desk-equipment
activity, sun angle (11:43 midday / 16:39 near sunset — the 19:00 check
discriminates: post-darkness onset rules sun out), or the long-uptime
thermal mode seen on 11-17 (3h21m onset) — possibly the same slow cycle
observed at different phases.

Correlation tooling status: Envoy has NO consumption CTs (production only,
verified via /ivp/pdm/energy — all consumption fields zero), so household
load correlation must come from user knowledge. **The webcam is still
pointed at the wrong desk** — re-aiming it at the panel is the top pending
action; it turns every future episode into an objective onset/clear
timestamp pair and enables the cross-trigger buffer captures.

1. **Charger swap + single power cycle** — now doubly motivated: it tests
   the PSU AND doubles as the controlled power event to observe the
   trigger/decay profile on a different supply. If noise never appears on
   the new supply: PSU confirmed as the trigger's source.
2. If noise still appears after the swap: it's on-board (cable/connector/
   panel timing) — reseat the RGB flat cable next.
3. Re-aim cam_watch at the panel for objective onset/decay timestamps,
   and let a run go overnight to check whether the long-uptime ghost-row
   artifact (a possibly distinct phenomenon) still exists on top of the
   cold-start pulsing.

Context: the 15-min bucket chart shipped today (96 bars; `hourly` → 15-min
buckets through solar_hist v3 / relay kwh_15m / dashboard_ui). Panel flashed
at 11:43 after a session with 4 hard power-cycles (incl. 3
download-mode power-ons visible in serial.log).

What happened next:

- **Artifact mode changed**: user reported CONTINUOUS full-screen pulsing
  artifacts immediately after the flash (previous mode: static ghost dash
  rows, onset ~3 h after power-on). Same pattern as before but faster onset
  and pulsing instead of static residue.
- **Software exonerated again, by fresh evidence** (11:44–11:48 captures):
  B0 == B1 byte-identical (768 000 B diff = 0) while the panel pulsed;
  buffers byte-static from 11:46:43 to 11:48+; "50 FPS / 99% CPU" perf
  overlay and the chart-baseline band verified present in the PRE-flash
  buffer dump too (pixel-sampled 114137_B0 vs 114620_B0). Verdict per the
  matrix: below the framebuffer, RGB scan path — again.
- **PCLK downward-lever tested and closed**:
  - 10 MHz → panel black, firmware healthy (fetched Envoy at 20 s uptime;
    framebuffer held a perfect UI the panel never displayed).
  - 12 MHz → panel sync lost hard: full-screen cycling colors
    (green/blue/red/blank…).
  - 13 MHz (restored) → normal. The ST7262's usable clock floor sits
    sharply between 12 and 13 MHz; NO downward timing margin to exploit.
- **Reframing of the artifact**: the 12 MHz color-cycling shows what RGB
  sync loss looks like on this panel. "Pulsing" at 13 MHz is plausibly the
  *intermittent* version of the same sync loss (panel resyncs show as
  pulses) — i.e., sync margin on the RGB lanes is small, and something
  (rail noise / thermal / cable) pushes it over briefly. The static ghost
  rows previously captured may be one residue mode of the same margin
  problem.
- Onset immediately after a power-cycling flash session strengthens the
  power-path candidates (PSU ripple/sag still the only untested doc
  hypothesis; cable reseat second).

Next steps (unchanged order, sharpened): (1) charger swap + single clean
power cycle, compare onset with cam_watch+glitch_watch running (both were
redeployed 11:48 and survived the diagnostic flashing); (2) cable reseat;
(3) PCLK is CLOSED as a lever (floor ~13 MHz, upward already rejected at
16 MHz in the 2026-11-15 noise saga).

The board: Waveshare ESP32-S3-Touch-LCD-7 (RGB565 direct-drive 7" panel,
LVGL 8, direct Enphase fetches every 5 min). Session context: recurring
"horizontal glitch lines / ghost rows" on the panel, reported as appearing
"after some time", distinct from the already-closed fetch-window radio noise
(see NOTES.md — PCLK 16→13 MHz + WiFi.setSleep(false), user-verified 2026-11-15).

## 8. 2026-11-18 evening: root cause NARROWED — RGB LCD vs WiFi radio airtime

Two days of chasing a below-framebuffer "idle ghost" (§2–§7) gave way tonight
to a clean one-variable isolation that pins the *recurrent* noise to **WiFi
radio airtime starving the RGB scan path** — the same shared memory-bus /
core-0 contention as the NOTES.md fetch-window saga, now confirmed as the
dominant cause and matched to an external ESP32-S3 reference the user found.

Three experiments, in order:

1. **Render stress, radio OFF** — `demo-stress` build (`-DDBG1_NO_WIFI` +
   `-DDBG1_DEMO_FEED`): animated 96-bar bell at 2 full redraws/s (~600×
   real-data load), zero radio. **Clean for 12 min** (~1,400 redraws).
   → chart drawing / memory-bus pressure from rendering is exonerated.

2. **Direct mode + bounce buffer ×20** (16→32 KB, both conf copies +
   main.cpp runtime call; N=24 even): **still corrupted during the Envoy TLS
   fetch** (user-observed mid-fetch). → a bigger bounce buffer does NOT
   absorb the 20–55 s radio bursts; direct mode is not salvageable.

3. **Socket push** — `push-demo` build + `tools/push_demo_server.py` (Mac TCP
   server, newline-JSON every 1 s; board is a plain-TCP client, no TLS/HTTP/
   polling): **clean + updating every second**, user-verified. → WiFi ON is
   fine when airtime is a ~1 s connect plus millisecond receives; the
   20–55 s Envoy TLS fetch is the trigger.

**Conclusion:** the artifact is RGB LCD scan-out vs WiFi radio airtime
contending for the shared Octal-SPI memory bus + core 0. Sustained radio
(direct Envoy TLS: handshake 6–28 s + GET 12–54 s, every 5 min) starves the
always-on ~26 MB/s pixel stream → tearing/ghost rows. The panel has no
downward PCLK margin (§6: 13 MHz floor) and the ×20 bounce buffer is
insufficient — the fix is to stop the long TLS pulls.

**Memory headroom** (push build, `[push] mem:` telemetry every 10 s):
internal heap ~45 KB free (tight pool — LVGL 128 KB + WiFi/lwIP + JSON
buffers), PSRAM ~5.8 MB free of 8 MB (framebuffers ~2.25 MB, zero
fragmentation). The throwaway client is ~32 KB hungrier than direct mode
(4 KB String buffer + dynamic JsonDocument) — recoverable with a fixed
buffer + StaticJsonDocument.

**Decision — rearchitect to push:** a separate always-on host (the Mac)
fetches the Envoy (0.3 s) and pushes to the board over a socket (or MQTT);
the board receives short pushes, never performs long TLS pulls. Next:
design the real push path (socket vs MQTT, reconnect/backoff, framing, clock
sync) and keep the relay HTTP-poll path as a fallback.

**Reconciliation with the 11-17 idle ghost:** the 20:52 buffer-clean,
between-fetches capture was NOT reproduced tonight and remains a separate,
rarer open question (possibly the same scan-path margin pushed by a different
stressor — PSU/thermal still untested). Tonight's noise is the
fetch-correlated class, and it is the reproducible, actionable one. PSU swap
+ cable reseat stay cheap open checks for any residual idle-class artifact.

## 9. 2026-11-19: push architecture live; idle-class artifact persists — co-stressors nearly exhausted

Cold-start summary of today's session (board reset ~15:00, user continuing
to run the workload-minimal build).

**Push rearchitecture deployed & verified live** (spec.md, branch
`push-rearchitecture`): Rust server ran unbroken overnight (same PID,
midnight rollover correct: yesterday's row closed 23:59:49, fresh day
row live), board stayed linked, ~900 B/s per second. The fetch-class
noise (§8's reproducible trigger) is gone by construction; what remains
is the §2/§7 idle-class artifact.

**Episode log (new):**
- 11-19 ~05:50 user sees ghost rows after ~8 h overnight uptime. 4th
  synchronized capture: **B0 == B1 byte-identical, LV matches** (mod
  clock seconds + perf-monitor overlay) → below-framebuffer, again.
- 11-19 ~14:58 (~8 h 10 m after power-on) ghost rows on the
  workload-minimal build → the strip's contents are ruled out as causes.

**Ruled out this session:** PSU (user tried multiple cables/power
sources), thermal (office stable 68-72 °F), EMI (user has seen it
>6 ft from computers), perf-monitor overlay / SNAPSHOT machinery /
serial handler / host watchers (stripped from the running build and it
still ghosts). Camera retired: blind at night (frame max 13/255).

**Confounder found + fixed:** LV_USE_PERF_MONITOR overlay (bottom-right
FPS/CPU text, redrawn every second since day one) masqueraded as "ghost
text" in row-diffs; masked in glitch_watch (49a90da), disabled in
firmware (d0a0de7).

**Current build on the board (d0a0de7, workload-minimal):** socket
parse (1 Hz) + render + clock label + now-line + heartbeat printf.
That's everything. DBG1_NO_WIFI A/B was considered and withdrawn —
asymmetric value (a ghost is decisive, a clean run is weak given
8 h+ clean stretches on buggy builds).

**Next steps (agreed order):**
1. Reseat RGB FPC cable (never done — power down first) → watch the
   evening/overnight window; note power-on time for onset correlation.
2. Still ghosts after reseat → Waveshare RMA (evidence packet is
   nearly complete); optionally first run the DBG1_NO_WIFI 24 h test
   to make the packet unanswerable.
3. Clean after reseat → keep watching several days.

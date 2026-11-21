# Performance plan: reducing the LVGL task from ~99% CPU

**Status (2026-11-21):** Step 0 ✅ done · **Step 1 ✅ done — target met with
margin (IDLE1 58% ≥ 50%) — judgment call: stop here for now; Steps 2–3
deferred (decision + triggers in Step 1 results, below).**

**Written:** 2026-11-20 (planning for the next work session — pick up here)
**Revised:** post-review — Step 1 re-diagnosed (the second render is the
perf monitor, not the push-data cycle), Step 2 buffer sizing and the Step 4
cache item corrected. The NOTES.md §2026-11-20 "push-data cycle" attribution
is superseded by Step 1 below.
**Reference:** [ESP-IDF Speed Optimization guide (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/speed.html)
**Prior evidence:** `docs/NOTES.md` §2026-11-20 (perf-monitor diagnosis), `docs/GHOSTING_INVESTIGATION.md` (§6 PCLK ladder, §8 root cause)

---

## 1. The problem, precisely

LVGL's on-screen perf monitor reports **~99% CPU**, and the serial probes
(`[DEBUG-c99]`, branch `diag/cpu-probes`) confirmed the number — ~99% of
the time *inside `lv_timer_handler()`* is wall-clock busy. (Both were
honest, but the monitor was also *causing* half of it — caveat below.)

The mechanism (NOTES.md 2026-11-20):

- We run the RGB panel in **full-refresh mode**: LVGL renders the entire
  800×480×2 = 750 KB screen into one of the two PSRAM framebuffers
  (`esp_lv_adapter_arduino.cpp`, `rgb_full_refresh = true`), then flips.
- The panel DMA **continuously scans out ~26 MB/s from the same Octal-PSRAM
  bus** (GHOSTING_INVESTIGATION §8).
- Every render therefore pays a bus-contention stall: measured
  **~450–500 ms per full-screen render**.
- Two renders per second fire (the 1 Hz coalesce tick, plus a second one
  whose corrected attribution is Step 1's subject: it is the perf
  monitor's own label update, not the push-data cycle) → the LVGL task is
  saturated.

Key insight: this is **memory-bus time, not compute**. The CPU is stalled
waiting on PSRAM writes that share a bus with an always-on DMA. No compiler
flag, cache tweak, or code cleanup removes a bus stall.

Two caveats on the headline ~99%, both about how it was measured:

- **The perf monitor was half the load.** `LV_USE_PERF_MONITOR` is on
  unconditionally (`lib/lv_conf.h:287`), and LVGL 8.4 updates its label at
  the end of any refresh finishing ≥300 ms after the last update
  (`lv_refr.c` — `lv_label_set_text_fmt` on the sys-layer label). A label
  update is an invalidation, and in full-refresh mode every invalidation
  is a whole 750 KB render. With renders at ~450–500 ms, the loop
  self-sustains at exactly the observed 2 renders/s — on alternating
  worker passes. Push data is *not* the cause: all three setters are
  buffer-only, applied by the coalesce timer (18e8e5f).
- **"Busy" includes blocked-vsync time.** The c99 probe bracketed
  `lv_timer_handler()` with `esp_timer` (wall time), and full-refresh
  `flush_cb` ends with `ulTaskNotifyTake(portMAX_DELAY)` waiting on the
  refresh-finish ISR (tens of ms per frame at the ~13 MHz PCLK floor).
  Blocked time is CPU-yielded, so even the post-Step-1 residual ~50%
  overstates true CPU occupancy.

### What the ESP-IDF doc says we should already have covered (we do)

| Doc lever | Status |
|---|---|
| Measure before optimizing | ✅ perf overlay + esp_timer probes + buffered-dump tooling |
| Invalidate only on change / coalesce writes | ✅ `18e8e5f` — one invalidated frame per second |
| Remove background CPU contention | ✅ Push rearchitecture (PRs #10/#12) — radio airtime now ~1 s at boot + ms-scale receives |
| Known-good task placement | ✅ LVGL task prio 2 / core 1 (core 0 was tried and regressed) |
| QIO flash, OPI PSRAM | ✅ `platformio.ini` (`qio`, `qio_opi`) |
| Reduce logging volume | ✅ console only used for debug probes |

So the doc's *global* optimizations are largely tapped out. Its
**Targeted Optimizations** section is where we go next: attack a specific
hot path (the render transport) rather than the whole firmware.

### One thing the doc warns about that we hadn't done yet — now done (Step 0, 2026-11-21)

> use `vTaskGetRunTimeStats()` to retrieve processor time per task

All our measurements bracketed the LVGL task in isolation. We had **no
system-wide picture**: is the 99% actually starving IDLE/other tasks, or is
it effectively "free" time the CPU spends stalled on the bus anyway? This
decided how urgent the fix is. Answer (Step 0 findings, below): nothing
starves — the load is confined to core 1's lvgl task.

---

## 2. Plan, in priority order

### Step 0 — Measure system-wide (do this first) — DONE 2026-11-21, findings below

One diagnostic build with `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` and a
serial command that dumps `vTaskGetRunTimeStats()`. (Tooling precedent:
`diag/cpu-probes`; add to that branch or a new `diag/rtos-stats` branch.)

Questions it answers:
- How much IDLE time remains on each core? (true saturation vs misleading
  headline number)
- What does the push_client / WiFi / timer stack actually cost? (we believe
  ~nothing post-rearchitecture — verify)
- Baseline to quantify the win when the partial-refresh work lands.

Interpretation caveat: run-time stats count only *running* time, so the
blocked-vsync waits inside `lv_timer_handler` land in IDLE's share, not the
LVGL task's. Expect the LVGL task's share to come in **below** the c99
wall-clock number — the two probes measure different things, and the
run-time-stats number is the honest one for "is anything starving".

Also useful after Step 1: `cpu_hal_get_cycle_count()` microbenchmarks of the
render path (doc: lower overhead than `esp_timer` for sub-2 ms intervals —
though our render is ~500 ms, so `esp_timer` around it is fine).

**Expected outcome:** a baseline table (task / % / core) committed to
NOTES.md so before/after comparison is honest.

**Findings (measured 2026-11-21 — done).** Branch `diag/rtos-stats`
(serial `stats` command → 30 s `uxTaskGetSystemState` snapshot diff;
companion `tools/rtos_stats_capture.py`; raw capture committed as
`docs/rtos_stats_baseline_2026-11-21.txt`). No sdkconfig change needed:
the prebuilt Arduino core (IDF 5.5.5) already ships
`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`. Full table + interpretation
in NOTES.md §2026-11-21; the short version:

- **Nothing is starving.** Core 0: 96.6% IDLE (radio+lwIP+timers ≈ 3.4%).
  Core 1: ~96% busy — all of it the lvgl task (93.7% *running*).
- **The 99% is real running time.** lvgl ran 93.7% of wall vs c99 ≈ 99%
  wall busy → blocked-vsync slice ≈ 5%, matching IDLE1's 4% share. The
  load is PSRAM-stall cycles, as diagnosed.
- **Push stack verified ≈ free:** loopTask 2.4%, wifi 1.2%, lwIP 0.3%.
- **Minor finds:** the 2 ms lvgl tick esp_timer costs ~1.9% of a core
  (cheap future lever, low priority); loopTask cadence is stretched ~4×
  under lvgl's prio-2 on the same core (harmless); stack HWMs healthy
  (lvgl 7.7 KB free of 12 KB).

Verdict for the plan: Steps 2–3 are headroom work, not rescue — exactly
§4's framing. Step 1 remains first, now with a measurable target: lvgl
~94% → ~47% running, IDLE1 4% → ~53%.

### Step 1 — Cheap win: turn the perf monitor off (it IS the second render) — DONE 2026-11-21

**Corrected diagnosis.** An earlier version of this step said "coalesce
the push-data update into the tick" — but that already happens
(18e8e5f: all three setters are buffer-only, applied by the 1 Hz
coalesce timer), and NOTES.md's own conclusion was "coalescing was
already at the app's floor". Push data has no invalidation path; the
proposed PR would have been a no-op.

The second render is the **perf monitor itself**. `LV_USE_PERF_MONITOR`
is on unconditionally in `lv_conf.h` (both copies — the `lib/` gotcha
applies), and LVGL 8.4's monitor updates its label whenever a refresh
finishes ≥300 ms after the last update (`lv_refr.c` — `lv_label_set_text_fmt`
on the sys-layer label). That label update is an invalidation, and in
full-refresh mode every invalidation is a whole-screen 750 KB render.
With renders at ~450–500 ms the loop self-sustains at exactly the observed
2 renders/s, on alternating worker passes. The on-screen monitor wasn't
just honestly reporting the load — it was half of it, in *production*
builds, not only diag ones.

The fix: `LV_USE_PERF_MONITOR 0` — or a build flag so diag builds can
still show it. One line, in each `lv_conf.h` copy.

- Expected effect: busy time roughly halves (~99% → ~50%).
- Scope: config-only; no transport change; no risk to tear behavior.
- Verify, don't assume (the diagnosis is inferred from static reading,
  and it supersedes NOTES.md's "push-data cycle" attribution): one build
  with the monitor off, re-run the c99 probe, and optionally log the dirty
  `area` in `flush_cb` to attribute invalidations beyond doubt.
- This matches the doc's first principle: *choose what to optimize* — at
  rest our screen changes at most once per second, so more than one render
  per second is waste by definition. The perf monitor broke that invariant
  every second, forever.

**Expected outcome:** merge the one-line PR; re-run the c99 probe to confirm
the busy-% drop. Decision point: if ~50% busy (wall-clock, of which part
is blocked-vsync — see Step 0) is acceptable for the foreseeable future,
Steps 2+ become optional quality work instead of urgent fixes.

**Results (measured 2026-11-21 — done).** `LV_USE_PERF_MONITOR 0` in both
lv_conf.h copies, branch `feat/perf-monitor-off`; measured via branch
`diag/step1-measure` (feat + diag harness); raw capture
`docs/rtos_stats_step1_2026-11-21.txt`; full before/after in NOTES.md
§2026-11-21. Headline: flush 2/s → **~1/s** (the coalesce floor — the
monitor diagnosis confirmed), lvgl running 93.7% → **39.3%** (two runs
<1% apart), IDLE1 4% → **58.2%**, c99 wall busy ~99% → **~41%**,
core-0 unchanged (~97% IDLE). Success criterion "≥50% IDLE on the LVGL
core" — met with margin. One honest gap: no automated visual check ran
(glitch_watch needs the stripped SNAPSHOT/LVSNAP commands, absent from
this build) — risk is near nil since Step 1 only *reduces* write
traffic into the scan path, but eyeball the panel when convenient.

**Decision (judgment call, recorded 2026-11-21): do NOT proceed to
Step 2 now.** Nothing starves; renders sit at the app's true 1 Hz
floor; §4's defensible-endpoint argument is now measurement-backed.
Step 2 remains the known fix with a written plan, triggered by either:
- touch latency actually bothering a user (~0.4 s full-screen render
  per interaction — the one cost Step 1 cannot touch), or
- a new PSRAM-bus user appearing (audio, camera, heavier UI).
Until one of those fires, the cost/risk of Step 2 (tearing on a panel
with a scan-path marginality history, SRAM budget that doesn't fit the
stubbed two-buffer path) buys headroom nobody currently needs.

### Step 2 — The real fix: partial refresh with internal-RAM draw buffers

This is the decision NOTES.md left pending, and it is exactly the doc's
"Targeted Optimizations" philosophy: shrink the hot path to only what
changed.

Mechanism:
- LVGL renders **only the dirty region** into small draw buffers in
  *internal SRAM* (800×40 rows × 2 B = 62.5 KiB per buffer), then
  `drawBitmap` blits just those rows into the scan framebuffer.
- Typical dashboard updates (a chart bar, the network pill, two labels)
  touch a small fraction of the screen → PSRAM write traffic drops by
  ~90%+ per render → the bus-contention stall collapses.

The adapter already has the path stubbed: `esp_lv_adapter_arduino.cpp`
allocates internal draw buffers when the RGB framebuffers aren't used
(`rgb_full_refresh = false` branch, `flush_cb` → `lcd->drawBitmap`,
`draw_bitmap_finish_cb` → `lv_disp_flush_ready`).

Costs / risks to weigh before building:
1. **Tearing** — we currently rely on whole-framebuffer swap for tear-free
   updates. Partial writes into the live scan framebuffer can shear pixels
   mid-scanline. Mitigations: (a) blit during vertical blank via the
   refresh-finish callback timing; (b) accept minor transient tearing on a
   dashboard (cosmetic); (c) hybrid — small internal render buffer(s),
   blit at vsync (see Step 3).
2. **Scan-path marginality** — GHOSTING_INVESTIGATION established the
   artifact originates below the framebuffer (scan path), and §6 closed the
   PCLK ladder at a ~13 MHz floor. Changing *when* and *how much* we write
   changes the noise profile. Keep `glitch_watch.py` running for the first
   bring-up and treat any episode recurrence as a one-variable experiment
   (same discipline as §8).
3. **Internal SRAM budget** — the tight one; the stubbed path as written
   does not fit. It allocates **two** buffers
   (`esp_lv_adapter_arduino.cpp`, `register_display`) = ~125 KiB internal,
   against evidence of ~79 KB largest free block (ghosting telemetry) and
   push_client's comment of ~45 KB free internal heap. The second
   allocation fails → `register_display` returns `nullptr` → boot-time
   display failure, not a graceful fallback. Plan for a **single** draw
   buffer (LVGL works with one; only double-buffering is lost) and/or a
   smaller height; buffer height should be a build flag, not magic.
   Verify free-heap numbers on the target before committing to a size.

**Expected outcome:** a branch (e.g. `feat/partial-refresh`) doing one-mode
switching only — either full or partial, selected at init — with the c99
metric, an interaction-latency eyeball check, and glitch_watch running.
Keep the full-refresh path intact behind the flag.

### Step 3 — If needed: tear-free hybrid / PPA offload

Only if Step 2's tearing is unacceptable or the win is smaller than hoped:

- **PPA (ESP32-S3 Pixel Processing Accelerator)**: hardware DMA blit —
  moves the framebuffer copy off the CPU entirely. The CPU-side stall during
  `drawBitmap` (CPU-cached writes through Octal PSRAM) becomes a DMA
  transfer. Espressif's LVGL port (`esp_lvgl_port`) has PPA integrations
  worth copying from.
- **Tear-free hybrid**: render dirty region to an internal buffer, then
  PPA-blit into the scan framebuffer during vblank (the driver's
  `esp_lcd_rgb_panel_event_callbacks_t.on_vsync` gives the timing point).

This is beyond the ESP-IDF doc but is the same idea it espouses — targeted,
hardware-aware optimization of the hot path.

### Step 4 — Doc-checklist follow-ups (small, low priority)

Verify rather than assume:

- [ ] Confirm the firmware env actually compiles with `-O2` (Arduino
      default is `-Os`); `platformio.ini` has no explicit flag. Check binary
      size delta. *Caveat: won't move the bus-stall needle; it's hygiene.*
- [ ] Cache split: **measured from the shipped Arduino core sdkconfig
      (IDF 5.5.5): I-cache 16 KB, D-cache 32 KB** — so at least the I-cache
      can grow; the S3 Kconfig also lists a 64 KB D-cache option. Verify
      what the chip's cache RAM actually permits (TRM) before assuming
      headroom, and note that a bigger D-cache can only help the PSRAM
      read side of the stall. The only *global* item in the doc that
      directly affects PSRAM access cost. Measure with the Step-0 harness
      (findings recorded above).
- [ ] ISR callbacks are already `IRAM_ATTR` (both in the adapter) ✅ — keep
      it that way for any new hot callbacks.
- [ ] `CONFIG_ESP_WIFI_IRAM_OPT` & friends: skip — radio work is ~zero
      post-push-rearchitecture; these trade IRAM for nothing here.

---

## 3. Success criteria

- `lv_timer_handler()` busy time within a **healthy bound** (target: idle
  dashboard leaves ≥50% IDLE on the LVGL core after Step 1 — **MET:
  58.2% (2026-11-21)**; ≥80% after Step 2 — deferred, see Step 1
  decision).
- Interaction (tab switch, touch) latency unchanged or better (NOTES.md
  baseline: judge by interaction responsiveness, not FPS on static content).
- No recurrence of the scan-path ghost artifact with `glitch_watch.py`
  running during bring-up.
- Radio budget untouched (still ~1 s boot connect + ms receives).

## 4. If we do nothing more

After Step 1, the residual ~50% busy time is bus-stall time (plus a
blocked-vsync slice FreeRTOS actually counts as IDLE — see Step 0) during
full-screen renders — bounded, once per second, on a core whose other
occupants are idle. The doc's own framing applies: optimization is only
worth it for functions executed *often enough to matter*. A dashboard that
sleeps between one render per second and yields the rest is arguably
already at a defensible endpoint. Steps 2–3 are about reclaiming headroom
for a future bus user, not about rescuing anything broken today.

---

## Appendix: files that matter

- `firmware/src/esp_lv_adapter_arduino.cpp` — transport, full/partial
  branching, buffer allocation, worker loop (`task_max_delay_ms` cap = why
  we run at ~2 Hz, not 50 Hz)
- `firmware/src/main.cpp` — adapter config (prio 2 / core 1), tear mode
- `firmware/src/dashboard_ui.cpp` — invalidation / coalesce logic (already
  at the app's floor, 18e8e5f — all setters buffer-only)
- `firmware/include/lv_conf.h` + `firmware/lib/lv_conf.h` — keep the two
  copies in sync (see the 2026-11-14 gotcha: the effective one is `lib/`);
  `LV_USE_PERF_MONITOR` lives here (Step 1)
- `firmware/lib/lvgl/src/core/lv_refr.c` — the perf-monitor label update
  (the Step 1 mechanism, ≥300 ms re-invalidation loop)
- `docs/GHOSTING_INVESTIGATION.md` — §6 PCLK floor, §8 root cause, §2 triage
  matrix; `firmware/tools/glitch_watch.py` — bring-up watch
- `diag/rtos-stats` branch — Step 0 tool: serial `stats` → 30 s run-time-stats
  diff (c99 probes included; `tools/rtos_stats_capture.py` captures);
  `diag/step1-measure` — the Step 1 verification build (feat + harness);
  `diag/cpu-probes` branch — the original c99 probe

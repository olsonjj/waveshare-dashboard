# Spec: Push-based solar dashboard (Rust host → board pure renderer)

## Problem Statement

The solar dashboard (Waveshare ESP32-S3, 7" RGB panel) shows screen corruption
— tearing / ghost rows — whenever the firmware fetches solar data over WiFi.
Root cause (investigated and documented in `docs/GHOSTING_INVESTIGATION.md` §8):
the RGB LCD's always-on ~26 MB/s pixel stream and the WiFi radio's Envoy TLS
fetch (20–55 s of radio every 5 min) contend for the shared Octal-SPI memory
bus + core 0, starving the display. Direct mode (the board performing the
Envoy fetch) is unsalvageable: the panel has no downward PCLK margin (13 MHz
floor), and a doubled bounce buffer did not absorb the bursts.

The user wants a dashboard that renders cleanly and stays current, without the
board performing long radio pulls.

## Solution

Rearchitect from "the board pulls the Envoy over TLS" to "a host service pushes
ready-to-render screen state to the board." A Rust service runs on the always-on
Mac, polls the Envoy every 60 seconds, accumulates today's 15-minute production
buckets, persists them in SQLite, and pushes the complete screen state to the
board over a raw TCP socket every second. The board becomes a pure renderer:
it connects, reads newline-delimited JSON, and renders — no Envoy access, no
bucketing, no NVS history, no long radio activity. This keeps the board's WiFi
airtime to a ~1 s connect plus millisecond receives, the quiet state already
proven clean (see the throwaway push prototype).

## User Stories

1. As a homeowner, I want the dashboard to render without tearing or ghost artifacts, so that I can read the solar numbers reliably.
2. As a homeowner, I want the current-power card to update every second, so that it feels live.
3. As a homeowner, I want the total-yield card to reflect today's production since midnight, so that I can track the day's running total.
4. As a homeowner, I want the peak-power card to show the highest power observed today, so that I can see the day's best output.
5. As a homeowner, I want the 7-day energy figure to stay correct, so that I can compare today against a rolling week.
6. As a homeowner, I want the generation curve to show today's production in 15-minute buckets from midnight to the current time, so that I can see the day's shape.
7. As a homeowner, I want the header clock to show correct local Pacific time, so that I can trust the chart's now-line.
8. As a homeowner, I want the chart's now-line to sit at the current time of day, so that I can see how production tracks against the present moment.
9. As a homeowner, I want the dashboard to begin a fresh, empty day at local Pacific midnight, so that each morning starts clean.
10. As a homeowner, I want not-yet-occurred (future) buckets to render as empty rather than as zero-production bars, so that the chart does not mislead about the evening's production.
11. As an operator, I want the host service to poll the Envoy every 60 seconds, so that data stays fresh without hammering the gateway.
12. As an operator, I want the service to cache the latest Envoy reading between polls, so that the 1-second pushes always have a value to send.
13. As an operator, I want the service to push a complete screen-state JSON line every second, so that the board never has to merge partial state.
14. As an operator, I want the service to run via a single `just run` command, so that starting and stopping it is simple and documented.
15. As an operator, I want the Envoy credentials kept out of git in a server-local config, so that secrets are never committed.
16. As an operator, I want today's buckets persisted in SQLite, so that a mid-day service restart reloads the day and continues without losing history.
17. As an operator, I want a mid-day restart to resume from the stored buckets and the Envoy's current cumulative yield, so that no production is double-counted or lost across the restart.
18. As a developer, I want the bucket-accumulation logic to be a pure, dependency-free module, so that it can be unit-tested without hardware or the Envoy.
19. As a developer, I want the bucket logic to behave like the retired board-side `solar_hist` (straddle carry, gap gate, rollover), so that the port is provably equivalent.
20. As a developer, I want the push payload to be a documented, versioned JSON contract, so that the board and the server can evolve independently.
21. As a developer, I want the board firmware to be a thin socket client plus the existing renderer, so that the radio-heavy data layer is deleted rather than patched.
22. As a developer, I want the board to have no Envoy, TLS, bucketing, or NVS code paths, so that the noise-causing work cannot regress.

## Implementation Decisions

- **Host language / tooling**: a Rust service in a new `server/` directory,
  with `just` as the task runner (`just run`, `just build`) and SQLite via
  `rusqlite`.
- **Envoy polling**: every 60 s; the service caches the latest reading and
  re-sends it on every 1 s push. (The Envoy only refreshes its watts ~5 min,
  so most polls re-read unchanged values — accepted.)
- **Peak power**: track `max(watts_now)` observed today. The inverters
  endpoint is out of scope this version.
- **Retention / rollover**: today only (midnight → now). At local Pacific
  midnight, buckets reset to empty and `wh_today`/`watts_peak_today` reset to 0.
  SQLite stores today's state so a mid-day restart reloads and continues.
- **7-day energy**: read `wh_7day` directly from the Envoy (no local 7-day
  accumulation).
- **Push transport**: raw TCP socket, newline-delimited JSON, one line per
  second. The payload is the complete screen state; the board merges nothing.
- **Credentials**: Envoy host + bearer token from a server-local gitignored
  config, not the firmware's `secrets.h`.
- **Board**: pure renderer. The socket client replaces the data layer;
  `solar_hist`, the NVS snapshot, the direct TLS fetch, and the relay HTTP
  poll are all retired. The board's clock and now-line are driven by the
  pushed `epoch`; the board no longer seeds/syncs its own clock.

The push contract (single seam between server and board):

```rust
struct ScreenState {
    epoch: i64,               // unix seconds (Mac NTP clock) → header clock + now-line
    watts_now: u32,
    wh_today: u32,
    watts_peak_today: u32,
    wh_7day: u32,
    kwh_15m: [f32; 96],       // index = hour*4 + min/15; -1.0 = future/unknown → no bar
}
```

Bucket accumulation (port of the retired `solar_hist` state machine): map a
cumulative `wh_today` sample at wall time `now` to bucket `b = hour*4 + min/15`.
On the first sample of a bucket, record `wh_start[b]`; on later samples set
`kwh[b] = (wh - wh_start[b]) / 1000`. When a sample lands in a new bucket, carry
the straddling energy across the boundary; when a sample is stale (beyond the
freshness gate), restart the hour from the current reading and leave missed
buckets `-1`. Roll the whole day over at Pacific midnight.

SQLite shape (one table, today's day):

```sql
CREATE TABLE day (
  date       TEXT PRIMARY KEY,   -- 'YYYY-MM-DD' local Pacific
  kwh_15m    BLOB,               -- 96 × f32 LE
  wh_start   BLOB,               -- 96 × f32 LE, -1 = unset
  last_wh    INTEGER,
  last_ts    INTEGER,
  peak_w     INTEGER
);
```

## Testing Decisions

**What makes a good test**: test external behavior, not internals — feed a
sequence of (timestamp, cumulative-wh) samples and assert the resulting bucket
array, peak, yield, and rollover. Do not test the socket wiring or Envoy HTTP
at the unit level; those are covered by a thin integration check.

**The seam**: the single seam is the `ScreenState` contract — the boundary at
which the server produces a ready-to-render screen and the board consumes it.
Everything on the server side collapses into "produce `ScreenState`"; the board
side is the already-proven renderer. Testing therefore concentrates on the
server-side producers.

**Modules tested**:
1. The pure bucket accumulator — the data-correctness heart. Cases mirror the
   existing `firmware/test/test_solar_hist/` harness: straddle carry across a
   bucket boundary, the gap gate (stale sample), first-sample-of-bucket, peak
   tracking, midnight rollover, future-bucket = -1.
2. The payload serializer — round-trip / exact-JSON test for the `ScreenState`
   contract.
3. The Envoy response parser — fixture JSON → `ScreenState` fields.
4. (Integration) The socket pusher — a fake TCP client connects and asserts it
   receives well-formed JSON lines at ~1 s cadence.

**Prior art**: the repo already has a native C++ unit harness for the bucket
logic (`firmware/test/test_solar_hist/`, Unity, `pio test -e native`). The Rust
tests are its direct equivalent, replacing the C++ implementation with the port
and matching its case list. The throwaway push prototype (`push_demo_server.py`
+ `push-demo` firmware) is the prior art for the socket contract and cadence.

## Out of Scope

- 48-hour retention (this version stores today only).
- Network / stale handling on the board — if the push stops, the board stops
  rendering; no reconnect/backoff, offline state, or staleness indicator yet.
- MQTT / pub-sub (raw socket chosen).
- The inverters endpoint for a more precise peak.
- Board reconnect/backoff and clock-drift handling.
- Authentication / encryption on the push socket (trusted LAN).
- Multiple consumers of the push stream.
- The git cleanup itself (discarding the uncommitted experiments) — a separate
  task, not part of this feature.

## Further Notes

- Root-cause context: `docs/GHOSTING_INVESTIGATION.md` §8 and `docs/NOTES.md`.
  The board's WiFi must stay off the long-TLS path; push keeps its airtime to
  ~1 s connect + millisecond receives.
- The throwaway socket-push prototype (`tools/push_demo_server.py`,
  `src/push_demo.{h,cpp}`, the `push-demo` build env) proved the transport
  clean and is to be discarded per the cleanup plan.
- The board currently runs the throwaway `push-demo` build; the demo server has
  been killed (port 8421 free).
- Envoy poll cadence (60 s) intentionally re-reads cached values between the
  Envoy's own ~5-min updates; the 1 s push cadence is driven by the clock tick.

//! buckets.rs — pure 15-minute bucket accumulator.
//!
//! Port of the retired board-side `solar_hist` state machine to 96 buckets
//! (index = hour*4 + min/15): first-sample-of-bucket seeds the wh baseline,
//! later samples update the live partial, a straddling sample carries the
//! baseline across a bucket boundary when it is fresh (gap gate), a stale
//! gap restarts at the current reading and leaves missed buckets empty,
//! and the whole day rolls over at local midnight.
//!
//! The module is pure and dependency-free: the caller computes the
//! LOCAL-time parts (`date`, `bucket`) of a sample's epoch — see `clock.rs`
//! — so every path here is deterministic under test without hardware.

pub const N_BUCKETS: usize = 96;

/// Straddle-carry freshness gate: the bucket-boundary sample must land
/// within 3 poll intervals (60 s each) of the previous sample, else the
/// gap swallows the missed buckets (they stay -1) and the new bucket
/// starts at the current reading. Direct analog of solar_hist's
/// `HIST_CARRY_FRESH_S = 900` (3 × 5-min polls).
pub const CARRY_FRESH_S: i64 = 180;

/// One Envoy sample: cumulative watt-hours today, current watts, and the
/// unix-seconds wall time of the reading.
pub struct Sample {
    pub wh_today: u32,
    pub watts_now: u32,
    pub now: i64,
}

/// Everything the push payload (and SQLite) needs for one local day.
pub struct DayState {
    /// Local date "YYYY-MM-DD" this state belongs to.
    pub date: String,
    /// kWh per 15-min bucket; -1.0 = no data (renders no bar).
    pub kwh: [f32; N_BUCKETS],
    /// wh_today reading that opened each bucket; -1.0 = unset.
    pub wh_start: [f32; N_BUCKETS],
    /// Last accepted cumulative wh reading; -1 = none yet.
    pub last_wh: i64,
    /// Epoch of the last accepted sample; 0 = unknown.
    pub last_ts: i64,
    /// Index of the bucket the last sample landed in; -1 = none yet.
    pub last_bucket: i32,
    /// Highest watts observed today.
    pub peak_w: u32,
    /// Latest cached Envoy card values (zeroed at rollover until the
    /// next poll re-reads the Envoy).
    pub watts_now: u32,
    pub wh_today: u32,
    pub wh_7day: u32,
}

impl DayState {
    pub fn new(date: &str) -> Self {
        DayState {
            date: date.to_string(),
            kwh: [-1.0; N_BUCKETS],
            wh_start: [-1.0; N_BUCKETS],
            last_wh: -1,
            last_ts: 0,
            last_bucket: -1,
            peak_w: 0,
            watts_now: 0,
            wh_today: 0,
            wh_7day: 0,
        }
    }
}

/// Fresh, empty day at `date` (buckets -1, yield/peak/cached cards zeroed).
/// Called by the 1-second push tick on a local-date change so a new day
/// begins clean even before the first Envoy poll of the morning.
pub fn rollover(st: &mut DayState, date: &str) {
    *st = DayState::new(date);
}

/// Fold one Envoy sample into the day state (port of solar_hist's
/// `hist_apply` at 96-bucket granularity). `date`/`bucket` are the
/// local-time parts of `sample.now`.
pub fn apply_sample(st: &mut DayState, s: &Sample, date: &str, bucket: usize) {
    if date != st.date {
        // local-date rollover (same trigger as solar_hist): the day begins
        // fresh before the sample is folded in
        rollover(st, date);
    }
    if bucket as i32 != st.last_bucket {
        // close the previous bucket from its baseline
        let prev = st.last_bucket;
        if prev >= 0 && st.wh_start[prev as usize] >= 0.0 && st.last_wh >= 0 {
            let p = prev as usize;
            st.kwh[p] = (st.last_wh as f32 - st.wh_start[p]) / 1000.0;
        }
        // seed the new bucket from the last sample of the old bucket —
        // but only when the boundary is the adjacent bucket AND the
        // previous sample is fresh. After a poll outage wider than the
        // gate, carrying the stale wh would dump all of the missed energy
        // into this one recovery bar; instead start at the current wh and
        // the skipped buckets stay -1 (no bar, not a wrong bar).
        let adjacent = (st.last_bucket + 1) as usize % N_BUCKETS == bucket;
        let fresh = st.last_ts > 0 && st.last_ts <= s.now && (s.now - st.last_ts) <= CARRY_FRESH_S;
        st.wh_start[bucket] = if st.last_wh >= 0 && adjacent && fresh {
            st.last_wh as f32
        } else {
            s.wh_today as f32
        };
        st.last_bucket = bucket as i32;
    }
    if st.wh_start[bucket] >= 0.0 {
        // live partial
        st.kwh[bucket] = (s.wh_today as f32 - st.wh_start[bucket]) / 1000.0;
    }
    st.last_wh = s.wh_today as i64;
    st.last_ts = s.now;
    st.watts_now = s.watts_now;
    st.wh_today = s.wh_today;
    if s.watts_now > st.peak_w {
        st.peak_w = s.watts_now;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn apply(st: &mut DayState, wh: u32, now: i64, date: &str, bucket: usize) {
        apply_sample(
            st,
            &Sample {
                wh_today: wh,
                watts_now: 0,
                now,
            },
            date,
            bucket,
        );
    }

    fn b(hour: usize, quarter: usize) -> usize {
        hour * 4 + quarter
    }

    const D1: &str = "2026-11-19";

    #[test]
    fn fresh_start_all_buckets_empty() {
        let st = DayState::new(D1);
        for i in 0..N_BUCKETS {
            assert_eq!(st.kwh[i], -1.0, "bucket {i}");
            assert_eq!(st.wh_start[i], -1.0, "bucket {i}");
        }
        assert_eq!(st.last_wh, -1);
        assert_eq!(st.last_bucket, -1);
        assert_eq!(st.peak_w, 0);
    }

    #[test]
    fn first_sample_seeds_bucket_and_live_partial() {
        let mut st = DayState::new(D1);
        let bk = b(9, 2); // 09:30-09:45
        apply(&mut st, 5000, 1000, D1, bk);
        assert_eq!(st.wh_start[bk], 5000.0);
        assert_eq!(st.kwh[bk], 0.0); // live partial at the opening reading
        assert_eq!(st.last_bucket, bk as i32);

        apply(&mut st, 6500, 1060, D1, bk);
        assert_eq!(st.kwh[bk], 1.5); // (6500 - 5000) / 1000
                                     // no other bucket was touched
        assert_eq!(st.kwh[bk + 1], -1.0);
    }

    #[test]
    fn straddle_carry_fresh_boundary() {
        let mut st = DayState::new(D1);
        let bk = b(10, 3); // 10:45-11:00
        apply(&mut st, 5000, 1000, D1, bk);
        let next = bk + 1; // 11:00-11:15, 60 s later: adjacent and fresh
        apply(&mut st, 6500, 1060, D1, next);
        // the closed bucket ends at its LAST sample: (5000 - 5000) = 0
        assert_eq!(st.kwh[bk], 0.0);
        // the new bucket is seeded from the previous sample's wh (5000),
        // so the straddle energy (5000 -> 6500 across the boundary) lands
        // in the NEW bucket as its opening live partial
        assert_eq!(st.wh_start[next], 5000.0);
        assert_eq!(st.kwh[next], 1.5);
    }

    #[test]
    fn gap_gate_no_giant_bar() {
        let mut st = DayState::new(D1);
        let bk = b(9, 0);
        apply(&mut st, 5000, 1000, D1, bk);
        // outage: next sample hours later, far beyond the 180 s gate
        let late = b(12, 0);
        apply(&mut st, 50000, 1000 + 4 * 3600, D1, late);
        // missed buckets stay -1 (no bar, not a wrong bar)
        for i in (bk + 1)..late {
            assert_eq!(st.kwh[i], -1.0, "bucket {i}");
        }
        // the recovery bucket starts at the CURRENT reading: no giant bar
        assert_eq!(st.wh_start[late], 50000.0);
        assert_eq!(st.kwh[late], 0.0);
    }

    #[test]
    fn future_last_ts_blocks_carry() {
        let mut st = DayState::new(D1);
        let bk = b(14, 0);
        apply(&mut st, 5000, 5000, D1, bk); // last_ts = 5000
        let next = bk + 1;
        apply(&mut st, 6000, 4000, D1, next); // now < last_ts (clock jump)
                                              // the carry gate requires last_ts <= now; a "future" last sample
                                              // must not seed the new bucket from it
        assert_eq!(st.wh_start[next], 6000.0);
    }

    #[test]
    fn midnight_rollover_on_sample() {
        let mut st = DayState::new(D1);
        apply(&mut st, 20000, 1000, D1, b(23, 3));
        let d2 = "2026-11-20";
        apply(&mut st, 0, 1000 + 900, d2, b(0, 0));
        for i in 0..N_BUCKETS {
            if i == 0 {
                continue;
            }
            assert_eq!(st.kwh[i], -1.0, "bucket {i}");
        }
        assert_eq!(st.date, d2);
        assert_eq!(st.peak_w, 0);
        // the new day's first bucket starts from the new reading
        assert_eq!(st.kwh[0], 0.0);
    }

    #[test]
    fn peak_tracking_and_rollover_reset() {
        let mut st = DayState::new(D1);
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 1000,
                watts_now: 100,
                now: 10,
            },
            D1,
            b(8, 0),
        );
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 2000,
                watts_now: 500,
                now: 70,
            },
            D1,
            b(8, 0),
        );
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 3000,
                watts_now: 300,
                now: 130,
            },
            D1,
            b(8, 0),
        );
        assert_eq!(st.peak_w, 500);
        rollover(&mut st, "2026-11-20");
        assert_eq!(st.peak_w, 0);
        assert_eq!(st.watts_now, 0);
        assert_eq!(st.wh_today, 0);
        assert_eq!(st.wh_7day, 0);
    }

    #[test]
    fn cached_cards_track_latest_sample() {
        let mut st = DayState::new(D1);
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 12_345,
                watts_now: 2_000,
                now: 100,
            },
            D1,
            b(11, 1),
        );
        assert_eq!(st.watts_now, 2_000);
        assert_eq!(st.wh_today, 12_345);
        st.wh_7day = 900_000; // served from the same reading; set by the poller
        assert_eq!(st.wh_7day, 900_000);
    }

    #[test]
    fn future_buckets_stay_empty() {
        let mut st = DayState::new(D1);
        apply(&mut st, 5000, 1000, D1, b(9, 2));
        for i in (b(9, 2) + 1)..N_BUCKETS {
            assert_eq!(st.kwh[i], -1.0, "bucket {i}");
        }
    }
}

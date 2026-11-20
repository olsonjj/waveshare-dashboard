//! contract.rs — the versioned push payload: the single seam between the
//! server and the board (spec.md "Implementation Decisions").
//!
//! One JSON object per line, pushed over TCP every second. The board
//! merges nothing: every line is the complete screen state.
//!
//! Wire format (`"v": 1`):
//!
//! ```json
//! {"v":1,"epoch":1763562000,"watts_now":3840,"wh_today":21651,
//!  "watts_peak_today":5120,"wh_7day":987650,
//!  "kwh_15m":[0.0,0.25,...]}
//! ```
//!
//! `epoch` is unix seconds from the host clock; the board derives its
//! header clock and the chart's now-line from it. `kwh_15m` has 96
//! entries (index = hour*4 + min/15); negative entries are future /
//! unknown and render no bar.

use crate::buckets::DayState;
use serde::Serialize;

pub const CONTRACT_VERSION: u32 = 1;

#[derive(Debug, Serialize)]
pub struct ScreenState {
    pub v: u32,
    pub epoch: i64,
    pub watts_now: u32,
    pub wh_today: u32,
    pub watts_peak_today: u32,
    pub wh_7day: u32,
    /// kWh per 15-min bucket (96 entries); negative = future/unknown.
    /// Vec because serde's derived array support stops at 32 — the wire
    /// format (a flat 96-element JSON array) is identical.
    pub kwh_15m: Vec<f32>,
}

impl ScreenState {
    /// Build the payload from the day state at wall time `epoch`.
    pub fn from_day(st: &DayState, epoch: i64) -> Self {
        ScreenState {
            v: CONTRACT_VERSION,
            epoch,
            watts_now: st.watts_now,
            wh_today: st.wh_today,
            watts_peak_today: st.peak_w,
            wh_7day: st.wh_7day,
            kwh_15m: st.kwh.to_vec(),
        }
    }

    /// One newline-terminated JSON line — the exact bytes the board reads.
    pub fn to_json_line(&self) -> String {
        let mut s = serde_json::to_string(self).expect("ScreenState serializes");
        s.push('\n');
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::buckets::N_BUCKETS;

    fn day_with(kwh: [f32; N_BUCKETS]) -> DayState {
        let mut st = DayState::new("2026-11-19");
        st.kwh = kwh;
        st.watts_now = 3840;
        st.wh_today = 21651;
        st.peak_w = 5120;
        st.wh_7day = 987_650;
        st
    }

    #[test]
    fn exact_json_line() {
        let mut kwh = [-1.0f32; N_BUCKETS];
        kwh[0] = 0.0;
        kwh[1] = 0.25;
        kwh[2] = -1.0;
        let line = ScreenState::from_day(&day_with(kwh), 1_763_562_000).to_json_line();

        // build the expected kwh_15m array text
        let arr: Vec<String> = kwh
            .iter()
            .map(|v| serde_json::to_string(v).unwrap())
            .collect();
        let expected = format!(
            "{{\"v\":1,\"epoch\":1763562000,\"watts_now\":3840,\"wh_today\":21651,\
             \"watts_peak_today\":5120,\"wh_7day\":987650,\"kwh_15m\":[{}]}}\n",
            arr.join(",")
        );
        assert_eq!(line, expected);
    }

    #[test]
    fn round_trip_through_serde_json() {
        let mut kwh = [-1.0f32; N_BUCKETS];
        for (i, v) in kwh.iter_mut().enumerate() {
            if i % 3 == 0 {
                *v = i as f32 / 100.0;
            }
        }
        let st = day_with(kwh);
        let line = ScreenState::from_day(&st, 42).to_json_line();
        let v: serde_json::Value = serde_json::from_str(line.trim_end()).unwrap();

        assert_eq!(v["v"], 1);
        assert_eq!(v["epoch"], 42);
        assert_eq!(v["watts_now"], 3840);
        assert_eq!(v["wh_today"], 21651);
        assert_eq!(v["watts_peak_today"], 5120);
        assert_eq!(v["wh_7day"], 987650);
        let arr = v["kwh_15m"].as_array().unwrap();
        assert_eq!(arr.len(), 96);
        assert!((arr[3].as_f64().unwrap() - 3.0 / 100.0).abs() < 1e-6);
        assert!(arr[1].as_f64().unwrap() < 0.0, "unset buckets are negative");
    }

    #[test]
    fn fresh_day_serializes_all_negative_buckets() {
        let st = DayState::new("2026-11-19");
        let v: serde_json::Value =
            serde_json::from_str(ScreenState::from_day(&st, 0).to_json_line().trim_end()).unwrap();
        let arr = v["kwh_15m"].as_array().unwrap();
        assert_eq!(arr.len(), 96);
        assert!(arr.iter().all(|x| x.as_f64().unwrap() < 0.0));
    }
}

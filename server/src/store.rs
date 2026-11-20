//! store.rs — SQLite persistence of today's bucket state.
//!
//! One table, one row per local day (spec "Implementation Decisions"):
//!
//! ```sql
//! CREATE TABLE day (
//!   date       TEXT PRIMARY KEY,   -- 'YYYY-MM-DD' local Pacific
//!   kwh_15m    BLOB,               -- 96 × f32 LE
//!   wh_start   BLOB,               -- 96 × f32 LE, -1 = unset
//!   last_wh    INTEGER,
//!   last_ts    INTEGER,
//!   peak_w     INTEGER
//! );
//! ```
//!
//! A mid-day restart reloads the row for today and continues: the bucket
//! baselines (`wh_start`) and the last accepted sample survive, so no
//! production is double-counted or lost across the restart.
//! Retention is today only — rows for other dates are pruned.

use rusqlite::{params, Connection};

use crate::buckets::{DayState, N_BUCKETS};

pub fn open(path: &str) -> rusqlite::Result<Connection> {
    let conn = Connection::open(path)?;
    conn.execute(
        "CREATE TABLE IF NOT EXISTS day (
           date       TEXT PRIMARY KEY,
           kwh_15m    BLOB,
           wh_start   BLOB,
           last_wh    INTEGER,
           last_ts    INTEGER,
           peak_w     INTEGER
         )",
        [],
    )?;
    Ok(conn)
}

fn f32_blob(v: &[f32]) -> Vec<u8> {
    let mut b = Vec::with_capacity(v.len() * 4);
    for x in v {
        b.extend_from_slice(&x.to_le_bytes());
    }
    b
}

fn blob_f32(buf: &[u8]) -> Option<[f32; N_BUCKETS]> {
    if buf.len() != N_BUCKETS * 4 {
        return None;
    }
    let mut out = [0.0f32; N_BUCKETS];
    for (i, v) in out.iter_mut().enumerate() {
        let mut b = [0u8; 4];
        b.copy_from_slice(&buf[i * 4..i * 4 + 4]);
        *v = f32::from_le_bytes(b);
    }
    Some(out)
}

/// Load the row for `date`, if present and well-formed. A row whose blobs
/// are the wrong size is treated as absent (fresh start) rather than
/// poisoning the day with half-decoded state.
pub fn load_today(conn: &Connection, date: &str) -> Option<DayState> {
    let row = conn
        .query_row(
            "SELECT kwh_15m, wh_start, last_wh, last_ts, peak_w
               FROM day WHERE date = ?1",
            params![date],
            |r| {
                Ok((
                    r.get::<_, Vec<u8>>(0)?,
                    r.get::<_, Vec<u8>>(1)?,
                    r.get::<_, i64>(2)?,
                    r.get::<_, i64>(3)?,
                    r.get::<_, i64>(4)?,
                ))
            },
        )
        .ok()?;
    let (kwh_b, whs_b, last_wh, last_ts, peak_w) = row;
    let kwh = blob_f32(&kwh_b)?;
    let wh_start = blob_f32(&whs_b)?;
    let mut st = DayState::new(date);
    st.kwh = kwh;
    st.wh_start = wh_start;
    st.last_wh = last_wh;
    st.last_ts = last_ts;
    st.peak_w = peak_w.max(0) as u32;
    // Restore the yield card from the last accepted sample so the display
    // is not blank for the up-to-60 s until the next poll; watts_now and
    // wh_7day have no safe stand-in and stay 0 until the next poll.
    if last_wh >= 0 {
        st.wh_today = last_wh as u32;
    }
    // Reconstruct which bucket the last sample landed in so a sample that
    // arrives in the SAME bucket keeps its live partial (wh_start[b] is
    // already set) rather than re-seeding.
    st.last_bucket = st
        .wh_start
        .iter()
        .rposition(|w| *w >= 0.0)
        .map(|i| i as i32)
        .unwrap_or(-1);
    Some(st)
}

/// Persist `st` as the row for its date.
pub fn save_today(conn: &Connection, st: &DayState) -> rusqlite::Result<()> {
    conn.execute(
        "INSERT INTO day (date, kwh_15m, wh_start, last_wh, last_ts, peak_w)
           VALUES (?1, ?2, ?3, ?4, ?5, ?6)
           ON CONFLICT(date) DO UPDATE SET
             kwh_15m = excluded.kwh_15m,
             wh_start = excluded.wh_start,
             last_wh = excluded.last_wh,
             last_ts = excluded.last_ts,
             peak_w = excluded.peak_w",
        params![
            st.date,
            f32_blob(&st.kwh),
            f32_blob(&st.wh_start),
            st.last_wh,
            st.last_ts,
            st.peak_w as i64,
        ],
    )?;
    Ok(())
}

/// Delete rows for dates other than `date` (retention: today only).
pub fn prune(conn: &Connection, date: &str) -> rusqlite::Result<usize> {
    conn.execute("DELETE FROM day WHERE date != ?1", params![date])
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::buckets::{apply_sample, rollover, Sample};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn tmp_db() -> Connection {
        let path = std::env::temp_dir().join(format!(
            "solar-push-test-{}-{:x}.db",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        open(path.to_str().unwrap()).expect("temp db opens")
    }

    const D1: &str = "2026-11-19";

    #[test]
    fn round_trip_mid_day_restart_resumes_without_double_count() {
        let conn = tmp_db();
        let mut st = DayState::new(D1);
        // morning: two samples in bucket 20 (05:00-05:15)
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 1000,
                watts_now: 100,
                now: 1000,
            },
            D1,
            20,
        );
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 2000,
                watts_now: 200,
                now: 1060,
            },
            D1,
            20,
        );
        assert_eq!(st.kwh[20], 1.0);
        save_today(&conn, &st).unwrap();

        // "restart": reload and keep folding samples in the same bucket
        let mut re = load_today(&conn, D1).expect("row reloads");
        assert_eq!(re.kwh[20], 1.0);
        assert_eq!(re.wh_start[20], 1000.0);
        assert_eq!(re.wh_today, 2000); // card restored from last_wh
        assert_eq!(re.peak_w, 200);
        assert_eq!(re.last_bucket, 20);
        apply_sample(
            &mut re,
            &Sample {
                wh_today: 3000,
                watts_now: 150,
                now: 1120,
            },
            D1,
            20,
        );
        // continued from the persisted baseline: (3000-1000)/1000, no
        // re-seed at 2000 (which would have shown 1.0)
        assert_eq!(re.kwh[20], 2.0);
    }

    #[test]
    fn restart_across_bucket_boundary_within_gate_carries_from_last_wh() {
        let conn = tmp_db();
        let mut st = DayState::new(D1);
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 5000,
                watts_now: 10,
                now: 2000,
            },
            D1,
            40,
        );
        save_today(&conn, &st).unwrap();

        let mut re = load_today(&conn, D1).unwrap();
        // first sample after restart lands in the NEXT bucket 55 s after
        // the stored sample: adjacent and within the 180 s gate
        apply_sample(
            &mut re,
            &Sample {
                wh_today: 6500,
                watts_now: 10,
                now: 2055,
            },
            D1,
            41,
        );
        assert_eq!(re.wh_start[41], 5000.0); // carried across the boundary
        assert_eq!(re.kwh[41], 1.5);
    }

    #[test]
    fn absent_or_corrupt_row_is_fresh_start() {
        let conn = tmp_db();
        assert!(load_today(&conn, D1).is_none());

        conn.execute(
            "INSERT INTO day (date, kwh_15m, wh_start, last_wh, last_ts, peak_w)
               VALUES (?1, x'00', x'00', -1, 0, 0)",
            params![D1],
        )
        .unwrap();
        assert!(load_today(&conn, D1).is_none(), "short blob -> fresh start");
    }

    #[test]
    fn other_dates_are_pruned() {
        let conn = tmp_db();
        save_today(&conn, &DayState::new("2026-11-18")).unwrap();
        save_today(&conn, &DayState::new(D1)).unwrap();
        assert_eq!(prune(&conn, D1).unwrap(), 1);
        assert!(load_today(&conn, D1).is_some());
        assert!(load_today(&conn, "2026-11-18").is_none());
    }

    #[test]
    fn rollover_saves_empty_new_day() {
        let conn = tmp_db();
        let mut st = DayState::new(D1);
        apply_sample(
            &mut st,
            &Sample {
                wh_today: 9000,
                watts_now: 900,
                now: 5000,
            },
            D1,
            50,
        );
        rollover(&mut st, "2026-11-20");
        save_today(&conn, &st).unwrap();
        let re = load_today(&conn, "2026-11-20").unwrap();
        assert!(re.kwh.iter().all(|k| *k < 0.0));
        assert_eq!(re.peak_w, 0);
    }
}

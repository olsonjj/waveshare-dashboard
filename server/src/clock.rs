//! clock.rs — local-time parts for the bucket accumulator.
//!
//! The pure accumulator in `buckets.rs` takes the LOCAL date and 15-min
//! bucket of a sample's epoch as inputs; this module computes them for
//! the Pacific time zone (the dashboard's audience), DST-aware via the
//! IANA database. Kept separate so `buckets.rs` stays dependency-free
//! and deterministic.

use chrono::{TimeZone, Timelike, Utc};
use chrono_tz::America::Los_Angeles;

/// Local-time parts of `epoch` (unix seconds) in US Pacific.
pub struct LocalParts {
    /// "YYYY-MM-DD"
    pub date: String,
    /// 15-minute bucket index: hour*4 + minute/15
    pub bucket: usize,
}

pub fn local_parts(epoch: i64) -> LocalParts {
    let dt = Utc
        .timestamp_opt(epoch, 0)
        .single()
        .expect("epoch within chrono's datetime range")
        .with_timezone(&Los_Angeles);
    LocalParts {
        date: dt.format("%Y-%m-%d").to_string(),
        bucket: dt.hour() as usize * 4 + dt.minute() as usize / 15,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_to_expected_date_and_bucket() {
        // 2026-11-19 09:37:00 PST (UTC-8)
        let lp = local_parts(1_795_109_820);
        assert_eq!(lp.date, "2026-11-19");
        assert_eq!(lp.bucket, 9 * 4 + 2); // 09:30-09:45
    }

    #[test]
    fn bucket_zero_at_local_midnight() {
        // 2026-11-19 00:00:00 PST = 08:00 UTC
        let lp = local_parts(1_795_075_200);
        assert_eq!(lp.date, "2026-11-19");
        assert_eq!(lp.bucket, 0);
    }

    #[test]
    fn dst_spring_forward_keeps_date_continuous() {
        // 2026-03-08 02:00 does not exist (PST -> PDT); 03:00 PDT =
        // 10:00 UTC. The local date must be the 8th either side.
        let before = local_parts(1_772_956_799); // 2026-03-07 23:59:59 PST
        let after = local_parts(1_772_964_000); //  2026-03-08 03:00:00 PDT
        assert_eq!(before.date, "2026-03-07");
        assert_eq!(after.date, "2026-03-08");
        assert_eq!(after.bucket, 3 * 4); // 03:00 bucket
    }

    #[test]
    fn dst_fall_back_repeats_an_hour_without_warping_buckets() {
        // 2026-11-01: 01:00 PDT -> 01:00 PST; epochs 1761961200 (01:00
        // PDT, bucket 4) and 1761964800 (01:00 PST, bucket 4) both map to
        // the same local date and bucket — the accumulator is idempotent
        // across the repeated hour (a fresh sample at the same bucket
        // simply updates the live partial).
        let a = local_parts(1_793_520_000);
        let b = local_parts(1_793_523_600);
        assert_eq!(a.date, b.date);
        assert_eq!(a.date, "2026-11-01");
        assert_eq!(a.bucket, b.bucket);
        assert_eq!(a.bucket, 4);
    }
}

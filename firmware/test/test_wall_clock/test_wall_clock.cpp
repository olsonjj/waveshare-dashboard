/*
 * test_wall_clock.cpp — host unit tests for the extracted wall-clock
 * parse/seed module (src/wall_clock.cpp). Tests run with TZ=PST8PDT set
 * in setUp, the same rules the board uses, so mktime pins real PST/PDT
 * offsets. Each expected epoch is hand-computed in a comment; every
 * constant was cross-checked against the host `date` tool in both
 * directions (BSD date: `date -r <epoch>` / `date -j -f ... +%s`).
 */
#include <unity.h>

#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "wall_clock.h"

/* Same TZ string main.cpp installs before seeding (must precede mktime). */
static const char *TZ_PACIFIC = "PST8PDT,M3.2.0,M11.1.0";

/* Fake local-time seam for the time-only form: fixed UTC "timezone" so the
 * injected date is fully deterministic. */
static struct tm* test_localtime(const time_t* t, struct tm* out)
{
    return gmtime_r(t, out);
}

/* 2026-11-15 00:00:00 UTC (see arithmetic in test 7). */
static const time_t NOW_NOV15_UTC = 1794700800;

void setUp(void)
{
    setenv("TZ", TZ_PACIFIC, 1);
    tzset();
}

void tearDown(void) {}

/* Sentinel-filler for "out must be untouched on failure" assertions. */
static void poison(struct tm* t)
{
    memset(t, 0, sizeof(*t));
    t->tm_year = -77; t->tm_mon = -77; t->tm_mday = -77;
    t->tm_hour = -77; t->tm_min = -77; t->tm_sec = -77;
    t->tm_isdst = -77;
}
static bool untouched(const struct tm* t)
{
    return t->tm_year == -77 && t->tm_mon == -77 && t->tm_mday == -77 &&
           t->tm_hour == -77 && t->tm_min == -77 && t->tm_sec == -77 &&
           t->tm_isdst == -77;
}

/* Shared day-count anchor (used by the epoch comments below):
 * 1970-01-01 -> 2026-01-01 = 56 years, of which 14 are leap (1972..2024)
 * = 56*365 + 14 = 20454 days = 1,767,225,600 s.
 * 2026-01-01 -> 2026-11-01 = 31+28+31+30+31+30+31+31+30+31 = 304 days. */

/* 1. Build stamp -> correct fields + the known PST epoch.
 * 2026-11-15 is AFTER DST end (first Sunday of Nov 2026 = Nov 1), so
 * 14:32:57 PST = 22:32:57 UTC:
 *   1,767,225,600 + 318*86400 (= 2026-11-15, 304 + 14 days)
 *   = 1,794,700,800 UTC midnight; + 22*3600 + 32*60 + 57 = 81,177
 *   -> 1,794,781,977.
 * (The task brief's 1763254377 was wrong: `date -r 1763254377 -u` gives
 * 2025-11-16 00:52:57 UTC. Verified forward too: `TZ=PST8PDT,M3.2.0,
 * M11.1.0 date -j -f "%b %d %Y %H:%M:%S" "Nov 15 2026 14:32:57" +%s`
 * -> 1794781977.) */
void test_build_stamp_nov2026_fields_and_pst_epoch(void)
{
    struct tm tmbuf;
    TEST_ASSERT_TRUE(wc_parse_build_stamp("Nov 15 2026", "14:32:57", &tmbuf));
    TEST_ASSERT_EQUAL_INT(126, tmbuf.tm_year);   /* 2026 - 1900 */
    TEST_ASSERT_EQUAL_INT(10,  tmbuf.tm_mon);    /* Nov, 0-based */
    TEST_ASSERT_EQUAL_INT(15,  tmbuf.tm_mday);
    TEST_ASSERT_EQUAL_INT(14,  tmbuf.tm_hour);
    TEST_ASSERT_EQUAL_INT(32,  tmbuf.tm_min);
    TEST_ASSERT_EQUAL_INT(57,  tmbuf.tm_sec);
    TEST_ASSERT_EQUAL_INT(-1,  tmbuf.tm_isdst);  /* TZ decides at mktime */
    TEST_ASSERT_EQUAL_INT64(1794781977LL, (long long)wc_tm_to_epoch(&tmbuf));
}

/* 2. Summer stamp resolves to PDT (UTC-7), not PST — pins tm_isdst = -1
 * plus the TZ rules doing the DST lookup at mktime time. 2026-07-04 sits
 * inside DST (2026-03-08 .. 2026-11-01), so 12:00:00 PDT = 19:00:00 UTC:
 *   1,767,225,600 + (181 + 3)*86400 (Jan-Jun = 181, +3 = Jul 4)
 *   = 1,783,123,200; + 19*3600 = 68,400 -> 1,783,191,600.
 * A PST (UTC-8) reading would be 3600 s higher (= 1,783,195,200), so the
 * asserted epoch pins the offset at exactly -7 h. */
void test_build_stamp_july2026_is_pdt(void)
{
    struct tm tmbuf;
    TEST_ASSERT_TRUE(wc_parse_build_stamp("Jul 4 2026", "12:00:00", &tmbuf));
    TEST_ASSERT_EQUAL_INT(-1, tmbuf.tm_isdst);
    /* 12:00 PST (UTC-8) would be 20:00 UTC = 1,783,195,200; the observed
     * PDT (UTC-7) epoch below pins the offset at exactly -7 h. */
    TEST_ASSERT_EQUAL_INT64(1783191600LL, (long long)wc_tm_to_epoch(&tmbuf));
}

/* 3. Winter stamp resolves to PST (UTC-8). 2026-01-04 12:00:00 PST
 * = 20:00:00 UTC: 1,767,225,600 + 3*86400 (= Jan 4) = 1,767,484,800;
 * + 20*3600 = 72,000 -> 1,767,556,800. */
void test_build_stamp_january2026_is_pst(void)
{
    struct tm tmbuf;
    TEST_ASSERT_TRUE(wc_parse_build_stamp("Jan 4 2026", "12:00:00", &tmbuf));
    TEST_ASSERT_EQUAL_INT(-1, tmbuf.tm_isdst);
    TEST_ASSERT_EQUAL_INT64(1767556800LL, (long long)wc_tm_to_epoch(&tmbuf));
}

/* 4. Malformed stamps are rejected, out untouched. */
void test_build_stamp_malformed_rejected(void)
{
    struct tm tmbuf;

    poison(&tmbuf);   /* unknown month name */
    TEST_ASSERT_FALSE(wc_parse_build_stamp("Xyz 15 2026", "14:32:57", &tmbuf));
    TEST_ASSERT_TRUE(untouched(&tmbuf));

    poison(&tmbuf);   /* "2026": "%3s %d %d" matches only 2 fields */
    TEST_ASSERT_FALSE(wc_parse_build_stamp("2026", "14:32:57", &tmbuf));
    TEST_ASSERT_TRUE(untouched(&tmbuf));

    poison(&tmbuf);   /* no seconds: "%d:%d:%d" matches only 2 fields */
    TEST_ASSERT_FALSE(wc_parse_build_stamp("Nov 15 2026", "12:00", &tmbuf));
    TEST_ASSERT_TRUE(untouched(&tmbuf));
}

/* 5. Seed policy: seed only when the clock is strictly more than 5 s
 * behind the build epoch (RTC ahead on soft reset must be kept). */
void test_should_seed_bounds(void)
{
    const time_t build = (time_t)1000000000;

    TEST_ASSERT_TRUE(wc_should_seed(build - 60, build));   /* 60 s behind */
    TEST_ASSERT_TRUE(wc_should_seed(build - 6, build));    /* exactly build-6 */
    TEST_ASSERT_FALSE(wc_should_seed(build - 5, build));   /* boundary: == 5 */
    TEST_ASSERT_FALSE(wc_should_seed(build + 10, build));  /* ahead: keep RTC */
    TEST_ASSERT_FALSE(wc_should_seed(build, build));
}

/* 6. Full form "yyyy-mm-dd hh:mm:ss": fields parsed, isdst = -1.
 * 2026-11-15 14:30:00 PST = 22:30:00 UTC = 1,794,700,800 + 22*3600 +
 * 30*60 = 1,794,781,800. */
void test_time_spec_full_form(void)
{
    struct tm tmbuf;
    TEST_ASSERT_TRUE(wc_parse_time_spec("2026-11-15 14:30:00", 0,
                                        test_localtime, &tmbuf));
    TEST_ASSERT_EQUAL_INT(126, tmbuf.tm_year);
    TEST_ASSERT_EQUAL_INT(10,  tmbuf.tm_mon);
    TEST_ASSERT_EQUAL_INT(15,  tmbuf.tm_mday);
    TEST_ASSERT_EQUAL_INT(14,  tmbuf.tm_hour);
    TEST_ASSERT_EQUAL_INT(30,  tmbuf.tm_min);
    TEST_ASSERT_EQUAL_INT(0,   tmbuf.tm_sec);
    TEST_ASSERT_EQUAL_INT(-1,  tmbuf.tm_isdst);
    TEST_ASSERT_EQUAL_INT64(1794781800LL, (long long)wc_tm_to_epoch(&tmbuf));
}

/* 6b. Out-of-range month: the full form does NOT validate ranges — it
 * accepts whatever sscanf matched. This test PINS CURRENT BEHAVIOR
 * (possible future hardening, would be a deliberate behavior change).
 * mktime normalizes unvalidated tm_mon = 12 (the 13th month) to Jan 2027:
 * 2027-01-01 10:00:00 PST = 18:00:00 UTC = (20454 + 365)*86400
 * = 20819*86400 = 1,798,761,600; + 18*3600 = 64,800 -> 1,798,826,400. */
void test_time_spec_full_form_pins_unvalidated_month(void)
{
    struct tm tmbuf;
    TEST_ASSERT_TRUE(wc_parse_time_spec("2026-13-01 10:00:00", 0,
                                        test_localtime, &tmbuf));
    TEST_ASSERT_EQUAL_INT(126, tmbuf.tm_year);
    TEST_ASSERT_EQUAL_INT(12,  tmbuf.tm_mon);   /* 13th month, unvalidated */
    TEST_ASSERT_EQUAL_INT(1,   tmbuf.tm_mday);
    TEST_ASSERT_EQUAL_INT(10,  tmbuf.tm_hour);
    TEST_ASSERT_EQUAL_INT(-1,  tmbuf.tm_isdst);
    TEST_ASSERT_EQUAL_INT64(1798826400LL, (long long)wc_tm_to_epoch(&tmbuf));
}

/* 7. Time-only form: date comes from the injected now_epoch via lt,
 * h:mm:ss replaces the time, isdst = -1. Fake lt is gmtime_r, and
 * 1,794,700,800 s = 20454*86400 + 318*86400 = 2026-11-15 00:00:00 UTC,
 * so the injected "today" is Nov 15 2026. */
void test_time_spec_time_only_uses_injected_date(void)
{
    struct tm tmbuf;
    TEST_ASSERT_TRUE(wc_parse_time_spec("14:30:00", NOW_NOV15_UTC,
                                        test_localtime, &tmbuf));
    TEST_ASSERT_EQUAL_INT(126, tmbuf.tm_year);   /* from injected now */
    TEST_ASSERT_EQUAL_INT(10,  tmbuf.tm_mon);
    TEST_ASSERT_EQUAL_INT(15,  tmbuf.tm_mday);
    TEST_ASSERT_EQUAL_INT(14,  tmbuf.tm_hour);   /* replaced */
    TEST_ASSERT_EQUAL_INT(30,  tmbuf.tm_min);
    TEST_ASSERT_EQUAL_INT(0,   tmbuf.tm_sec);
    TEST_ASSERT_EQUAL_INT(-1,  tmbuf.tm_isdst);
}

/* 7b. Time-only rejects h >= 24 and non-numeric garbage, out untouched. */
void test_time_spec_time_only_rejects(void)
{
    struct tm tmbuf;

    poison(&tmbuf);   /* hour out of range */
    TEST_ASSERT_FALSE(wc_parse_time_spec("25:00:00", NOW_NOV15_UTC,
                                         test_localtime, &tmbuf));
    TEST_ASSERT_TRUE(untouched(&tmbuf));

    poison(&tmbuf);   /* matches neither the full nor the time-only form */
    TEST_ASSERT_FALSE(wc_parse_time_spec("hello", NOW_NOV15_UTC,
                                         test_localtime, &tmbuf));
    TEST_ASSERT_TRUE(untouched(&tmbuf));
}

/* 8. Round-trip sanity: parse build stamp -> epoch -> a running clock
 * fixed 10 min behind it must be re-seeded (the stale-stamp bug class:
 * an RTC stuck behind the stamp silently shifted every hourly bucket). */
void test_stamp_to_epoch_to_seed_decision_roundtrip(void)
{
    struct tm tmbuf;
    TEST_ASSERT_TRUE(wc_parse_build_stamp("Nov 15 2026", "14:32:57", &tmbuf));
    time_t build_epoch = wc_tm_to_epoch(&tmbuf);

    TEST_ASSERT_EQUAL_INT64(1794781977LL, (long long)build_epoch);
    TEST_ASSERT_TRUE(wc_should_seed(build_epoch - 600, build_epoch));
    TEST_ASSERT_FALSE(wc_should_seed(build_epoch, build_epoch));
}

/* ---- RFC 7231 Date header parsing (UTC; NOT the PST/PDT mktime path, so
 * these expected epochs are absolute UTC even though setUp sets TZ) ---- */
static const time_t HDR_POISON = (time_t)-77;

/* 9. Valid IMF-fixdate: "Mon, 16 Nov 2026 22:15:41 GMT" -> 1,794,867,341.
 * Hand arithmetic (UTC, no TZ offset):
 *   2026-01-01 00:00 UTC = 1,767,225,600 (20454 days: 56 years x 365 +
 *   14 leap days 1972..2024; the anchor documented above); Nov 16 = day
 *   304 (Jan..Oct) + 15 = 319 -> + 319*86400 = 1,794,787,200;
 *   + 22*3600 + 15*60 + 41 = 80,141 -> 1,794,867,341.
 * (Verified: `date -j -u -f "%a, %d %b %Y %H:%M:%S"
 *   "Mon, 16 Nov 2026 22:15:41" +%s`; reverse `date -r 1794867341 -u`.) */
void test_http_date_nov2026(void)
{
    time_t epoch = HDR_POISON;
    TEST_ASSERT_TRUE(wc_parse_http_date("Mon, 16 Nov 2026 22:15:41 GMT",
                                        &epoch));
    TEST_ASSERT_EQUAL_INT64(1794867341LL, (long long)epoch);
}

/* 10. Leap day pins the leap-year arithmetic. "Tue, 29 Feb 2028 12:00:00
 * GMT" -> 1,835,438,400. Hand arithmetic:
 *   2028-01-01 = (58*365 + 14) days = 21,184 days = 1,830,297,600;
 *   + 59*86400 (Jan 31 + Feb 28 to reach Feb 29) = 1,835,395,200;
 *   + 12*3600 = 43,200 -> 1,835,438,400.
 * (Verified: date -j -u ... "Tue, 29 Feb 2028 12:00:00" +%s) */
void test_http_date_leap_day_2028(void)
{
    time_t epoch = HDR_POISON;
    TEST_ASSERT_TRUE(wc_parse_http_date("Tue, 29 Feb 2028 12:00:00 GMT",
                                        &epoch));
    TEST_ASSERT_EQUAL_INT64(1835438400LL, (long long)epoch);
}

/* 11. Out-of-range month name: rejected, *out untouched. */
void test_http_date_unknown_month_rejected(void)
{
    time_t epoch = HDR_POISON;

    TEST_ASSERT_FALSE(wc_parse_http_date("Mon, 16 Xyz 2026 22:15:41 GMT",
                                         &epoch));  /* unknown month */
    TEST_ASSERT_EQUAL_INT64((long long)HDR_POISON, (long long)epoch);
}

/* 12. Seconds field truncated: rejected, *out untouched. */
void test_http_date_truncated_field_rejected(void)
{
    time_t epoch = HDR_POISON;

    TEST_ASSERT_FALSE(wc_parse_http_date("Mon, 16 Nov 2026 22:15 GMT",
                                         &epoch));  /* truncated field */
    TEST_ASSERT_EQUAL_INT64((long long)HDR_POISON, (long long)epoch);
}

/* 13. Missing GMT designator (RFC 7231 mandates GMT): rejected, *out
 * untouched. */
void test_http_date_missing_gmt_rejected(void)
{
    time_t epoch = HDR_POISON;

    TEST_ASSERT_FALSE(wc_parse_http_date("Mon, 16 Nov 2026 22:15:41",
                                         &epoch));  /* missing GMT */
    TEST_ASSERT_EQUAL_INT64((long long)HDR_POISON, (long long)epoch);
}

/* 14. Sync gate: nudge only when |now - source| > 5 s, either direction
 * (boundary style mirrors test_should_seed_bounds). */
void test_should_sync_bounds(void)
{
    const time_t now = (time_t)1000000000;

    TEST_ASSERT_TRUE(wc_should_sync(now, now + 6));    /* source ahead +6 */
    TEST_ASSERT_FALSE(wc_should_sync(now, now + 5));   /* boundary: +5 */
    TEST_ASSERT_TRUE(wc_should_sync(now, now - 6));    /* source behind -6 */
    TEST_ASSERT_FALSE(wc_should_sync(now, now - 5));   /* boundary: -5 */
    TEST_ASSERT_FALSE(wc_should_sync(now, now));       /* equal: nothing */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_stamp_nov2026_fields_and_pst_epoch);
    RUN_TEST(test_build_stamp_july2026_is_pdt);
    RUN_TEST(test_build_stamp_january2026_is_pst);
    RUN_TEST(test_build_stamp_malformed_rejected);
    RUN_TEST(test_should_seed_bounds);
    RUN_TEST(test_time_spec_full_form);
    RUN_TEST(test_time_spec_full_form_pins_unvalidated_month);
    RUN_TEST(test_time_spec_time_only_uses_injected_date);
    RUN_TEST(test_time_spec_time_only_rejects);
    RUN_TEST(test_stamp_to_epoch_to_seed_decision_roundtrip);
    RUN_TEST(test_http_date_nov2026);
    RUN_TEST(test_http_date_leap_day_2028);
    RUN_TEST(test_http_date_unknown_month_rejected);
    RUN_TEST(test_http_date_truncated_field_rejected);
    RUN_TEST(test_http_date_missing_gmt_rejected);
    RUN_TEST(test_should_sync_bounds);
    return UNITY_END();
}

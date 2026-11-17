/*
 * wall_clock.h — wall-clock parse/seed policy, pure C++ (no Arduino).
 *
 * Mechanical extraction of the parse_time_spec / build-stamp / seed-policy
 * logic from main.cpp so the stale-stamp bug class (wrong epoch -> wrong
 * hour attribution in the hourly buckets, false midnight rollovers) can be
 * unit-tested on the host. Board specifics (the running RTC via time(),
 * localtime_r, TZ setup) are injected through the seams below.
 *
 * TZ contract (unchanged from main.cpp): the caller must setenv("TZ",
 * "PST8PDT,M3.2.0,M11.1.0", 1) + tzset() BEFORE any parse -> mktime use;
 * every parsed struct tm carries tm_isdst = -1 so the TZ rules decide
 * PST vs PDT at mktime time.
 */
#ifndef WALL_CLOCK_H
#define WALL_CLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Local-time seam: localtime_r on the board, gmtime_r (or a fake) in tests */
typedef struct tm* (*WallClockLocalTime)(const time_t*, struct tm*);

/* Parse "yyyy-mm-dd hh:mm:ss" (full) or "hh:mm:ss" (time-only; date
 * taken from now_epoch via lt). Validation identical to today: full
 * form accepts whatever sscanf matched (same fields); time-only form
 * requires 0 <= h < 24. Returns false on no match (out untouched).
 * out->tm_isdst = -1 in both forms (TZ rules decide PST/PDT at mktime
 * time). */
bool wc_parse_time_spec(const char* s, time_t now_epoch,
                        WallClockLocalTime lt, struct tm* out);

/* Parse BUILD_STAMP_DATE ("Mon DD YYYY", %3s %d %d) and
 * BUILD_STAMP_TIME ("%d:%d:%d") into a struct tm with tm_isdst = -1.
 * Unknown month name or malformed input -> false (out untouched). */
bool wc_parse_build_stamp(const char* date, const char* time_, struct tm* out);

/* Seed policy: seed only when the running clock is behind the build
 * epoch by more than 5 s (RTC ahead on soft reset must be kept). */
bool wc_should_seed(time_t now_epoch, time_t build_epoch);

/* Parse an RFC 7231 Date header VALUE, e.g. "Mon, 16 Nov 2026 22:15:41 GMT"
 * (IMF-fixdate; the leading weekday token is accepted but not validated),
 * into an absolute UTC epoch. Pure days-from-civil arithmetic — NO mktime
 * and NO TZ machinery (the header is absolute UTC; newlib has no timegm).
 * Range-checked: day 1-31, hour 0-23, minute 0-59, second 0-60 (60 is the
 * tolerated leap second), year 1970-2100. Returns false with *out
 * untouched on any parse/validation failure. */
bool wc_parse_http_date(const char* hdr, time_t* out);

/* Envoy clock-sync gate (relay-era policy): true iff the running clock and
 * the header epoch differ by MORE than 5 s, either direction. */
bool wc_should_sync(time_t now_epoch, time_t source_epoch);

/* epoch for a parsed tm under the current TZ (mktime wrapper, named
 * for clarity at call sites). */
time_t wc_tm_to_epoch(struct tm* t);

#endif /* WALL_CLOCK_H */

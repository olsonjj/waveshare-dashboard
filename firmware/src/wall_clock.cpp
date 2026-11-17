/*
 * wall_clock.cpp — pure wall-clock parse/seed policy extracted from
 * main.cpp. Validation and semantics are unchanged (mechanical move);
 * see wall_clock.h for the seam contract.
 */
#include "wall_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool wc_parse_time_spec(const char *s, time_t now_epoch,
                        WallClockLocalTime lt, struct tm *out)
{
    /* full "yyyy-mm-dd hh:mm:ss" */
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6) {
        memset(out, 0, sizeof(*out));
        out->tm_year = y - 1900; out->tm_mon = mo - 1; out->tm_mday = d;
        out->tm_hour = h; out->tm_min = mi; out->tm_sec = se;
        out->tm_isdst = -1;
        return true;
    }
    /* "hh:mm:ss" -> keep today's date from the running clock */
    if (sscanf(s, "%d:%d:%d", &h, &mi, &se) == 3 && h >= 0 && h < 24) {
        lt(&now_epoch, out);
        out->tm_hour = h; out->tm_min = mi; out->tm_sec = se;
        out->tm_isdst = -1;
        return true;
    }
    return false;
}

bool wc_parse_build_stamp(const char *date, const char *time_, struct tm *out)
{
    static const char *const months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                          "Jul","Aug","Sep","Oct","Nov","Dec" };
    char mon[4] = {0};
    int day, year, hour, minute, sec;
    if (sscanf(date, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(time_, "%d:%d:%d", &hour, &minute, &sec) != 3) {
        return false;
    }
    int mon_idx = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) { mon_idx = i; break; }
    }
    if (mon_idx < 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_year = year - 1900; out->tm_mon = mon_idx; out->tm_mday = day;
    out->tm_hour = hour; out->tm_min = minute; out->tm_sec = sec;
    out->tm_isdst = -1;                 /* let TZ rules decide PST vs PDT */
    return true;
}

bool wc_should_seed(time_t now_epoch, time_t build_epoch)
{
    return now_epoch < build_epoch - 5;
}

/* days_from_civil (Howard Hinnant's algorithm, leap-safe incl. century
 * rule): days since 1970-01-01 for the proleptic Gregorian date. Used
 * instead of mktime because the HTTP Date header is absolute UTC (and
 * newlib has no timegm). */
static long wc_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);            /* [0, 399] */
    const unsigned doy =
        (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

bool wc_parse_http_date(const char *hdr, time_t *out)
{
    static const char *const months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                          "Jul","Aug","Sep","Oct","Nov","Dec" };
    char wday[17] = {0};   /* %16[A-Za-z] writes at most 16 chars + NUL */
    char mon[4] = {0};
    int day, year, hour, minute, sec, mon_idx;

    /* Leading weekday token: accepted and NOT validated (RFC 7231
     * deprecates the weekday anyway; it is for human readers only). The
     * scanset stops at the comma, whose literal match also rejects a
     * comma-less or digit-led prefix. */
    if (sscanf(hdr, "%16[A-Za-z], %d %3s %d %d:%d:%d",
               wday, &day, mon, &year, &hour, &minute, &sec) != 7) {
        return false;
    }
    /* RFC 7231 mandates the GMT designator; a literal in the sscanf format
     * would be invisible to the return count if missing, so check the
     * tail explicitly (this also rejects junk after the value). */
    size_t n = strlen(hdr);
    if (n < 4 || strcmp(hdr + n - 4, " GMT") != 0) {
        return false;
    }
    mon_idx = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) { mon_idx = i; break; }
    }
    /* 60 is the tolerated leap second; day is capped at 31 without a
     * per-month length table (a short-month day-31 never appears on the
     * wire, and range validation follows the spec, not the calendar). */
    if (mon_idx < 0 ||
        day < 1 || day > 31 ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        sec < 0 || sec > 60 ||
        year < 1970 || year > 2100) {
        return false;
    }
    *out = (time_t)wc_days_from_civil(year, mon_idx + 1, day) * 86400L
           + (time_t)hour * 3600 + (time_t)minute * 60 + (time_t)sec;
    return true;
}

bool wc_should_sync(time_t now_epoch, time_t source_epoch)
{
    /* relay-era policy: nudge only when the drift exceeds 5 s, either
     * direction (same gate as fetch_solar_relay's epoch sync). */
    return llabs((long long)(now_epoch - source_epoch)) > 5;
}

time_t wc_tm_to_epoch(struct tm *t)
{
    return mktime(t);
}

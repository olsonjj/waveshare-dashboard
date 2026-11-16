/*
 * wall_clock.cpp — pure wall-clock parse/seed policy extracted from
 * main.cpp. Validation and semantics are unchanged (mechanical move);
 * see wall_clock.h for the seam contract.
 */
#include "wall_clock.h"

#include <stdio.h>
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

time_t wc_tm_to_epoch(struct tm *t)
{
    return mktime(t);
}

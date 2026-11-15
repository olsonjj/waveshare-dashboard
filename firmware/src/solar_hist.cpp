/*
 * solar_hist.cpp — on-board hourly kWh buckets (pure, host-testable).
 *
 * Mechanical extraction of the hist_* block from dashboard_data.cpp.
 * Logic unchanged: bucket h = wh_today(end) - wh_today(hour start); the
 * current hour is a live partial; -1 = no data this hour. Board specifics
 * (NVS, clock, serial) arrive through the SolarHistStore /
 * SolarHistLocalTime / SolarHistLog seams so the logic runs in host tests.
 */
#include "solar_hist.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---------- module state ---------- */
static SolarHistStore*    g_store = nullptr;
static SolarHistLocalTime g_localtime = nullptr;
static SolarHistLog       g_log = nullptr;

static char     g_hist_date[11] = "";    /* local "YYYY-MM-DD"          */
static float    g_hist_kwh[24];
static int32_t  g_hist_wh_start[24];
static int32_t  g_hist_last_wh = -1;
static int      g_hist_last_hour = -1;
static int64_t  g_hist_last_ts = 0;       /* epoch of last accepted sample; 0 = unknown */

/* The whole snapshot is stored as ONE versioned blob: a power failure or
 * reset between five separate Preferences commits used to pair a new date
 * with yesterday's buckets (midnight rollover then never fired).
 * Packed so the NVS byte size is compiler-independent. */
#define HIST_SNAP_VER 2u
#define HIST_CARRY_FRESH_S 900                    /* 3 poll intervals of 5 min */
struct __attribute__((packed)) HistSnap {
    uint8_t  ver;
    char     date[11];                     /* "YYYY-MM-DD" + NUL          */
    float    kwh[24];
    int32_t  wh_start[24];
    int32_t  last_wh;
    int32_t  last_hour;
    int64_t  last_ts;                     /* epoch of last sample; 0 = unknown.
                                           * v2: gates the hour-boundary carry so
                                           * energy from a skipped poll outage is
                                           * not all dumped into the recovery hour */
};

/* ver-1 blob (written by the previous firmware revision): same layout as
 * HistSnap WITHOUT the trailing int64_t last_ts field. Defined separately
 * (not reinterpreted as a HistSnap) so the field copy is explicit. */
struct __attribute__((packed)) HistSnapV1 {
    uint8_t  ver;
    char     date[11];
    float    kwh[24];
    int32_t  wh_start[24];
    int32_t  last_wh;
    int32_t  last_hour;
};
static_assert(sizeof(HistSnapV1) == sizeof(HistSnap) - sizeof(int64_t),
              "v1 snapshot must be HistSnap without last_ts");

/* one-time migration from the pre-blob firmware's five separate legacy keys
 * ("date" String, "kwh" bytes, "whstart" bytes, "lastwh", "lasthour"):
 * only tried when the "snap" blob is absent or invalid, so a valid v2
 * snapshot is never overwritten. last_ts is unknown (0) — the carry gate
 * simply stays closed until the first accepted sample of the new firmware. */
static void hist_migrate_legacy(void)
{
    if (g_store->bytesLen("kwh") != 24 * sizeof(float))
        return;
    char datebuf[16];                  /* oversize dates must fail the ==10 check */
    if (g_store->getString("date", datebuf, sizeof(datebuf)) != 10)
        return;
    if (g_store->bytesLen("whstart") != 24 * sizeof(int32_t))
        return;

    HistSnap snap;
    memset(&snap, 0, sizeof(snap));
    snap.ver = HIST_SNAP_VER;
    if (g_store->getBytes("kwh", snap.kwh, sizeof(snap.kwh)) != sizeof(snap.kwh)
        || g_store->getBytes("whstart", snap.wh_start, sizeof(snap.wh_start)) != sizeof(snap.wh_start))
        return;                            /* read failed: fresh start */
    snap.last_wh   = g_store->getInt("lastwh", -1);
    snap.last_hour = g_store->getInt("lasthour", -1);
    snap.last_ts   = 0;
    memcpy(snap.date, datebuf, 10);
    snap.date[10] = '\0';

    /* adopt locally exactly like a loaded snapshot — the data was read
     * successfully, so the running display keeps it REGARDLESS of whether
     * the converted blob could be written back */
    memcpy(g_hist_kwh, snap.kwh, sizeof(g_hist_kwh));
    memcpy(g_hist_wh_start, snap.wh_start, sizeof(g_hist_wh_start));
    g_hist_last_wh   = snap.last_wh;
    g_hist_last_hour = snap.last_hour;
    g_hist_last_ts   = snap.last_ts;
    memcpy(g_hist_date, snap.date, sizeof(g_hist_date));
    g_hist_date[sizeof(g_hist_date) - 1] = '\0';

    /* the legacy keys are the only PERSISTED copy of this data — do not
     * delete them unless the snap blob is confirmed written (NVS may be
     * full). On a failed write the in-memory state above is still correct
     * and the next boot re-reads the legacy keys and retries. */
    size_t written = g_store->putBytes("snap", &snap, sizeof(snap));
    if (written != sizeof(snap)) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[data] hourly history: snapshot save FAILED (%u/%u B) - legacy keys kept",
                 (unsigned)written, (unsigned)sizeof(snap));
        g_log(buf);
        return;
    }

    g_store->remove("date");
    g_store->remove("kwh");
    g_store->remove("whstart");
    g_store->remove("lastwh");
    g_store->remove("lasthour");
    g_log("[data] hourly history: migrated legacy snapshot");
}

static void hist_load(void)
{
    g_store->begin();
    HistSnap snap;
    size_t n = g_store->bytesLen("snap");
    bool have_snap = false;

    if (n == sizeof(snap) && g_store->getBytes("snap", &snap, sizeof(snap)) == sizeof(snap)
        && snap.ver == HIST_SNAP_VER) {
        have_snap = true;                  /* adopt below */
    } else if (n == sizeof(HistSnapV1)) {
        /* old ver-1 blob (same layout without last_ts): convert in place so
         * a firmware upgrade does not discard the saved bars. Runs BEFORE
         * the five-key migration — a v1 snap and legacy five-key data are
         * never both present in practice. */
        HistSnapV1 v1;
        if (g_store->getBytes("snap", &v1, sizeof(v1)) == sizeof(v1) && v1.ver == 1u) {
            HistSnap up;
            memset(&up, 0, sizeof(up));
            up.ver = HIST_SNAP_VER;
            memcpy(up.date,     v1.date,     sizeof(up.date));
            memcpy(up.kwh,      v1.kwh,      sizeof(up.kwh));
            memcpy(up.wh_start, v1.wh_start, sizeof(up.wh_start));
            up.last_wh   = v1.last_wh;
            up.last_hour = v1.last_hour;
            up.last_ts   = 0;              /* unknown: carry gate stays closed
                                            * until the first accepted sample */
            g_store->putBytes("snap", &up, sizeof(up));
            snap = up;
            have_snap = true;
            g_log("[data] hourly history: upgraded v1 snapshot");
        }
    }

    if (have_snap) {
        memcpy(g_hist_kwh, snap.kwh, sizeof(g_hist_kwh));
        memcpy(g_hist_wh_start, snap.wh_start, sizeof(g_hist_wh_start));
        g_hist_last_wh   = snap.last_wh;
        g_hist_last_hour = snap.last_hour;
        g_hist_last_ts   = snap.last_ts;
        memcpy(g_hist_date, snap.date, sizeof(g_hist_date));
        g_hist_date[sizeof(g_hist_date) - 1] = '\0';
        char buf[96];
        snprintf(buf, sizeof(buf), "[data] hourly history loaded: %s (%d buckets set)",
                 g_hist_date,
                 g_hist_last_hour >= 0 ? g_hist_last_hour + 1 : 0);
        g_log(buf);
        return;
    }

    /* wrong size / unknown version / no snap at all: the five-key legacy
     * migration is the LAST resort, else start fresh */
    for (int i = 0; i < 24; i++) { g_hist_kwh[i] = -1.0f; g_hist_wh_start[i] = -1; }
    g_hist_date[0] = '\0';
    g_hist_last_wh = -1;
    g_hist_last_hour = -1;
    g_hist_last_ts = 0;
    hist_migrate_legacy();
    if (g_hist_date[0] == '\0')
        g_log("[data] hourly history: none stored");
}

/* fold one wh_today sample into the buckets + persist (port of the
 * relay's _apply_sample: hour-boundary close, midnight reset, live partial) */
static void hist_apply(uint32_t wh, time_t now)
{
    struct tm tmv;
    g_localtime(&now, &tmv);
    char today[11];
    strftime(today, sizeof(today), "%Y-%m-%d", &tmv);

    if (strcmp(today, g_hist_date) != 0) {           /* midnight rollover */
        strcpy(g_hist_date, today);
        for (int i = 0; i < 24; i++) { g_hist_kwh[i] = -1.0f; g_hist_wh_start[i] = -1; }
        g_hist_last_wh = -1;
        g_hist_last_hour = -1;
        char buf[64];
        snprintf(buf, sizeof(buf), "[data] hourly: new day %s", today);
        g_log(buf);
    }
    int hour = tmv.tm_hour;
    if (hour != g_hist_last_hour) {                  /* close previous hour */
        if (g_hist_last_hour >= 0 && g_hist_wh_start[g_hist_last_hour] >= 0
            && g_hist_last_wh >= 0) {
            g_hist_kwh[g_hist_last_hour] =
                (g_hist_last_wh - g_hist_wh_start[g_hist_last_hour]) / 1000.0f;
            char buf[96];
            snprintf(buf, sizeof(buf), "[data] hourly: closed hour %d at %.2f kWh",
                     g_hist_last_hour,
                     (double)g_hist_kwh[g_hist_last_hour]);
            g_log(buf);
        }
        /* seed the new hour from the last sample of the old hour — but only
           when the boundary was really the adjacent hour AND the previous
           sample is fresh (gate vs. now_epoch). After a poll outage several
           hours wide, carrying the stale wh would dump ALL of the missed
           energy into this one recovery bar; instead start at the current
           wh and the skipped hours stay -1 (no bar, not a wrong bar). */
        bool hour_adjacent = (g_hist_last_hour + 1) % 24 == hour;
        bool sample_fresh = g_hist_last_ts > 0
                            && (time_t)g_hist_last_ts <= now
                            && (now - (time_t)g_hist_last_ts) <= HIST_CARRY_FRESH_S;
        g_hist_wh_start[hour] = (g_hist_last_wh >= 0 && hour_adjacent && sample_fresh)
                                ? g_hist_last_wh : (int32_t)wh;
        g_hist_last_hour = hour;
    }
    if (g_hist_wh_start[hour] >= 0)                 /* live partial */
        g_hist_kwh[hour] = ((int32_t)wh - g_hist_wh_start[hour]) / 1000.0f;
    g_hist_last_wh = (int32_t)wh;

    HistSnap snap;
    snap.ver = HIST_SNAP_VER;
    memcpy(snap.date, g_hist_date, sizeof(snap.date));
    memcpy(snap.kwh, g_hist_kwh, sizeof(snap.kwh));
    memcpy(snap.wh_start, g_hist_wh_start, sizeof(snap.wh_start));
    snap.last_wh   = g_hist_last_wh;
    snap.last_hour = g_hist_last_hour;
    g_hist_last_ts = (int64_t)now;         /* every accepted sample is fresh */
    snap.last_ts   = g_hist_last_ts;
    g_store->putBytes("snap", &snap, sizeof(snap));
}

void solar_hist_init(SolarHistStore* store, SolarHistLocalTime lt, SolarHistLog log)
{
    g_store = store;
    g_localtime = lt;
    g_log = log;
    hist_load();
}

void solar_hist_apply(uint32_t wh_today, time_t now)
{
    hist_apply(wh_today, now);
}

const char* solar_hist_date(void)
{
    return g_hist_date;
}

const float* solar_hist_kwh(void)
{
    return g_hist_kwh;
}

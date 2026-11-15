/*
 * test_solar_hist.cpp — host unit tests for the extracted hourly-bucket
 * module (src/solar_hist.cpp). Clock is fixed UTC (gmtime_r); storage is an
 * in-memory fake of SolarHistStore. Each test case pins a real failure mode
 * this code has had or nearly had (see dashboard_data.cpp history).
 */
#include <unity.h>

#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <stdint.h>
#include <time.h>

#include "solar_hist.h"

/* ---------------- fixed clock (UTC, gmtime-based) ---------------- */

/* 2025-06-10 00:00:00 UTC */
static const time_t DAY0 = 1749513600;

static time_t T(int day, int hour, int min = 0, int sec = 0)
{
    return DAY0 + (time_t)day * 86400 + hour * 3600 + min * 60 + sec;
}

static struct tm* test_localtime(const time_t* t, struct tm* out)
{
    return gmtime_r(t, out);           /* fixed "UTC": no TZ dependence */
}

/* ---------------- in-memory SolarHistStore fake ---------------- */

class FakeStore : public SolarHistStore {
public:
    std::map<std::string, std::vector<uint8_t> > blob;
    std::map<std::string, int32_t> ints;
    bool fail_next_put = false;        /* one putBytes returns 0 */
    bool fail_all_put = false;         /* every putBytes returns 0 */

    bool begin() override { return true; }

    size_t bytesLen(const char* key) override {
        std::map<std::string, std::vector<uint8_t> >::const_iterator it = blob.find(key);
        if (it == blob.end()) return 0;
        return it->second.size();
    }

    size_t getBytes(const char* key, void* buf, size_t len) override {
        std::map<std::string, std::vector<uint8_t> >::const_iterator it = blob.find(key);
        if (it == blob.end()) return 0;
        size_t n = it->second.size() < len ? it->second.size() : len;
        memcpy(buf, it->second.data(), n);
        return n;
    }

    size_t putBytes(const char* key, const void* buf, size_t len) override {
        if (fail_next_put) { fail_next_put = false; return 0; }
        if (fail_all_put) return 0;
        std::vector<uint8_t> v((const uint8_t*)buf, (const uint8_t*)buf + len);
        blob[key] = v;
        return len;
    }

    size_t getString(const char* key, char* buf, size_t len) override {
        std::map<std::string, std::vector<uint8_t> >::const_iterator it = blob.find(key);
        if (it == blob.end() || len == 0) return 0;
        size_t n = it->second.size() < len - 1 ? it->second.size() : len - 1;
        memcpy(buf, it->second.data(), n);
        buf[n] = '\0';
        return n;
    }

    int32_t getInt(const char* key, int32_t def) override {
        std::map<std::string, int32_t>::const_iterator it = ints.find(key);
        return it == ints.end() ? def : it->second;
    }

    void remove(const char* key) override {
        blob.erase(key);
        ints.erase(key);
    }
};

/* ---------------- log capture ---------------- */

static std::vector<std::string> g_log_lines;

static void test_log(const char* line)
{
    g_log_lines.push_back(line);
}

static bool log_contains(const char* needle)
{
    for (size_t i = 0; i < g_log_lines.size(); i++) {
        if (g_log_lines[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

/* ---------------- snapshot blob helpers ---------------- */

/* Binary layout of the packed snap blobs (pinned here on purpose; the
 * module's static_assert ties the struct side, these offsets tie the tests):
 *   v2: ver 0 | date 1..11 | kwh 12 | wh_start 108 | last_wh 204 |
 *       last_hour 208 | last_ts 212..219  (220 bytes)
 *   v1: same without last_ts (212 bytes) */
static const size_t SNAP_VER_OFF = 0, SNAP_DATE_OFF = 1, SNAP_KWH_OFF = 12,
                    SNAP_WHSTART_OFF = 108, SNAP_LASTWH_OFF = 204,
                    SNAP_LASTHOUR_OFF = 208, SNAP_LASTTS_OFF = 212;
static const size_t SNAP_V1_SIZE = 212, SNAP_V2_SIZE = 220;

static void put_f(std::vector<uint8_t>& b, size_t off, float v)
{
    memcpy(&b[off], &v, sizeof(float));
}

static float get_f(const std::vector<uint8_t>& b, size_t off)
{
    float v;
    memcpy(&v, &b[off], sizeof(float));
    return v;
}

static void put_i32(std::vector<uint8_t>& b, size_t off, int32_t v)
{
    memcpy(&b[off], &v, sizeof(int32_t));
}

static void plant_legacy_five(FakeStore& s, float kwh10)
{
    std::vector<uint8_t> kwh(24 * sizeof(float), 0);
    std::vector<uint8_t> whstart(24 * sizeof(int32_t), 0);
    for (int i = 0; i < 24; i++) {
        put_f(kwh, 4 * i, (i == 10) ? kwh10 : -1.0f);
        put_i32(whstart, 4 * i, (i == 10) ? 200000 : -1);
    }
    s.blob["kwh"] = kwh;
    s.blob["whstart"] = whstart;
    s.blob["date"] = std::vector<uint8_t>((const uint8_t*)"2025-06-10",
                                          (const uint8_t*)"2025-06-10" + 10);
    s.ints["lastwh"] = 200300;
    s.ints["lasthour"] = 10;
}

static std::vector<uint8_t> make_v1(int hour, float kwh, int32_t whstart,
                                    int32_t last_wh, int last_hour)
{
    std::vector<uint8_t> b(SNAP_V1_SIZE, 0);
    b[SNAP_VER_OFF] = 1;
    memcpy(&b[SNAP_DATE_OFF], "2025-06-10", 10);
    for (int i = 0; i < 24; i++) {
        put_f(b, SNAP_KWH_OFF + 4 * i, (i == hour) ? kwh : -1.0f);
        put_i32(b, SNAP_WHSTART_OFF + 4 * i, (i == hour) ? whstart : -1);
    }
    put_i32(b, SNAP_LASTWH_OFF, last_wh);
    put_i32(b, SNAP_LASTHOUR_OFF, last_hour);
    return b;
}

static std::vector<uint8_t> make_v2(int hour, float kwh, int32_t whstart,
                                    int32_t last_wh, int last_hour,
                                    int64_t last_ts)
{
    std::vector<uint8_t> b = make_v1(hour, kwh, whstart, last_wh, last_hour);
    b.resize(SNAP_V2_SIZE, 0);
    b[SNAP_VER_OFF] = 2;
    memcpy(&b[SNAP_LASTTS_OFF], &last_ts, sizeof(int64_t));
    return b;
}

static void init_hist(FakeStore& s)
{
    solar_hist_init(&s, test_localtime, test_log);
}

/* ---------------- Unity plumbing ---------------- */

void setUp(void)    { g_log_lines.clear(); }
void tearDown(void) {}

/* 1. Fresh start: empty store → no date, all 24 buckets missing. */
void test_fresh_start_empty_store(void)
{
    FakeStore s;
    init_hist(s);
    TEST_ASSERT_EQUAL_STRING("", solar_hist_date());
    const float* kwh = solar_hist_kwh();
    for (int i = 0; i < 24; i++)
        TEST_ASSERT_EQUAL_FLOAT(-1.0f, kwh[i]);
    TEST_ASSERT_TRUE(log_contains("[data] hourly history: none stored"));
}

/* 2. v2 round-trip: samples across two hours, re-init restores state. */
void test_v2_roundtrip_reinit_restores(void)
{
    FakeStore a;
    init_hist(a);
    solar_hist_apply(100000, T(0, 13, 30));
    solar_hist_apply(100300, T(0, 13, 59));  /* live partial: hour 13 = 0.30 */
    solar_hist_apply(100700, T(0, 14, 5));   /* carry seeds hour 14: +0.40 */

    FakeStore b;
    b.blob = a.blob;                         /* fake "reboot": store persists */
    b.ints = a.ints;
    init_hist(b);

    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    const float* kwh = solar_hist_kwh();
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.3f, kwh[13]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.4f, kwh[14]);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, kwh[12]);
    TEST_ASSERT_TRUE(log_contains(
        "[data] hourly history loaded: 2025-06-10 (15 buckets set)"));
}

/* 3. v1 upgrade: 212-byte ver-1 blob → bars kept, last_ts=0 (carry gate
 * closed: an apply one hour later must NOT carry), stored snap now 220 B. */
void test_v1_upgrade_bars_kept_carry_gate_closed(void)
{
    FakeStore s;
    s.blob["snap"] = make_v1(13, 0.25f, 100000, 100250, 13);
    init_hist(s);

    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    TEST_ASSERT_EQUAL_FLOAT(0.25f, solar_hist_kwh()[13]);
    TEST_ASSERT_TRUE(log_contains("[data] hourly history: upgraded v1 snapshot"));
    TEST_ASSERT_EQUAL_UINT(SNAP_V2_SIZE, s.blob["snap"].size());
    TEST_ASSERT_EQUAL_INT(2, s.blob["snap"][SNAP_VER_OFF]);
    g_log_lines.clear();

    /* one hour later: gate must be closed (last_ts=0) → no carry */
    solar_hist_apply(100850, T(0, 14));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, solar_hist_kwh()[14]);   /* carry would give 0.60 */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.25f, solar_hist_kwh()[13]);
    TEST_ASSERT_TRUE(log_contains("[data] hourly: closed hour 13 at 0.25 kWh"));
}

/* 4. Five-key legacy migration: bars restored, five keys removed, logged. */
void test_legacy_five_key_migration(void)
{
    FakeStore s;
    plant_legacy_five(s, 0.55f);
    init_hist(s);

    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    TEST_ASSERT_EQUAL_FLOAT(0.55f, solar_hist_kwh()[10]);
    TEST_ASSERT_TRUE(log_contains("[data] hourly history: migrated legacy snapshot"));

    /* all five legacy keys removed */
    TEST_ASSERT_TRUE(s.blob.find("date") == s.blob.end());
    TEST_ASSERT_TRUE(s.blob.find("kwh") == s.blob.end());
    TEST_ASSERT_TRUE(s.blob.find("whstart") == s.blob.end());
    TEST_ASSERT_TRUE(s.ints.find("lastwh") == s.ints.end());
    TEST_ASSERT_TRUE(s.ints.find("lasthour") == s.ints.end());

    /* converted v2 blob stored */
    TEST_ASSERT_EQUAL_UINT(SNAP_V2_SIZE, s.blob["snap"].size());
    TEST_ASSERT_EQUAL_INT(2, s.blob["snap"][SNAP_VER_OFF]);
    TEST_ASSERT_EQUAL_INT(200300,
        (int)*(const int32_t*)(const void*)&s.blob["snap"][SNAP_LASTWH_OFF]);
}

/* 5. Migration with putBytes failure: bars still adopted, legacy keys kept,
 * failure logged (they are the only persisted copy). */
void test_migration_put_failure_keeps_legacy_keys(void)
{
    FakeStore s;
    plant_legacy_five(s, 0.55f);
    s.fail_next_put = true;
    init_hist(s);

    /* adopted into memory regardless */
    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    TEST_ASSERT_EQUAL_FLOAT(0.55f, solar_hist_kwh()[10]);

    /* legacy keys still present */
    TEST_ASSERT_TRUE(s.blob.find("date") != s.blob.end());
    TEST_ASSERT_TRUE(s.blob.find("kwh") != s.blob.end());
    TEST_ASSERT_TRUE(s.blob.find("whstart") != s.blob.end());
    TEST_ASSERT_TRUE(s.ints.find("lastwh") != s.ints.end());
    TEST_ASSERT_TRUE(s.ints.find("lasthour") != s.ints.end());

    TEST_ASSERT_TRUE(log_contains(
        "[data] hourly history: snapshot save FAILED (0/220 B) - legacy keys kept"));
    TEST_ASSERT_FALSE(log_contains("migrated legacy snapshot"));
}

/* 6. v1 upgrade with putBytes failure: v1 blob kept, bars adopted; a later
 * init with the failure cleared re-upgrades idempotently. */
void test_v1_upgrade_put_failure_then_retry(void)
{
    FakeStore s1;
    s1.blob["snap"] = make_v1(13, 0.25f, 100000, 100250, 13);
    s1.fail_next_put = true;
    init_hist(s1);

    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    TEST_ASSERT_EQUAL_FLOAT(0.25f, solar_hist_kwh()[13]);
    TEST_ASSERT_TRUE(log_contains("[data] hourly history: upgraded v1 snapshot"));
    TEST_ASSERT_EQUAL_UINT(SNAP_V1_SIZE, s1.blob["snap"].size());  /* v1 kept */

    FakeStore s2;
    s2.blob = s1.blob;                       /* reboot with the failed store */
    s2.ints = s1.ints;                       /* (fail flag does not persist) */
    init_hist(s2);

    /* idempotent re-upgrade: bars preserved, blob converted for real */
    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    TEST_ASSERT_EQUAL_FLOAT(0.25f, solar_hist_kwh()[13]);
    TEST_ASSERT_EQUAL_UINT(SNAP_V2_SIZE, s2.blob["snap"].size());
    TEST_ASSERT_EQUAL_INT(2, s2.blob["snap"][SNAP_VER_OFF]);
}

/* 7. Carry: samples in hour 13, then 60 s later in hour 14 → hour 13's bar
 * closes with its OWN last sample's delta, hour 14 baseline = the hour-13
 * sample's wh (carry), live partial continues from there. */
void test_hour_boundary_carry_fresh_sample(void)
{
    FakeStore s;
    init_hist(s);
    solar_hist_apply(100000, T(0, 13, 30));  /* first sample: base = wh */
    solar_hist_apply(100300, T(0, 13, 59));  /* live partial: 0.30 */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.3f, solar_hist_kwh()[13]);

    solar_hist_apply(100700, T(0, 14, 0));   /* adjacent + fresh → carry */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.3f, solar_hist_kwh()[13]);  /* closed */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.4f, solar_hist_kwh()[14]);  /* 100700-100300 */

    solar_hist_apply(101000, T(0, 14, 5));   /* live partial on carried base */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.7f, solar_hist_kwh()[14]);
}

/* 8. Gap gate: hours 14-16 stay missing; recovery hour starts from the
 * current wh (no giant bar built from the stale carry). */
void test_gap_gate_no_giant_bar(void)
{
    FakeStore s;
    init_hist(s);
    solar_hist_apply(100000, T(0, 13));
    solar_hist_apply(100500, T(0, 13, 30));  /* live partial: 0.50 */
    solar_hist_apply(104000, T(0, 17));      /* > 3 h gap */

    const float* kwh = solar_hist_kwh();
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, kwh[13]);
    for (int h = 14; h <= 16; h++)
        TEST_ASSERT_EQUAL_FLOAT(-1.0f, kwh[h]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, kwh[17]);  /* baseline = current wh */

    TEST_ASSERT_TRUE(log_contains("[data] hourly: closed hour 13 at 0.50 kWh"));
}

/* 9. Stale/future timestamp: last_ts ahead of `now` → no carry. */
void test_future_last_ts_blocks_carry(void)
{
    FakeStore s;
    /* last_ts 10 min in the future relative to the applied sample */
    s.blob["snap"] = make_v2(13, 0.75f, 100000, 100800, 13, T(0, 14) + 600);
    init_hist(s);
    g_log_lines.clear();

    solar_hist_apply(101500, T(0, 14));
    /* closed hour 13, but hour 14 baseline = current wh (gate blocked) */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.8f, solar_hist_kwh()[13]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, solar_hist_kwh()[14]);   /* carry would give 0.70 */
    TEST_ASSERT_TRUE(log_contains("[data] hourly: closed hour 13 at 0.80 kWh"));
}

/* 10. Midnight rollover: buckets reset, new day logged, hour 0 starts fresh. */
void test_midnight_rollover_reset(void)
{
    FakeStore s;
    init_hist(s);
    solar_hist_apply(50000, T(0, 23));
    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, solar_hist_kwh()[23]);

    solar_hist_apply(50500, T(1, 0));
    TEST_ASSERT_EQUAL_STRING("2025-06-11", solar_hist_date());
    const float* kwh = solar_hist_kwh();
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, kwh[23]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, kwh[0]);   /* fresh, not carried */
    for (int h = 1; h < 24; h++)
        TEST_ASSERT_EQUAL_FLOAT(-1.0f, kwh[h]);
    TEST_ASSERT_TRUE(log_contains("[data] hourly: new day 2025-06-11"));
}

/* 11. Live partial: two samples in the same hour update the same bar. */
void test_live_partial_same_hour(void)
{
    FakeStore s;
    init_hist(s);
    solar_hist_apply(1000, T(0, 9));
    solar_hist_apply(1500, T(0, 9, 20));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, solar_hist_kwh()[9]);
}

/* 12. A usable v2 snap must win: legacy keys are left untouched. */
void test_valid_v2_snap_prevents_migration(void)
{
    FakeStore s;
    s.blob["snap"] = make_v2(5, 1.5f, 100000, 101500, 5, T(0, 5));
    plant_legacy_five(s, 0.55f);             /* five keys ALSO planted */
    init_hist(s);

    TEST_ASSERT_EQUAL_STRING("2025-06-10", solar_hist_date());
    const float* kwh = solar_hist_kwh();
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, kwh[5]);   /* v2 adopted, */
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, kwh[10]);           /* not legacy */

    /* legacy keys untouched */
    TEST_ASSERT_TRUE(s.blob.find("date") != s.blob.end());
    TEST_ASSERT_TRUE(s.blob.find("kwh") != s.blob.end());
    TEST_ASSERT_TRUE(s.blob.find("whstart") != s.blob.end());
    TEST_ASSERT_TRUE(s.ints.find("lastwh") != s.ints.end());
    TEST_ASSERT_TRUE(s.ints.find("lasthour") != s.ints.end());

    TEST_ASSERT_TRUE(log_contains("[data] hourly history loaded"));
    TEST_ASSERT_FALSE(log_contains("migrated legacy snapshot"));
    TEST_ASSERT_FALSE(log_contains("upgraded v1 snapshot"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fresh_start_empty_store);
    RUN_TEST(test_v2_roundtrip_reinit_restores);
    RUN_TEST(test_v1_upgrade_bars_kept_carry_gate_closed);
    RUN_TEST(test_legacy_five_key_migration);
    RUN_TEST(test_migration_put_failure_keeps_legacy_keys);
    RUN_TEST(test_v1_upgrade_put_failure_then_retry);
    RUN_TEST(test_hour_boundary_carry_fresh_sample);
    RUN_TEST(test_gap_gate_no_giant_bar);
    RUN_TEST(test_future_last_ts_blocks_carry);
    RUN_TEST(test_midnight_rollover_reset);
    RUN_TEST(test_live_partial_same_hour);
    RUN_TEST(test_valid_v2_snap_prevents_migration);
    return UNITY_END();
}

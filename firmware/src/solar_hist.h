/*
 * solar_hist.h — on-board hourly kWh buckets, pure C++ (no Arduino).
 *
 * Mechanical extraction of the hist_* block from dashboard_data.cpp so the
 * bucket logic can be unit-tested on the host. Board specifics (NVS storage,
 * local clock, serial logging) are injected through the seams below.
 *
 * Bucket scheme (unchanged from the relay + dashboard_data.cpp):
 *   bucket h = wh_today(end) - wh_today(hour start); the current hour is a
 *   live partial; -1 means no data for that hour.
 */
#ifndef SOLAR_HIST_H
#define SOLAR_HIST_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Storage seam: 1:1 mirror of the NVS ops the module uses. Two adapters
 * will satisfy it (NVS-backed for the board, in-memory fake for tests). */
class SolarHistStore {
public:
    virtual ~SolarHistStore() = default;
    virtual bool    begin() = 0;
    virtual size_t  bytesLen(const char* key) = 0;
    virtual size_t  getBytes(const char* key, void* buf, size_t len) = 0;
    virtual size_t  putBytes(const char* key, const void* buf, size_t len) = 0;
    virtual size_t  getString(const char* key, char* buf, size_t len) = 0;
    virtual int32_t getInt(const char* key, int32_t def) = 0;
    virtual void   remove(const char* key) = 0;
};

typedef void (*SolarHistLog)(const char* line);
typedef struct tm* (*SolarHistLocalTime)(const time_t*, struct tm*);

/* (Re)bind the seams and load/upgrade/migrate persisted state. */
void solar_hist_init(SolarHistStore* store, SolarHistLocalTime lt, SolarHistLog log);

/* Fold one wh_today sample into the buckets + persist (midnight rollover,
 * hour-boundary close, gap-gated carry, live partial). `now` is the epoch
 * of the sample. */
void solar_hist_apply(uint32_t wh_today, time_t now);

/* Current local date "YYYY-MM-DD", "" when no sample has been accepted. */
const char* solar_hist_date(void);

/* 24 floats, index 0 = midnight, -1.0f = hour missing. */
const float* solar_hist_kwh(void);

#endif /* SOLAR_HIST_H */

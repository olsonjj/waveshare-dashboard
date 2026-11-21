/**
 * dashboard_ui.h — two-tab demo dashboard (solar + web stats)
 * Mock data today; swap the refresh timer for real data sources later.
 */
#ifndef DASHBOARD_UI_H
#define DASHBOARD_UI_H

#include <lvgl.h>

/* Creates the two-tab dashboard on the active screen and starts its update timer. */
void dashboard_ui_init(void);

/* Switch tabs programmatically (host simulator / tests). */
void dashboard_ui_select_tab(int idx);

/* Update the "Open PRs" card. Pass note != nullptr to show an error/status
 * instead of a number (e.g. "API off"). Must be called with the LVGL lock held. */
void dashboard_ui_set_pr_count(uint32_t count, const char *note);

/** Update the Solar tab: current power (W), energy produced today (Wh),
 *  today's peak power (W), energy over the trailing 7 days (Wh), and a
 *  wall-clock string for the last-updated pill ("--:--" when data is
 *  unavailable). Buffers only — applied by the 1 Hz coalesce timer so all
 *  per-second changes share ONE invalidated frame (full-refresh RGB makes
 *  every dirty frame a full-screen render; see dashboard_ui.cpp). Display
 *  latency ≤1 s. Must be called with the LVGL adapter lock held. */
void dashboard_ui_set_solar(uint32_t watts_now, uint32_t wh_today,
                             uint32_t watts_peak, uint32_t wh_7day,
                             const char *updated);

/** Update the generation bar chart: 96 bars, one per 15 minutes, kWh
 *  for that quarter hour, index 0 = 12:00 AM (index = hour*4 + min/15).
 *  Negative entries render no bar (future/unknown bucket); pass nullptr
 *  or an all-negative array to clear all bars. Buffers only — applied by
 *  the 1 Hz coalesce timer (≤1 s display latency), in the same frame as
 *  solar/clock updates. Must be called with the LVGL adapter lock held. */
void dashboard_ui_set_chart(const float kwh[96]);

/** Show/hide the header "NETWORK LOST" pill. up=true hides it (push link
 *  established, or the Wi-Fi handshake is still in progress at boot);
 *  up=false shows it (no credentials, refused, dropped, or stale).
 *  Buffers only — applied by the 1 Hz coalesce timer (≤1 s latency), in
 *  the same frame as other updates. Must be called with the LVGL adapter
 *  lock held. */
void dashboard_ui_set_link(bool up);

#endif // DASHBOARD_UI_H

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
 *  and today's peak power (W). Must be called with the LVGL adapter lock
 *  held (same contract as dashboard_ui_set_pr_count). */
void dashboard_ui_set_solar(uint32_t watts_now, uint32_t wh_today, uint32_t watts_peak);

#endif // DASHBOARD_UI_H

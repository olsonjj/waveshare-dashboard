/**
 * dashboard_ui.h — two-tab demo dashboard (solar + web stats)
 * Mock data today; swap the refresh timer for real data sources later.
 */
#ifndef DASHBOARD_UI_H
#define DASHBOARD_UI_H

#include <lvgl.h>

/* Creates the two-tab dashboard on the active screen and starts its update timer. */
void dashboard_ui_init(void);

/* Update the "Open PRs" card. Pass note != nullptr to show an error/status
 * instead of a number (e.g. "API off"). Must be called with the LVGL lock held. */
void dashboard_ui_set_pr_count(uint32_t count, const char *note);

#endif // DASHBOARD_UI_H

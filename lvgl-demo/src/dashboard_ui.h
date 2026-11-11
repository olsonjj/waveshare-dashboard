/**
 * dashboard_ui.h — two-tab demo dashboard (solar + web stats)
 * Mock data today; swap the refresh timer for real data sources later.
 */
#ifndef DASHBOARD_UI_H
#define DASHBOARD_UI_H

#include <lvgl.h>

/* Creates the two-tab dashboard on the active screen and starts its update timer. */
void dashboard_ui_init(void);

#endif // DASHBOARD_UI_H

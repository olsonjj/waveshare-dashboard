/**
 * dashboard_data.h — Wi-Fi + API scaffolding.
 *
 * - Connects to Wi-Fi from secrets.h with a schedule we own: one attempt, then
 *   an exponential-backoff ladder with the radio OFF between attempts (the
 *   panel's RGB refresh path lives on core 0 — see NOTES.md — so the driver's
 *   own 1 s retry loop starves it and shows as screen noise).
 * - Polls the GitHub REST API for open-PR count on GITHUB_REPO once connected.
 * - Pushes results into the UI through dashboard_ui_set_pr_count() (LVGL-locked).
 *
 * Called from main.cpp:  dashboard_data_init() in setup(), dashboard_data_tick()
 * from loop(). The first fetch runs as soon as Wi-Fi is up; then every
 * DATA_POLL_INTERVAL_MS. Compile main.cpp with -DDBG1_NO_WIFI to build without
 * the data layer at all (debug switch for panel-glitch investigations).
 */
#pragma once

#include <stdint.h>

/** Wire up Wi-Fi listeners. Non-blocking. */
void dashboard_data_init(void);

/** Pump the poller state machine. Call from loop() as often as possible. */
void dashboard_data_tick(void);

/** Run one fetch immediately (serial "FETCH" command — diagnostics). */
void dashboard_data_fetch_now(void);

/** Probe TLS connect to host:port (serial "PROBE host[:port]" — diagnostics). */
void dashboard_data_probe(const char *spec);

/** Data polling interval between successful fetches. */
#define DATA_POLL_INTERVAL_MS (60UL * 1000UL)

/** Timeouts */
#define WIFI_CONNECT_TIMEOUT_MS  (10UL * 1000UL)
#define HTTP_TIMEOUT_MS          (8UL * 1000UL)

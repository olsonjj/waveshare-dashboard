/**
 * dashboard_data.h — Wi-Fi + API scaffolding.
 *
 * - Connects to Wi-Fi from secrets.h and auto-reconnects on drop.
 * - Polls the GitHub REST API for open-PR count on GITHUB_REPO.
 * - Pushes results into the UI through dashboard_ui_set_pr_count() (LVGL-locked).
 *
 * Called from main.cpp:  dashboard_data_init() in setup(), dashboard_data_tick()
 * from loop(). The first fetch runs as soon as Wi-Fi is up; then every
 * DATA_POLL_INTERVAL_MS.
 *
 * Extend this file with more pollers (solar/MQTT/...) — keep one state machine
 * per source, all non-blocking between polls.
 */
#pragma once

#include <stdint.h>

/** Wire up Wi-Fi listeners. Non-blocking. */
void dashboard_data_init(void);

/** Pump the poller state machine. Call from loop() as often as possible. */
void dashboard_data_tick(void);

/** Data polling interval between successful fetches. */
#define DATA_POLL_INTERVAL_MS (60UL * 1000UL)

/** Timeouts */
#define WIFI_CONNECT_TIMEOUT_MS  (20UL * 1000UL)
#define HTTP_TIMEOUT_MS          (8UL * 1000UL)

/*
 * main.cpp — the board is a pure renderer: socket parse + draw, nothing else.
 *
 * Deliberately NO diagnostic code paths (SNAPSHOT/LVSNAP frame dumps, serial
 * command handler, perf-monitor overlay, heap telemetry): during the
 * RGB-ghost diagnosis the firmware runs workload-minimal so the panel's
 * scan path has no co-stressors to point at (see docs/GHOSTING_INVESTIGATION.md
 * and docs/NOTES.md, 2026-11-19 session). The dump commands live in git
 * history (revert the strip commit to restore them).
 *
 * Periodic work on this board, in full:
 *   - LVGL tick timer (framework) + render/flush of invalidated regions
 *   - clock label (1 s), chart now-line (1 s)           [dashboard_ui]
 *   - push stream: read+parse one ~900 B JSON line, update labels/bars,
 *     small redraw + flush (1 s)                        [push_client]
 *   - [hb] heartbeat printf every ~5 s (4 KB TX ring — never spin-waits)
 * Data layer can be compiled out entirely with -DDBG1_NO_WIFI (A/B builds).
 */
#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_err.h>

#include <lvgl.h>

#include "esp_lv_adapter_arduino.h"
#include "dashboard_ui.h"
#include "push_client.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

/* ------------------------------------------------------------------ */
/* wall clock: TZ only — the pushed epoch is the sole time source      */
/* ------------------------------------------------------------------ */
/* The board no longer seeds or syncs its own clock (no build stamp, no
 * serial TIME, no SNTP): the push server's `epoch` field drives the RTC
 * via push_client.cpp (nudged when drift > 5 s). The header clock and
 * the chart now-line render localtime() with TZ=PST8PDT (DST aware). */
static const char *TZ_PACIFIC = "PST8PDT,M3.2.0,M11.1.0";

void setup()
{
    /* 4 KB TX ring + 921600 baud: Serial.print() never spin-waits on the
     * 128-byte HW FIFO (see NOTES.md serial hardening rationale) */
    Serial.setTxBufferSize(4096);
    Serial.begin(921600);
    Serial.println("solar dashboard start");

    /* TZ must be set before any localtime()/mktime() use */
    setenv("TZ", TZ_PACIFIC, 1);
    tzset();

    Board *board = new Board();
    Serial.printf("[s1] board created\n");
    if ((board == nullptr) || !board->init()) {
        Serial.println("Board init failed");
        while (true) {
            delay(1000);
        }
    }
    Serial.printf("[s2] board init done\n");

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    LCD *lcd = board->getLCD();
    if (lcd == nullptr) {
        Serial.println("LCD device is not available");
        while (true) {
            delay(1000);
        }
    }
    auto *lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        lcd->configFrameBufferNumber(frame_buffer_count);
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }

    assert(board->begin());
    Serial.printf("[s3] board begin done (LCD+bus configured)\n");

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.task_priority = 2;
    adapter_config.task_core_id = ARDUINO_RUNNING_CORE;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        lcd, lcd->getFrameWidth(), lcd->getFrameHeight(), rotation
    );
    disp_config.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != nullptr);

    if (board->getTouch() != nullptr) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, board->getTouch());
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != nullptr);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    Serial.printf("[s4] adapter started\n");

#ifndef DBG1_NO_WIFI
    push_client_init();
#else
    Serial.println("[dbg1] data layer disabled (glitch experiment)\n");
#endif

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    dashboard_ui_init();
    esp_lv_adapter_unlock();

    Serial.println("solar dashboard ready");
    Serial.flush();
}

void loop()
{
    static uint32_t n = 0;
    /* LVGL runs in the adapter's own task; don't touch it from here */
#ifndef DBG1_NO_WIFI
    push_client_tick();
#endif
    if ((++n % 50) == 0) {
        Serial.printf("[hb] alive t=%lu ms\n", (unsigned long)millis());
        Serial.flush();
    }
    delay(100);
}

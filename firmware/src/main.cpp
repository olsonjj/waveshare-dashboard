#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_err.h>

#include <sys/time.h>
#include <time.h>

#include <lvgl.h>

#include "esp_lv_adapter_arduino.h"
#include "dashboard_ui.h"
#include "dashboard_data.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

/* ------------------------------------------------------------------ */
/* wall clock: TZ + build-time seed + serial "TIME" sync command       */
/* ------------------------------------------------------------------ */
/* No battery-backed RTC and (currently) no NTP, so:
 *   1. system time is seeded from the build timestamp on every boot
 *      (accurate to ~a minute after flash / power cycle)
 *   2. the serial port accepts "TIME hh:mm:ss" or
 *      "TIME yyyy-mm-dd hh:mm:ss" to set exact local time
 * The header clock reads localtime() with TZ=PST8PDT (DST aware;
 * UTC-8 in winter, UTC-7 in summer). Drift: soft RTC keeps running
 * while powered (resets survive); hard power cycle = build seed.
 */
static const char *TZ_PACIFIC = "PST8PDT,M3.2.0,M11.1.0";

static bool parse_time_spec(const char *s, struct tm *out)
{
    /* full "yyyy-mm-dd hh:mm:ss" */
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6) {
        memset(out, 0, sizeof(*out));
        out->tm_year = y - 1900; out->tm_mon = mo - 1; out->tm_mday = d;
        out->tm_hour = h; out->tm_min = mi; out->tm_sec = se;
        out->tm_isdst = -1;
        return true;
    }
    /* "hh:mm:ss" -> keep today's date from the running clock */
    if (sscanf(s, "%d:%d:%d", &h, &mi, &se) == 3 && h >= 0 && h < 24) {
        time_t now = time(nullptr);
        localtime_r(&now, out);
        out->tm_hour = h; out->tm_min = mi; out->tm_sec = se;
        out->tm_isdst = -1;
        return true;
    }
    return false;
}

static void apply_time(struct tm *tmbuf, const char *src)
{
    time_t t = mktime(tmbuf);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", localtime_r(&t, tmbuf));
    Serial.printf("[time] set from %s: %s\n", src, buf);
}

static void seed_time_from_build(void)
{
    /* __DATE__ = "Mmm dd yyyy" | __TIME__ = "hh:mm:ss"
     * (gcc stamps local time of the build host = Pacific) */
    const char *months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                             "Jul","Aug","Sep","Oct","Nov","Dec" };
    char mon[4] = {0};
    int day, year, hour, minute, sec;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &sec) != 3) {
        Serial.println("[time] build stamp parse failed");
        return;
    }
    int mon_idx = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) { mon_idx = i; break; }
    }
    if (mon_idx < 0) {
        Serial.println("[time] build stamp month unknown");
        return;
    }
    struct tm tmbuf;
    memset(&tmbuf, 0, sizeof(tmbuf));
    tmbuf.tm_year = year - 1900; tmbuf.tm_mon = mon_idx; tmbuf.tm_mday = day;
    tmbuf.tm_hour = hour; tmbuf.tm_min = minute; tmbuf.tm_sec = sec;
    tmbuf.tm_isdst = -1;                 /* let TZ rules decide PST vs PDT */

    /* Seed ONLY if the running RTC is behind the build stamp. Reboots (soft
     * reset) keep the RTC ticking, so re-seeding unconditionally would
     * rewind the clock to build time on every reboot — days off by the time
     * anyone notices. With the relay path the epoch field corrected this
     * within seconds; in direct mode the RTC is all we have. */
    time_t build_epoch = mktime(&tmbuf);
    if (time(nullptr) < build_epoch - 5) {
        char stamp[80];
        snprintf(stamp, sizeof(stamp), "%s %s", __DATE__, __TIME__);
        apply_time(&tmbuf, stamp);
    } else {
        Serial.println("[time] RTC ahead of build stamp - kept");
    }
}

static void handle_serial_line(const String &line)
{
    String s = line;
    s.trim();
    if (s == "TIME ?") {
        time_t now = time(nullptr);
        struct tm tmbuf;
        localtime_r(&now, &tmbuf);
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z (%z)", &tmbuf);
        Serial.printf("[time] now: %s\n", buf);
        return;
    }
    if (s.startsWith("TIME ")) {
        struct tm tmbuf;
        if (parse_time_spec(s.c_str() + 5, &tmbuf)) {
            apply_time(&tmbuf, "serial");
        } else {
            Serial.println("[time] syntax: TIME hh:mm:ss | TIME yyyy-mm-dd hh:mm:ss | TIME ?");
        }
    }
    if (s == "FETCH") {
        dashboard_data_fetch_now();
    }
    if (s.startsWith("PROBE ")) {
        dashboard_data_probe(s.c_str() + 6);
    }
}

void setup()
{
    /* 4 KB TX ring + 921600 baud: Serial.print() never spin-waits on the
     128-byte HW FIFO (see NOTES.md serial hardening rationale) */
    Serial.setTxBufferSize(4096);
    Serial.begin(921600);
    Serial.println("solar dashboard start");

    /* TZ must be set before any localtime()/mktime() use */
    setenv("TZ", TZ_PACIFIC, 1);
    tzset();
    seed_time_from_build();

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
    dashboard_data_init();
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
    static String cmdbuf;
    /* LVGL runs in the adapter's own task; don't touch it from here */
#ifndef DBG1_NO_WIFI
    dashboard_data_tick();
#endif
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (cmdbuf.length()) {
                handle_serial_line(cmdbuf);
                cmdbuf = "";
            }
        } else if (cmdbuf.length() < 64) {
            cmdbuf += c;
        }
    }
    if ((++n % 50) == 0) {
        Serial.printf("[hb] alive t=%lu ms\n", (unsigned long)millis());
        Serial.flush();
    }
    delay(100);
}

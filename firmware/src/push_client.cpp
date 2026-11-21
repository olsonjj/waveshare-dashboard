/**
 * push_client.cpp — Wi-Fi + TCP socket client for the push architecture.
 *
 * Data flow:
 *   WiFi station (radio-quiet profile ported from the retired data layer)
 *     └─ tick() state machine:  WIFI_CONNECTING → WIFI_BACKOFF → …
 *          └─ once up: connect to PUSH_SERVER_HOST:PUSH_SERVER_PORT
 *               └─ read newline-delimited JSON lines (one per second)
 *                    └─ ArduinoJson → epoch (clock) + ScreenState fields
 *                       → dashboard_ui_set_solar()/_set_chart() (bounded lock)
 *
 * Radio budget: a ~1 s Wi-Fi connect at boot plus millisecond socket
 * receives — the quiet state already proven clean (GHOSTING_INVESTIGATION
 * §8, push-demo experiment). No TLS, no polling, no long radio pulls.
 */
#include "push_client.h"
#include "secrets.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>

#include "esp_lv_adapter_arduino.h"   // esp_lv_adapter_lock/unlock for UI updates
#include "dashboard_ui.h"

/* ---------- constants ---------- */

/* Wi-Fi connect timeout; failed attempts back off with the radio OFF
 * (same rationale and ladder as the retired data layer: the RGB refresh
 * path shares core 0 with the radio, so scan/handshake bursts must not
 * hammer it). */
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 10UL * 1000UL;
static const uint32_t backoff_s[] = {15, 45, 120, 300, 900, 1800};

/* Socket reconnect cadence (plain LAN TCP connect: milliseconds of radio). */
static const uint32_t SOCK_RETRY_MS = 10UL * 1000UL;

/* Staleness watchdog: the contract guarantees one line per second, so 15 s
 * of silence means the server died without a FIN/RST (or the path dropped). */
static const uint32_t STREAM_STALE_MS = 15UL * 1000UL;

/* Bounded LVGL lock for data->UI calls: a wedged LVGL task must never
 * freeze the data layer — skip/retry the UI update instead. */
static const TickType_t UI_LOCK_TIMEOUT = pdMS_TO_TICKS(5000);

/* Push contract (server/src/contract.rs): one JSON line per second.
 * 96 float buckets + scalar fields comfortably fit; oversized lines are
 * dropped and logged rather than corrupting the next one. */
#define N_BUCKETS 96
static const size_t LINE_BUF_LEN = 2048;

/* ---------- state ---------- */

enum PushState {
    WIFI_DISABLED,     /* no credentials in secrets.h */
    WIFI_CONNECTING,   /* radio up, awaiting IP */
    WIFI_BACKOFF,      /* radio OFF, waiting to retry */
    SOCK_TRYING,       /* Wi-Fi up, socket down, retrying every 10 s */
    LINKED,            /* socket open, reading the 1 s push stream */
};

static PushState state = WIFI_CONNECTING;
static bool boot_grace = true;   /* only the FIRST Wi-Fi handshake counts as
                                  * "in progress, don't alarm" (boot); mid-run
                                  * handshakes after a drop are a real outage */
static uint32_t state_since_ms = 0;
static uint32_t next_action_ms = 0;   /* backoff expiry / socket retry */
static uint32_t wifi_fail_count = 0;
static NetworkClient sock;
static char line_buf[LINE_BUF_LEN];
static size_t line_len = 0;
static uint32_t last_line_ms = 0;   /* last complete line (staleness watchdog) */

static void enter_state(PushState s)
{
    state = s;
    state_since_ms = millis();
}

static uint32_t backoff_delay(void)
{
    uint32_t i = wifi_fail_count > 0 ? wifi_fail_count - 1 : 0;
    if (i >= sizeof(backoff_s) / sizeof(backoff_s[0]))
        i = sizeof(backoff_s) / sizeof(backoff_s[0]) - 1;
    return backoff_s[i] * 1000UL;
}

static void radio_off(void)
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

static void radio_up_and_connect(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          /* beacon-wake power save perturbed the panel */
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    enter_state(WIFI_CONNECTING);
}

/* ---------- push line handling ---------- */

/* The push payload is ~900 B; parsing it on the internal heap adds churn
 * to a pool that runs at ~45 KB free, so the JSON document allocates from
 * PSRAM instead (~5.8 MB free — see GHOSTING_INVESTIGATION §8 telemetry).
 * The document is static and cleared per line, so the pool is reused
 * instead of alloc/free every second. */
class PsramAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t n) override
    {
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override
    {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

static void process_line(char *line)
{
    static PsramAllocator alloc;
    static JsonDocument doc(&alloc);
    doc.clear();
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        Serial.printf("[push] JSON parse failed: %s\n", err.c_str());
        return;
    }
    if ((doc["v"] | 0u) != 1u) {
        Serial.println("[push] unknown contract version - line dropped");
        return;
    }

    /* clock: the pushed epoch is the ONLY time source (no build-stamp
     * seed, no SNTP — both are retired). Only nudge when drift > 5 s so
     * the RTC isn't stomped mid-render every second. */
    time_t epoch = doc["epoch"] | (time_t)0;
    if (epoch > 0) {
        if ((time_t)llabs((long long)(time(nullptr) - epoch)) > 5) {
            struct timeval tv = { epoch, 0 };
            settimeofday(&tv, nullptr);
        }
    }

    /* screen state: the line is COMPLETE — nothing is merged or carried
     * across lines, so a dropped line costs one second of freshness. */
    uint32_t watts_now = doc["watts_now"] | 0u;
    uint32_t wh_today = doc["wh_today"] | 0u;
    uint32_t peak = doc["watts_peak_today"] | 0u;
    uint32_t wh_7day = doc["wh_7day"] | 0u;

    float kwh[N_BUCKETS];
    for (int i = 0; i < N_BUCKETS; i++) kwh[i] = -1.0f;   /* -1 = no bar */
    JsonArrayConst arr = doc["kwh_15m"];
    if (!arr.isNull()) {
        int n = arr.size() < N_BUCKETS ? (int)arr.size() : N_BUCKETS;
        for (int i = 0; i < n; i++) {
            kwh[i] = arr[i] | (-1.0f);
        }
    }

    /* "last updated" pill: when this line was pushed (local wall time) */
    char upd[16] = "--:--";
    if (epoch > 0) {
        struct tm tmv;
        localtime_r(&epoch, &tmv);
        strftime(upd, sizeof(upd), "%I:%M %p", &tmv);
    }

    /* bounded lock: a wedged LVGL task must never freeze the data layer —
     * skip the UI update this second instead of waiting forever */
    if (esp_lv_adapter_lock(UI_LOCK_TIMEOUT) == ESP_OK) {
        dashboard_ui_set_solar(watts_now, wh_today, peak, wh_7day, upd);
        dashboard_ui_set_chart(kwh);
        esp_lv_adapter_unlock();
    } else {
        Serial.println("[push] ui lock TIMEOUT - update skipped");
    }
}

/* feed one byte; a complete line is processed immediately */
static void feed_byte(uint8_t c)
{
    if (c == '\n') {
        if (line_len > 0 && line_buf[line_len - 1] == '\r') line_len--;
        line_buf[line_len] = '\0';
        if (line_len > 0) process_line(line_buf);
        last_line_ms = millis();   /* any complete line proves the stream is alive */
        line_len = 0;
        return;
    }
    if (line_len >= LINE_BUF_LEN - 1) {          /* oversized: drop the line */
        if (line_len == LINE_BUF_LEN - 1)
            Serial.printf("[push] line over %u B dropped\n",
                          (unsigned)(LINE_BUF_LEN - 1));
        line_len++;                              /* keep eating until '\n' */
        return;
    }
    line_buf[line_len++] = (char)c;
}

/* ---------- link-state reporting (header NETWORK LOST pill) ---------- */

/* Mirror link transitions to the UI, edge-triggered: one bounded-lock UI
 * call per transition, nothing per-second.
 *
 * Retry contract: link_reported is committed only AFTER the UI update
 * succeeds. If the 5 s lock times out the edge is not consumed, so the
 * next ~100 ms tick retries — a "lost" transition can never be dropped.
 *
 * WIFI_CONNECTING is reported as up only while boot_grace is set (the
 * first Wi-Fi handshake, i.e. at boot): a normal boot passes through it
 * in ~1 s before SOCK_TRYING, so the pill doesn't flash at boot; if the
 * handshake fails, WIFI_BACKOFF reports lost and the pill appears. After
 * the grace flag clears, a mid-run re-handshake (Wi-Fi dropped while
 * streaming) is an outage and reports lost immediately. Any other
 * non-LINKED state (socket refused, stream dropped/stale, no credentials)
 * reports lost. */
static bool link_reported = true;

static void report_link(bool up)
{
    if (up == link_reported) return;
    if (esp_lv_adapter_lock(UI_LOCK_TIMEOUT) != ESP_OK) {
        Serial.println("[push] ui lock TIMEOUT - link pill update retried next tick");
        return;   /* edge NOT consumed — retry on the next tick */
    }
    dashboard_ui_set_link(up);
    esp_lv_adapter_unlock();
    link_reported = up;
}

/* ---------- public API ---------- */

void push_client_init(void)
{
    if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASS) == 0) {
        Serial.println("[push] no WiFi credentials in secrets.h — push client disabled");
        enter_state(WIFI_DISABLED);
        return;
    }

    /* mute chatty radio-driver logs (our own [push] prints unaffected) */
    esp_log_level_set("wifi", ESP_LOG_NONE);
    esp_log_level_set("wifi_init", ESP_LOG_NONE);
    esp_log_level_set("phy", ESP_LOG_NONE);

    WiFi.mode(WIFI_STA);               /* station only */
    WiFi.setAutoReconnect(false);      /* driver-level retry starves the panel
                                        * path; we schedule reconnects instead */
    WiFi.setSleep(false);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.printf("[push] WiFi connected, IP: %s\n",
                      WiFi.localIP().toString().c_str());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);

    Serial.printf("[push] connecting to \"%s\", push server %s:%u...\n",
                  WIFI_SSID, PUSH_SERVER_HOST, (unsigned)PUSH_SERVER_PORT);
    radio_up_and_connect();
}

void push_client_tick(void)
{
    switch (state) {
    case WIFI_DISABLED:
        break;

    case WIFI_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
            wifi_fail_count = 0;
            boot_grace = false;   /* first handshake completed — later ones alarm */
            enter_state(SOCK_TRYING);
            next_action_ms = millis();          /* first connect attempt now */
        } else if (millis() - state_since_ms > WIFI_CONNECT_TIMEOUT_MS) {
            wifi_fail_count++;
            uint32_t wait = backoff_delay();
            Serial.printf("[push] WiFi connect attempt %lu failed — retry in %lu s\n",
                          (unsigned long)wifi_fail_count, (unsigned long)(wait / 1000));
            radio_off();
            next_action_ms = millis() + wait;
            state = WIFI_BACKOFF;
            state_since_ms = millis();
        }
        break;

    case WIFI_BACKOFF:
        if ((int32_t)(millis() - next_action_ms) >= 0) {
            Serial.println("[push] retrying WiFi...");
            radio_up_and_connect();
        }
        break;

    case SOCK_TRYING:
        if (WiFi.status() != WL_CONNECTED) {    /* link dropped */
            sock.stop();
            radio_up_and_connect();             /* CONNECTING's timeout applies backoff */
            break;
        }
        if ((int32_t)(millis() - next_action_ms) >= 0) {
            Serial.println("[push] connecting to push server...");
            if (sock.connect(PUSH_SERVER_HOST, PUSH_SERVER_PORT)) {
                Serial.println("[push] linked — reading 1 s push stream");
                line_len = 0;
                last_line_ms = millis();   /* grace period starts now */
                enter_state(LINKED);
            } else {
                next_action_ms = millis() + SOCK_RETRY_MS;
            }
        }
        break;

    case LINKED:
        if (WiFi.status() != WL_CONNECTED || !sock.connected()) {
            Serial.println("[push] stream lost — retrying");
            sock.stop();
            enter_state(SOCK_TRYING);
            next_action_ms = millis() + SOCK_RETRY_MS;
            break;
        }
        /* half-open detection: the contract guarantees a line every second,
         * so silence means the server died without a FIN/RST (or the path
         * dropped while WiFi stays associated) — connected() stays true and
         * available() returns 0 forever otherwise. */
        if (millis() - last_line_ms > STREAM_STALE_MS) {
            Serial.println("[push] stream stale (no line in 15 s) — reconnecting");
            sock.stop();
            enter_state(SOCK_TRYING);
            next_action_ms = millis();   /* reconnect immediately */
            break;
        }
        /* millisecond-scale: drain whatever has arrived since last tick */
        while (sock.available() > 0) {
            int c = sock.read();
            if (c < 0) break;
            feed_byte((uint8_t)c);
        }
        break;
    }

    /* every exit path: reflect link health on the header pill (LINKED is
     * up; the first boot handshake is tolerated; everything else alarms) */
    report_link(state == LINKED || (state == WIFI_CONNECTING && boot_grace));
}

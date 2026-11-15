/**
 * dashboard_data.cpp — Wi-Fi + HTTPS + JSON scaffolding.
 *
 * Data flow:
 *   WiFi station (auto-reconnect)
 *     └─ tick() state machine:  WIFI_CONNECTING → IDLE → FETCHING → IDLE → ...
 *          └─ fetch_open_prs():  NetworkClientSecure (CA bundle) + HTTPClient
 *                                  └─ ArduinoJson  → doc["total_count"]
 *                                     └─ dashboard_ui_set_pr_count()  (adapter lock)
 */
#include "dashboard_data.h"
#include "secrets.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "esp_lv_adapter_arduino.h"   // esp_lv_adapter_lock/unlock for UI updates
#include "dashboard_ui.h"

/* ---------- state ---------- */
enum DataState { WIFI_DISABLED, WIFI_CONNECTING, WIFI_BACKOFF, IDLE, FETCHING };

static DataState state = WIFI_CONNECTING;
static uint32_t state_since_ms = 0;
static uint32_t last_poll_ms = 0;
static uint32_t wifi_fail_count = 0;
static uint32_t next_connect_ms = 0;

static void enter_state(DataState s);

/*
 * Backoff after failed WiFi attempts. The panel's RGB refresh path lives on
 * core 0 (see NOTES.md): scan/handshake bursts starve it and show up as screen
 * noise, so failed attempts must sit with the radio OFF and NOT hammer the
 * WiFi driver. Cap at 30 minutes; a connected period resets the ladder.
 */
static const uint32_t backoff_s[] = {15, 45, 120, 300, 900, 1800};
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
    WiFi.setSleep(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    enter_state(WIFI_CONNECTING);
}

static void enter_state(DataState s)
{
    state = s;
    state_since_ms = millis();
}

static void schedule_next_poll(void)
{
    last_poll_ms = millis();
}

/* ---------- fetchers ---------- */
static int fetch_open_prs(uint32_t *out_count)
{
    NetworkClientSecure tls;
    tls.useBuiltinCACertBundle();          // validated TLS, no certs to babysit

    HTTPClient http;
    char url[192];
    snprintf(url, sizeof(url),
             "https://api.github.com/search/issues?q=repo:%s+type:pr+state:open&per_page=1",
             GITHUB_REPO);

    if (!http.begin(tls, url)) {
        return -1;
    }

    http.setUserAgent("waveshare-dashboard/0.1");
    http.addHeader("Accept", "application/vnd.github+json");
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (strlen(GITHUB_TOKEN) > 0) {
        http.addHeader("Authorization", "Bearer " GITHUB_TOKEN);
        http.addHeader("X-GitHub-Api-Version", "2022-11-28");
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[data] GitHub HTTP error %d (%s)\n", code, http.errorToString(code).c_str());
        http.end();
        return code < 0 ? -1 : code;
    }

    /* GitHub search responses can be tens of KB; filter to what we need */
    String payload = http.getString();
    http.end();

    JsonDocument filter;
    filter["total_count"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[data] JSON parse failed: %s\n", err.c_str());
        return -2;
    }

    uint32_t n = doc["total_count"] | 0u;
    *out_count = n;
    return 0;
}

/* ---------- UI push (LVGL runs on the adapter task — always lock) ---------- */
static void push_pr_count(void)
{
    uint32_t n = 0;
    int rc = fetch_open_prs(&n);
    if (rc == 0) {
        Serial.printf("[data] open PRs: %lu\n", (unsigned long)n);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            dashboard_ui_set_pr_count(n, nullptr);
            esp_lv_adapter_unlock();
        }
    } else {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            dashboard_ui_set_pr_count(0, "API off");
            esp_lv_adapter_unlock();
        }
        schedule_next_poll();              // retry on the normal interval
    }
}

/* ---------- public API ---------- */
void dashboard_data_init(void)
{
    if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASS) == 0) {
        Serial.println("[data] no WiFi credentials in secrets.h — data layer disabled");
        enter_state(WIFI_DISABLED);
        return;
    }

    WiFi.mode(WIFI_STA);                   // station only; this board has no AP role
    WiFi.setAutoReconnect(false);          // driver-level retry starves the panel path;
                                           // we schedule reconnects with backoff instead
    WiFi.setSleep(true);                   // modem sleep between transactions

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.printf("[data] WiFi connected, IP: %s\n",
                      WiFi.localIP().toString().c_str());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);

    printf("[data] connecting to WiFi network \"%s\"...\n", WIFI_SSID);
    radio_up_and_connect();
}

void dashboard_data_tick(void)
{
    if (state == WIFI_DISABLED) return;

    switch (state) {
    case WIFI_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[data] WiFi ready\n");
            wifi_fail_count = 0;
            push_pr_count();               // first fetch immediately
            enter_state(IDLE);
        } else if (millis() - state_since_ms > WIFI_CONNECT_TIMEOUT_MS) {
            wifi_fail_count++;
            uint32_t wait = backoff_delay();
            Serial.printf("[data] WiFi connect attempt %lu failed — retry in %lu s\n",
                          (unsigned long)wifi_fail_count, (unsigned long)(wait / 1000));
            radio_off();                   // panel path regains core 0 while we wait
            next_connect_ms = millis() + wait;
            state = WIFI_BACKOFF;
            state_since_ms = millis();
        }
        break;

    case WIFI_BACKOFF:
        if ((int32_t)(millis() - next_connect_ms) >= 0) {
            printf("[data] retrying WiFi...\n");
            radio_up_and_connect();
        }
        break;

    case IDLE:
        if (WiFi.status() != WL_CONNECTED) {
            // dropped link: fresh attempt; if it stays down, CONNECTING's
            // timeout applies backoff
            WiFi.disconnect();
            radio_up_and_connect();
            break;
        }
        if (millis() - last_poll_ms > DATA_POLL_INTERVAL_MS) {
            push_pr_count();
            schedule_next_poll();
        }
        break;

    case FETCHING:
        /* fetches are synchronous in this scaffold; state not entered yet.
           Convert to async (esp_http_client events) if response handling
           must not block loop(). */
    default:
        break;
    }
}

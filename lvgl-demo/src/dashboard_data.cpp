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
enum DataState { WIFI_CONNECTING, IDLE, FETCHING };

static DataState state = WIFI_CONNECTING;
static uint32_t state_since_ms = 0;
static uint32_t last_poll_ms = 0;
static uint32_t wifi_fail_count = 0;

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
    WiFi.mode(WIFI_STA);                   // station only; this board has no AP role
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true);                   // modem sleep between transactions

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.printf("[data] WiFi connected, IP: %s\n",
                      WiFi.localIP().toString().c_str());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.printf("[data] WiFi disconnected (status %d), auto-reconnecting\n",
                      info.wifi_sta_disconnected.reason);
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    printf("[data] connecting to WiFi network \"%s\"...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    enter_state(WIFI_CONNECTING);
}

void dashboard_data_tick(void)
{
    switch (state) {
    case WIFI_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[data] WiFi ready\n");
            wifi_fail_count = 0;
            push_pr_count();               // first fetch immediately
            enter_state(IDLE);
        } else if (millis() - state_since_ms > WIFI_CONNECT_TIMEOUT_MS) {
            wifi_fail_count++;
            Serial.printf("[data] WiFi connect attempt %lu failed (%s), retrying in 10 s\n",
                          (unsigned long)wifi_fail_count, WiFi.status() == WL_NO_SSID_AVAIL ?
                          "SSID not found" : "timeout");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            enter_state(WIFI_CONNECTING);  // restart the timer as well
            delay(10 * 1000);              // grace period; blocks loop(), UI unaffected
        }
        break;

    case IDLE:
        if (millis() - last_poll_ms > DATA_POLL_INTERVAL_MS) {
            if (WiFi.status() == WL_CONNECTED) {
                push_pr_count();
                schedule_next_poll();
            }
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

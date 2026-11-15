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
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_log.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>

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
    WiFi.setSleep(false);          // E4: modem power-save off (beacon wakeups perturbed panel)
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

/* LAN relay (relay/solar_relay.py on the Mac): tiny JSON over plain HTTP.
 * The direct Envoy TLS path works but takes ~29 s per phase from the board
 * (radio airtime the panel can't afford); the relay fetches the Envoy on
 * its fast path and serves us a sub-second LAN GET. */
#if SOLAR_POLL_VIA_RELAY
static int fetch_solar_relay(uint32_t *watts_now, uint32_t *wh_today,
                              uint32_t *watts_peak, float hourly_kwh[24])
{
    uint32_t t0 = millis();
    HTTPClient http;
    if (!http.begin(RELAY_URL)) {
        return -1;
    }
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.addHeader("X-Relay-Key", RELAY_KEY);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[data] relay HTTP error %d (%s) dt=%lu ms\n",
                      code, http.errorToString(code).c_str(),
                      (unsigned long)(millis() - t0));
        http.end();
        return -1;
    }
    String payload = http.getString();
    http.end();

    JsonDocument doc;                    // payload is ~90 bytes; no filter needed
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[data] relay JSON parse failed: %s\n", err.c_str());
        return -2;
    }
    *watts_now = doc["watts_now"] | 0u;
    *wh_today   = doc["wh_today"] | 0u;
    *watts_peak = doc["watts_peak_today"] | 0u;

    /* hourly_kwh: array of 24 floats (index 0 = midnight), kWh per hour.
     * Absent key / short array / non-numeric entries are left at the
     * caller's "missing" marker (-1) so those hours render no bar. */
    JsonArrayConst hourly = doc["hourly_kwh"];
    if (!hourly.isNull()) {
        for (int i = 0; i < 24; i++) {
            hourly_kwh[i] = hourly[i] | (-1.0f);
        }
    }

    /* wall clock: the relay's host clock is NTP-synced; SNTP on the board
       wedges the loop ~24-40 s after boot (see NOTES.md 2026-11-15), so sync
       the soft RTC from here instead. Only nudge when drift > 5 s. */
    time_t epoch = doc["epoch"] | (time_t)0;
    if (epoch != 0) {
        if ((time_t)llabs((long long)(time(nullptr) - epoch)) > 5) {
            struct timeval tv = { epoch, 0 };
            settimeofday(&tv, nullptr);
            Serial.println("[data] clock synced from relay");
        }
    }
    Serial.printf("[data] relay: %lu W, %lu Wh today (dt=%lu ms, age %0.1f s)\n",
                  (unsigned long)*watts_now, (unsigned long)*wh_today,
                  (unsigned long)(millis() - t0), (double)(doc["age_s"] | 0.0));
    return 0;
}
#endif /* SOLAR_POLL_VIA_RELAY */

/* Enphase IQ Gateway direct fetch — the scheduled poll path in direct mode.
 * Enphase IQ Gateway (local LAN): GET /ivp/pdm/energy -> production.pcu,
 * then /api/v1/production/inverters on the same TLS session for the peak
 * (sum of per-micro maxReportWatts).
 * Gateway serves TLS with a SELF-SIGNED cert (verified device on our own
 * LAN) so verification is intentionally disabled here. */
/* kept across polls: the Envoy handshake is slow (~21 s) but keep-alive
 * works, so reuse the session and only re-handshake when it drops */
static NetworkClientSecure s_envoy_tls;

static int fetch_enphase(uint32_t *watts_now, uint32_t *wh_today,
                         uint32_t *watts_peak)
{
    uint32_t t0 = millis();
    if (!s_envoy_tls.connected()) {
        s_envoy_tls.setInsecure();                 // self-signed gateway cert
        s_envoy_tls.setHandshakeTimeout(40);       // slow-but-working handshake (see NOTES.md)
        s_envoy_tls.setTimeout(15000);
        if (!s_envoy_tls.connect(ENVOY_HOST, 443)) {
            char tls_msg[128] = {0};
            s_envoy_tls.lastError(tls_msg, sizeof(tls_msg));
            Serial.printf("[data] envoy connect FAIL dt=%lu ms err=[%s]\n",
                          (unsigned long)(millis() - t0), tls_msg);
            s_envoy_tls.stop();                   // clean slate for next attempt
            return -1;
        }
        Serial.printf("[data] envoy tls handshake dt=%lu ms\n",
                      (unsigned long)(millis() - t0));
    } else {
        Serial.println("[data] envoy reusing connection");
    }

    HTTPClient http;
    char url[96];
    snprintf(url, sizeof(url), "https://%s/ivp/pdm/energy", ENVOY_HOST);
    if (!http.begin(s_envoy_tls, url)) {
        s_envoy_tls.stop();
        return -1;
    }
    http.setReuse(true);                  // keep the TLS session alive across polls
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.addHeader("Authorization", "Bearer " ENVOY_TOKEN);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    uint32_t t_hdr = millis();
    if (code != 200) {
        Serial.printf("[data] envoy HTTP error %d (%s) dt=%lu ms\n",
                      code, http.errorToString(code).c_str(),
                      (unsigned long)(t_hdr - t0));
        http.end();
        s_envoy_tls.stop();                // don't reuse a dead session
        return -1;
    }
    String payload = http.getString();
    http.end();
    uint32_t t_body = millis();

    JsonDocument filter;
    filter["production"]["pcu"]["wattsNow"] = true;
    filter["production"]["pcu"]["wattHoursToday"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload,
        DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[data] envoy JSON parse failed: %s\n", err.c_str());
        return -2;
    }
    *watts_now = doc["production"]["pcu"]["wattsNow"] | 0u;
    *wh_today   = doc["production"]["pcu"]["wattHoursToday"] | 0u;
    uint32_t t_peak = millis();

    /* second endpoint on the same TLS session: per-micro daily maxes ->
     * today's system peak (they don't peak simultaneously, but it's the
     * closest the local API offers). Failure here only costs the peak. */
    snprintf(url, sizeof(url), "https://%s/api/v1/production/inverters", ENVOY_HOST);
    if (!http.begin(s_envoy_tls, url)) {
        Serial.println("[data] inverters endpoint begin FAIL");
    } else {
        http.addHeader("Authorization", "Bearer " ENVOY_TOKEN);
        http.addHeader("Accept", "application/json");
        int code2 = http.GET();
        if (code2 != 200) {
            Serial.printf("[data] inverters HTTP error %d - peak stays 0\n", code2);
            http.end();
            s_envoy_tls.stop();            /* session may be dead; rebuild next poll */
        } else {
            String inv = http.getString();
            http.end();
            JsonDocument idoc;
            DeserializationError ierr = deserializeJson(idoc, inv);
            if (ierr) {
                Serial.printf("[data] inverters JSON parse failed: %s\n", ierr.c_str());
            } else {
                for (JsonVariantConst v : idoc.as<JsonArray>()) {
                    *watts_peak += v["maxReportWatts"] | 0u;
                }
            }
        }
    }

    Serial.printf("[data] envoy: %lu W, %lu Wh today, %lu W peak (get %lu ms, body %lu ms, inverters %lu ms, %u B)\n",
                  (unsigned long)*watts_now, (unsigned long)*wh_today,
                  (unsigned long)*watts_peak,
                  (unsigned long)(t_hdr - t0), (unsigned long)(t_body - t_hdr),
                  (unsigned long)(millis() - t_peak), (unsigned)payload.length());
    return 0;
}

#if !SOLAR_POLL_VIA_RELAY
/* ----- on-board hourly buckets (direct mode) ------------------------------
 * Same scheme the relay built: bucket h = wh_today(end) - wh_today(hour
 * start); the current hour is a live partial; -1 = no data this hour.
 * The 5-min poll cadence is plenty of resolution for hour buckets. State
 * lives in NVS so reboots mid-day don't lose the chart. NOTE: bucket
 * indices trust the board RTC (build-stamp seeded; see main.cpp). */
#include <Preferences.h>

static Preferences s_hist;
static char     s_hist_date[11] = "";    /* local "YYYY-MM-DD"          */
static float    s_hist_kwh[24];
static int32_t  s_hist_wh_start[24];
static int32_t  s_hist_last_wh = -1;
static int      s_hist_last_hour = -1;

static void hist_load(void)
{
    s_hist.begin("solarhist", false);
    String saved = s_hist.getString("date", "");
    size_t n = s_hist.getBytes("kwh", s_hist_kwh, sizeof(s_hist_kwh));
    s_hist.getBytes("whstart", s_hist_wh_start, sizeof(s_hist_wh_start));
    s_hist_last_wh   = s_hist.getInt("lastwh", -1);
    s_hist_last_hour = s_hist.getInt("lasthour", -1);
    if (n == sizeof(s_hist_kwh) && saved.length() == 10) {
        strncpy(s_hist_date, saved.c_str(), sizeof(s_hist_date) - 1);
        Serial.printf("[data] hourly history loaded: %s (%d buckets set)\n",
                      s_hist_date,
                      s_hist_last_hour >= 0 ? s_hist_last_hour + 1 : 0);
    } else {
        Serial.println("[data] hourly history: none stored");
    }
}

/* fold one wh_today sample into the buckets + persist (port of the
 * relay's _apply_sample: hour-boundary close, midnight reset, live partial) */
static void hist_apply(uint32_t wh)
{
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char today[11];
    strftime(today, sizeof(today), "%Y-%m-%d", &tmv);

    if (strcmp(today, s_hist_date) != 0) {           /* midnight rollover */
        strcpy(s_hist_date, today);
        for (int i = 0; i < 24; i++) { s_hist_kwh[i] = -1.0f; s_hist_wh_start[i] = -1; }
        s_hist_last_wh = -1;
        s_hist_last_hour = -1;
        Serial.printf("[data] hourly: new day %s\n", today);
    }
    int hour = tmv.tm_hour;
    if (hour != s_hist_last_hour) {                  /* close previous hour */
        if (s_hist_last_hour >= 0 && s_hist_wh_start[s_hist_last_hour] >= 0
            && s_hist_last_wh >= 0) {
            s_hist_kwh[s_hist_last_hour] =
                (s_hist_last_wh - s_hist_wh_start[s_hist_last_hour]) / 1000.0f;
            Serial.printf("[data] hourly: closed hour %d at %.2f kWh\n",
                          s_hist_last_hour,
                          (double)s_hist_kwh[s_hist_last_hour]);
        }
        s_hist_wh_start[hour] = (int32_t)wh;
        s_hist_last_hour = hour;
    }
    if (s_hist_wh_start[hour] >= 0)                 /* live partial */
        s_hist_kwh[hour] = ((int32_t)wh - s_hist_wh_start[hour]) / 1000.0f;
    s_hist_last_wh = (int32_t)wh;

    s_hist.putString("date", s_hist_date);
    s_hist.putBytes("kwh", s_hist_kwh, sizeof(s_hist_kwh));
    s_hist.putBytes("whstart", s_hist_wh_start, sizeof(s_hist_wh_start));
    s_hist.putInt("lastwh", s_hist_last_wh);
    s_hist.putInt("lasthour", s_hist_last_hour);
}
#endif /* !SOLAR_POLL_VIA_RELAY */

static void push_solar(void)
{
    uint32_t watts_now = 0, wh_today = 0, watts_peak = 0;
    float hourly[24];
    for (int i = 0; i < 24; i++) hourly[i] = -1.0f;   /* -1 = hour missing */
#if SOLAR_POLL_VIA_RELAY
    int rc = fetch_solar_relay(&watts_now, &wh_today, &watts_peak, hourly);
#else
    int rc = fetch_enphase(&watts_now, &wh_today, &watts_peak);
    if (rc == 0) {
        hist_apply(wh_today);
        memcpy(hourly, s_hist_kwh, sizeof(hourly));
    }
#endif
    uint32_t tw = millis();
    /* bounded lock: a wedged LVGL task must never freeze the data layer —
       skip the UI update this cycle instead of waiting forever */
    if (esp_lv_adapter_lock(pdMS_TO_TICKS(5000)) == ESP_OK) {
        if (rc == 0) {
            /* The wall clock is local (RTC seeded per main.cpp policy; the
             * relay path would have applied its epoch already). */
            char upd[16] = "--:--";
            time_t t = time(nullptr);
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(upd, sizeof(upd), "%I:%M %p", &tmv);
            dashboard_ui_set_solar(watts_now, wh_today, watts_peak, upd);
            dashboard_ui_set_hourly(hourly);
        } else {
            dashboard_ui_set_solar(0, 0, 0, "--:--");   // zeros: stale data can't masquerade
        }
        esp_lv_adapter_unlock();
        if (millis() - tw > 100) {
            Serial.printf("[data] ui lock slow: %lu ms\n", (unsigned long)(millis() - tw));
        }
    } else {
        Serial.printf("[data] ui lock TIMEOUT after %lu ms - UI update skipped\n",
                      (unsigned long)(millis() - tw));
    }
    schedule_next_poll();
}

static int fetch_open_prs(uint32_t *out_count)
{
    NetworkClientSecure tls;
    tls.useBuiltinCACertBundle();          // validated TLS, no certs to babysit
    tls.setHandshakeTimeout(20);           // cap TLS handshake — an 8 s HTTP timeout doesn't cover it

    HTTPClient http;
    char url[192];
    snprintf(url, sizeof(url),
             "https://api.github.com/search/issues?q=repo:%s+type:pr+state:open&per_page=1",
             GITHUB_REPO);

    /* diagnostics: DNS step separately, so a failure can't hide here */
    uint32_t t_dns = millis();
    IPAddress ip;
    if (!WiFi.hostByName("api.github.com", ip)) {
        Serial.printf("[data] DNS FAIL api.github.com (t=%lu ms)\n", (unsigned long)(millis() - t_dns));
        return -3;
    }
    Serial.printf("[data] dns api.github.com -> %s (%lu ms)\n",
                  ip.toString().c_str(), (unsigned long)(millis() - t_dns));

    uint32_t t_heap = ESP.getFreeHeap();

    /* connect TLS ourselves (explicit diagnostics) — HTTPClient reuses an
       already-connected socket. Also caps failure time far below the ~55 s
       hang HTTPClient's timeout does not cover. */
    uint32_t t_tls = millis();
    if (!tls.connect("api.github.com", 443)) {
        char tls_msg[128] = {0};
        tls.lastError(tls_msg, sizeof(tls_msg));
        Serial.printf("[data] tls connect FAIL dt=%lu ms heap=%u err=[%s]\n",
                      (unsigned long)(millis() - t_tls), (unsigned)ESP.getFreeHeap(), tls_msg);
        return -1;
    }
    Serial.printf("[data] tls connect OK dt=%lu ms heap %u->%u\n",
                  (unsigned long)(millis() - t_tls),
                  (unsigned)t_heap, (unsigned)ESP.getFreeHeap());

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

    uint32_t t_get = millis();
    int code = http.GET();
    uint32_t dt = millis() - t_get;
    if (code != 200) {
        char tls_msg[128] = {0};
        tls.lastError(tls_msg, sizeof(tls_msg));
        Serial.printf("[data] GitHub HTTP error %d (%s) dt=%lu ms heap %u->%u tls=[%s]\n",
                      code, http.errorToString(code).c_str(), (unsigned long)dt,
                      (unsigned)t_heap, (unsigned)ESP.getFreeHeap(), tls_msg);
        http.end();
        return code < 0 ? -1 : code;
    }

    /* GitHub search responses can be tens of KB; filter to what we need */
    String payload = http.getString();
    http.end();
    Serial.printf("[data] GitHub 200, %u B, %lu ms, heap %u->%u\n",
                  (unsigned)payload.length(), (unsigned long)dt,
                  (unsigned)t_heap, (unsigned)ESP.getFreeHeap());

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

/* ---------- diagnostics: generic TLS probe ---------- */
void dashboard_data_probe(const char *spec)
{
    Serial.printf("[probe] rssi=%d dBm\n", WiFi.RSSI());
    if (strcmp(spec, "RSSI") == 0) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *host = buf;
    uint16_t port = 443;
    bool raw = false;
    bool big = false;
    if (strncmp(host, "RAW:", 4) == 0) { raw = true; host += 4; }
    if (strncmp(host, "BIG:", 4) == 0) { big = true; host += 4; }
    char *colon = strchr(host, ':');
    if (colon) { *colon = '\0'; port = (uint16_t)atoi(colon + 1); }

    uint32_t t0 = millis();
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) {
        Serial.printf("[probe] DNS FAIL %s\n", host);
        return;
    }
    Serial.printf("[probe] %s -> %s (dns %lu ms)\n", host, ip.toString().c_str(),
                  (unsigned long)(millis() - t0));

    /* BIG mode: connect to host:port, send a ~1.2 KB request, watch for a
       response. Tests whether LARGER outbound frames make it (TLS ClientHellos
       are ~200-400 B; everything that worked so far was < 50 B). */
    if (big) {
        NetworkClient tcp;
        t0 = millis();
        bool ok = tcp.connect(host, port);
        Serial.printf("[probe-big] tcp connect %s dt=%lu ms\n", ok ? "OK" : "FAIL",
                      (unsigned long)(millis() - t0));
        if (ok) {
            String req = String("GET / HTTP/1.1\r\nHost: ") + host +
                         "\r\nUser-Agent: ";
            while (req.length() < 1100) req += 'x';
            req += "\r\nAccept: */*\r\n\r\n";
            t0 = millis();
            size_t sent = tcp.write((const uint8_t *)req.c_str(), req.length());
            Serial.printf("[probe-big] sent %u/%u B in %lu ms\n",
                          (unsigned)sent, (unsigned)req.length(), (unsigned long)(millis() - t0));
            for (int i = 0; i < 12; i++) {
                delay(2000);
                Serial.printf("[probe-big] t+%ds: connected=%d available=%d\n",
                              (i + 1) * 2, (int)tcp.connected(), (int)tcp.available());
                if (tcp.available() > 0) {
                    int n = tcp.read((uint8_t *)buf, 64);
                    Serial.printf("[probe-big] first %d bytes: ", n);
                    for (int k = 0; k < n && k < 32; k++) {
                        if (buf[k] >= 32 && buf[k] < 127) Serial.write(buf[k]); else Serial.write('.');
                    }
                    Serial.println();
                    break;
                }
                if (!tcp.connected()) { Serial.println("[probe-big] killed before any response"); break; }
            }
            tcp.stop();
        }
        return;
    }

    /* RAW mode: plain TCP to an arbitrary port; send a valid minimal TLS
       record prefix (ClientHello start) and report the peer's reaction.
       Discriminates: 443-connect-blocked vs TLS-payload-killed. */
    if (raw) {
        NetworkClient tcp;
        t0 = millis();
        bool ok = tcp.connect(host, port);
        Serial.printf("[probe-raw] tcp connect %s dt=%lu ms\n", ok ? "OK" : "FAIL",
                      (unsigned long)(millis() - t0));
        if (ok) {
            const uint8_t chello_prefix[] = {
                0x16, 0x03, 0x01, 0x00, 0x2c,   // TLS record: handshake, vers 3.1, len 44
                0x01, 0x00, 0x00, 0x28,          // ClientHello, len 40
                0x03, 0x03,                      // client_version 3.3
                0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
                0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,       // random (31 bytes ok)
                0x00                                             // session id len 0
            };
            tcp.write(chello_prefix, sizeof(chello_prefix));
            /* poll connection state over time: a real FIN/RST flips
               connected() false; benign silence keeps it open (read()
               alone is non-blocking and cannot distinguish these) */
            for (int i = 0; i < 12; i++) {
                delay(5000);
                Serial.printf("[probe-raw] t+%ds: connected=%d available=%d\n",
                              (i + 1) * 5, (int)tcp.connected(), (int)tcp.available());
            }
            int n = tcp.available();
            if (n > 0) {
                n = tcp.read((uint8_t *)buf, 64);
                Serial.printf("[probe-raw] peer sent %d bytes: ", n);
                for (int i = 0; i < n && i < 8; i++) Serial.printf("%02x ", (uint8_t)buf[i]);
                Serial.println();
            }
            tcp.stop();
        }
        return;
    }

    if (port == 80) {
        NetworkClient tcp;
        t0 = millis();
        bool ok = tcp.connect(host, port);
        Serial.printf("[probe-tcp] connect %s dt=%lu ms\n", ok ? "OK" : "FAIL",
                      (unsigned long)(millis() - t0));
        if (ok) {
            tcp.print(String("HEAD / HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n");
            t0 = millis();
            String resp = tcp.readStringUntil('\n');
            Serial.printf("[probe-tcp] first line: %s (dt=%lu ms)\n", resp.c_str(),
                          (unsigned long)(millis() - t0));
            tcp.stop();
        }
        return;
    }

    NetworkClientSecure tls;
    tls.useBuiltinCACertBundle();
    tls.setHandshakeTimeout(20);           // don't let a bad handshake drag 55 s
    tls.setTimeout(20000);
    t0 = millis();
    bool ok = tls.connect(host, port);
    Serial.printf("[probe] tls %s dt=%lu ms err=[%s]\n", ok ? "OK" : "FAIL",
                  (unsigned long)(millis() - t0),
                  [&]() { static char e[128]; e[0] = 0; tls.lastError(e, sizeof(e)); return e; }());
    if (ok) {
        // sanity: send a bare HTTP HEAD so we know the session is truly usable
        String req = String("HEAD / HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n";
        tls.print(req);
        t0 = millis();
        String resp = tls.readStringUntil('\n');
        Serial.printf("[probe] first line: %s (dt=%lu ms)\n", resp.c_str(),
                      (unsigned long)(millis() - t0));
    }
    tls.stop();
}

/* ---------- public API ---------- */
void dashboard_data_fetch_now(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[fetch] not connected");
        return;
    }
    Serial.printf("[fetch] now, heap=%u\n", (unsigned)ESP.getFreeHeap());
    uint32_t t0 = millis();
    push_solar();
    Serial.printf("[fetch] done total_ms=%lu\n", (unsigned long)(millis() - t0));
}

void dashboard_data_init(void)
{
    if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASS) == 0) {
        Serial.println("[data] no WiFi credentials in secrets.h — data layer disabled");
        enter_state(WIFI_DISABLED);
        return;
    }

    /* Mute chatty radio-driver logs (our own [data] prints are unaffected;
       these tags only gate ESP_LOGx from the WiFi/PHY stack) */
    esp_log_level_set("wifi", ESP_LOG_NONE);
    esp_log_level_set("wifi_init", ESP_LOG_NONE);
    esp_log_level_set("phy", ESP_LOG_NONE);

    WiFi.mode(WIFI_STA);                   // station only; this board has no AP role
    WiFi.setAutoReconnect(false);          // driver-level retry starves the panel path;
                                           // we schedule reconnects with backoff instead
    WiFi.setSleep(false);                  // E4: modem power-save off (see radio_up_and_connect)

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.printf("[data] WiFi connected, IP: %s\n",
                      WiFi.localIP().toString().c_str());
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);

    /* SNTP CONFIRMED as the once-per-boot loop wedge: its first DNS+query
       burst after connect froze loop() 24-40 s (every build with SNTP;
       clean in every build without — 2026-11-15 session, NOTES.md).
       The wall clock now runs from the build-stamp-seeded RTC (direct mode)
       or the relay's epoch field (relay mode); serial TIME stays as the
       manual fallback. */
#if !SOLAR_POLL_VIA_RELAY
    hist_load();
#endif

    printf("[data] connecting to WiFi network \"%s\"...\n", WIFI_SSID);
    radio_up_and_connect();
}

void dashboard_data_tick(void)
{
    if (state == WIFI_DISABLED) return;

    switch (state) {
    case WIFI_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[data] WiFi ready t=%lu\n", (unsigned long)millis());
            wifi_fail_count = 0;
            push_solar();               // first fetch immediately
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
        {
        uint32_t t_st = millis();
        wl_status_t st = WiFi.status();
        if (millis() - t_st > 100) {
            Serial.printf("[data] WiFi.status() SLOW: %lu ms\n",
                          (unsigned long)(millis() - t_st));
        }
        if (st != WL_CONNECTED) {
            // dropped link: fresh attempt; if it stays down, CONNECTING's
            // timeout applies backoff
            WiFi.disconnect();
            radio_up_and_connect();
            break;
        }
        if (millis() - last_poll_ms > DATA_POLL_INTERVAL_MS) {
            Serial.printf("[data] poll start t=%lu\n", (unsigned long)millis());
            push_solar();
            Serial.printf("[data] poll end t=%lu\n", (unsigned long)millis());
        }
        static uint32_t last_idle_log = 0;
        if (millis() - last_idle_log > 10000) {
            last_idle_log = millis();
            Serial.printf("[data] idle connected t=%lu\n", (unsigned long)millis());
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

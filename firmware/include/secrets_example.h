/**
 * secrets_example.h — copy this file to secrets.h and fill in your values.
 * secrets.h is gitignored; never commit real credentials.
 *
 *   cp include/secrets_example.h include/secrets.h
 *
 * The board is a pure renderer: it needs only Wi-Fi credentials and the
 * address of the Rust push server (server/ — see README.md). Envoy
 * credentials now live ONLY in the server's config.toml (gitignored).
 */
#pragma once

// Wi-Fi (2.4 GHz networks only — the ESP32-S3 doesn't do 5 GHz)
#define WIFI_SSID   "your-network-name"
#define WIFI_PASS   "your-network-password"

// Rust push server (server/): the board connects and reads one complete
// screen-state JSON line per second. Host = the always-on Mac's LAN IP.
#define PUSH_SERVER_HOST ""   // e.g. "192.168.182.147"
#define PUSH_SERVER_PORT 8421

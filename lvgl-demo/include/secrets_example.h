/**
 * secrets_example.h — copy this file to secrets.h and fill in your values.
 * secrets.h is gitignored; never commit real credentials.
 *
 *   cp include/secrets_example.h include/secrets.h
 */
#pragma once

// Wi-Fi (2.4 GHz networks only — the ESP32-S3 doesn't do 5 GHz)
#define WIFI_SSID   "your-network-name"
#define WIFI_PASS   "your-network-password"

// GitHub — repo whose data shows in the "Web Stats" tab.
// Token is optional: without one you get 60 requests/hour (fine at a 60 s poll),
// with a fine-grained classic PAT (repo: read) you get 5,000/hour.
// Create one: GitHub → Settings → Developer settings → Personal access tokens.
#define GITHUB_REPO  "olsonjj/waveshare-dashboard"
#define GITHUB_TOKEN ""   // e.g. "github_pat_xxxxxxxxxxxx"

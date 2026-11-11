# Waveshare ESP32-S3-Touch-LCD-7 dashboard

Two-tab HMI dashboard (solar + web stats) for the Waveshare ESP32-S3-Touch-LCD-7.
Built with PlatformIO / Arduino-esp32 / LVGL 8.4 on macOS.

See [NOTES.md](NOTES.md) for the full bring-up record: flashing procedures, board gotchas
(CAN-muxed USB port, UART slide switch, 16 MB flash override), commands, and performance baselines.

## Project layout

```
lvgl-demo/              PlatformIO project (this is what you build/flash)
├── platformio.ini      board config: 16 MB flash, OPI PSRAM, console → UART0
├── src/                application code (main.cpp, dashboard_ui.cpp, ...)
├── include/            headers (lv_conf.h copy)
├── lib/                vendored libraries (LVGL 8.4, ESP32_Display_Panel, ...)
└── .pio/               build output (gitignored)

demos/                  Waveshare official examples clone — reference only, not part of the repo
NOTES.md                bring-up record + gotchas
```

## Quick start

1. `brew install platformio`
2. Open `lvgl-demo/platformio.ini` and confirm ports (see NOTES.md)
3. Flash: `cd lvgl-demo && pio run -t upload` (UART Type-C port, slide switch at UART1)
4. Console: see `pio run -t monitor` or the capture snippet in NOTES.md

## Status

- [x] Bring-up (screen, touch, flashing) — 2024-11
- [x] Two-tab LVGL dashboard with mock data
- [ ] Wi-Fi + real data (GitHub API, solar feed)
- [ ] SquareLine-generated UI integration

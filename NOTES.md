# ESP32-S3-Touch-LCD-7 — Bring-up Notes (macOS)

Board: Waveshare ESP32-S3-Touch-LCD-7 (SKU 27078) — ESP32-S3 N16R8 (16 MB flash / 8 MB OPI PSRAM),
7" 800×480 RGB IPS (ST7262), GT911 capacitive touch, CH422G I/O expander, CH343 USB-UART bridge.
Date: 2025-11-10

## Working project

- **`~/Code/waveshare/lvgl-demo`** — PlatformIO (Arduino framework) project running the LVGL v8
  widgets demo with touch + FPS overlay. Status: ✔ boots, ✔ screen, ✔ touch, ✔ measured ~FPS on overlay.
- **`~/Code/waveshare/demos`** — clone of https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7
  (official examples + prebuilt libraries, referenced but not modified).

## Toolchain (as installed on this Mac)

- PlatformIO Core 6.2.0 via Homebrew (`brew install platformio`), VS Code extension `platformio.platformio-ide`
- Platform `espressif32 @ 8.0.0` (arduino-esp32 3.3.x) — note: Waveshare docs mention Arduino core 3.0.7 in the
  IDE guide, but the shipped `ESP32_Display_Panel v1.0.0` library requires arduino-esp32 **≥ 3.1.0**
- esptool v5.4.0 (PlatformIO-managed)
- `7zz` (brew install 7zip) — used to extract `LVGL_v8.4.0.7z` from the Waveshare example package
- Libraries are vendored in `lvgl-demo/lib/` (copied from the Waveshare repo, so builds are reproducible):

  | Library | Version | Source |
  |---|---|---|
  | ESP32_Display_Panel | 1.0.0 | Waveshare repo `examples/Arduino/libraries` |
  | ESP32_IO_Expander | 1.0.1 | '' |
  | esp-lib-utils | 0.1.2 | '' |
  | lvgl | 8.4.0 | `LVGL_v8.4.0.7z` from the same folder |

## Physical board gotchas (each one cost us an hour!)

1. **Two Type-C ports, very different personalities**
   - `USB` (native ESP32 USB): **disabled by default!** Its data lines are *multiplexed with CAN* and
     selected by CH422G expander pin **EXIO5** (default pulled high = CAN mode → the computer sees no
     USB device at all). Only a running firmware can switch it to USB mode. Do not rely on it for flashing.
   - `UART` (CH343 bridge): **this is the flashing/logging port.** Enumerates on macOS as
     `/dev/cu.usbmodem5B901265491` (WCH VID 0x1A86, "USB Single Serial", CDC mode).
     **It also powers the board** (verified). Single-cable development works: Mac → `UART` port only.
2. **UART1/UART2 slide switch** (silkscreen `UART1  UART2`, bottom edge near the BOOT button —
   actually a black slider, not a labelled DIP block): must be at **UART1** or the CH343 enumerates but
   carries *zero* data. This caused all our "No serial data received" failures. **Leave it at UART1 forever.**
3. **Boot/download mode**: hold **BOOT** → tap **RESET** (or power off/on) → release BOOT.
   Screen goes dark = in download mode. Only needed if auto-reset misbehaves; after the switch fix the
   auto-download via RTS/DTR works and `pio run -t upload` connects by itself.
4. Red herring: `/dev/cu.usbmodem302NTJJ2K2122` is the **LG monitor's** CDC interface, not the board.

## Software/project gotchas

1. **`board_upload.flash_size = 16MB` is the correct override** in PlatformIO espressif32 8.0.0.
   (`board_build.flash_size` is silently ignored → images stamped 8 MB → bootloader rejects the 16 MB
   partition table → boot crash loop: `partition 3 invalid ... exceeds flash chip size 0x800000`.)
   Sanity check the image header: `xxd -l 4 bootloader.bin` → byte 3 `0x4f` = 16 MB @ 80 MHz.
2. `board_build.arduino.memory_type = qio_opi` + `board_build.psram_type = opi` (for the R8 PSRAM).
3. **`lv_conf.h` is found ONLY if it lies at `lib/lv_conf.h`** (LVGL's default relative include from
   `lib/lvgl/src` = two levels up) — `-DLV_CONF_INCLUDE_SIMPLE` + project `include/` did NOT work for
   lib-scope sources in this setup. Both copies exist (`lib/lv_conf.h` + `include/lv_conf.h`), keep them in sync.
4. Two Waveshare conf headers must be present for ESP32_Display_Panel:
   - `esp_panel_board_custom_conf.h` (board: Waveshare:ESP32-S3-Touch-LCD-7, ST7262 RGB 21 MHz, GT911, CH422G)
     → **detected only from `lib/esp_panel_board_custom_conf.h`** (the `__has_include` fallback resolves to
     the `lib/` directory). Symptom if missing: `No default board configuration detected`.
   - Both files currently in `lvgl-demo/src/` AND `lvgl-demo/lib/` (src copies are inert but harmless).
   - PCLK set to **21 MHz** (from 16 MHz) for smoother UI (Waveshare's own ESP-IDF perf-note value).
5. **Console/log routing**: with `ARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` all Serial output goes
   to the native USB port — which is CAN-muxed off → invisible. Use `-DARDUINO_USB_CDC_ON_BOOT=0`
   (current setting) so `Serial` = UART0 = the CH343 port we can read. `CORE_DEBUG_LEVEL=3` (info) is on.
6. LVGL `lv_timer_handler()` is called **only inside the adapter task** (esp_lv_adapter) — do not call it
   from Arduino `loop()` (thread-safety). The `loop()` here only prints a 2-second heartbeat.
7. LVGL perf overlay: `LV_USE_PERF_MONITOR 1` in `lv_conf.h` (bottom-right FPS/CPU readout).

## Command cheat-sheet

```bash
cd ~/Code/waveshare/lvgl-demo
pio run                                            # build
pio run -t upload --upload-port /dev/cu.usbmodem5B901265491   # flash (auto-reset works now)
# if esptool can't connect: hold BOOT, tap RESET, keep holding during the upload attempt

# console (one-shot capture; heartbeat prints every ~2 s)
timeout 20 ~/.platformio/penv/bin/python - <<'EOF'
import serial, time
s = serial.Serial('/dev/cu.usbmodem5B901265491', 115200, timeout=2)
t0=time.time(); buf=b''
while time.time()-t0 < 18: buf += s.read(4096)
print(buf.decode('utf-8','replace'))
EOF

# emergency full-flash at 115200 (when partial states glitch)
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodem5B901265491 --baud 115200 \
  --before default_reset --after hard_reset write_flash \
  0x0000 .pio/build/waveshare-esp32-s3-lcd7/bootloader.bin \
  0x8000 .pio/build/waveshare-esp32-s3-lcd7/partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/waveshare-esp32-s3-lcd7/firmware.bin
```

Note: `pio`'s esptool wrapper sometimes needs the pyserial of the PlatformIO env (`~/.platformio/penv/bin/python`).

## Performance baseline (measured)

- Widgets demo @ PCLK 21 MHz, default adapter task (prio 2, core 1): **~33 FPS, LVGL “CPU” ~99%**
  (CPU% = LVGL busy-time, not system-wide — expected with a demo that animates everything).
- Widgets demo @ priority 4/core 0: regressed to ~3 FPS with tearing (core-0 contention, see above).
  Known-good LVGL task config: priority 2, core = ARDUINO_RUNNING_CORE(1).
- **Custom dashboard (dashboard_ui.cpp): 4–5 FPS, low CPU — this is healthy.** LVGL redraws only on change;
  the 2 s mock-data timer drives a few dirty-region updates/s. FPS spikes during touch/animation.
  Don't chase FPS on static content — judge by *interaction responsiveness* instead.
- Reference: Waveshare ESP-IDF v5.3 LVGL benchmark on this board: 26 FPS avg, 41 FPS interface @ 21 MHz.

## Project state (v1)

- `lvgl-demo/src/main.cpp` — instrumented bring-up (stage markers [s1..s4] + 2 s heartbeat in loop())
- `lvgl-demo/src/dashboard_ui.cpp/.h` — **two-tab dashboard, dark theme**:
  Solar tab (arc gauge + 24 h chart + battery + energy labels; 2 s mock random-walk timer) and
  Web Stats tab (3 status cards + weekly commits bar chart). Fonts enabled: 12/14/16/26/30 + 20/24/32/48.
- Build ~8–30 s, flash ~40 s over single UART cable, no button presses (auto-reset).
- Next: real data sources (Wi-Fi → GitHub REST / MQTT / Home Assistant), then polish + deploy.

## Debug log decode guide (from the CH343 console)

```
LVGL v8 demo start / [s1..s4] stage markers      — our instrumented setup() in src/main.cpp
[E][Panel] No default board configuration …      — esp_panel_board_custom_conf.h not detected (see gotcha 4)
[E][flash_parts] partition … exceeds …0x800000   — image stamped with wrong flash size (see gotcha 1)
rst:0x3 (RTC_SW_SYS_RST) loop + boot errors      — crash loop at bootloader stage
[hb] alive t=…                                   — application loop healthy
[E][esp_lcd_touch_gt911] Unable to initialize…   — benign warning (recovers; touchpad ID read OK)
```

## Ideas / next steps

- [ ] Verify FPS counter after PCLK 21 MHz bump (was 16 MHz); tune `adapter_config.task_priority` (2) if LVGL CPU%
      reads high; consider `-DCONFIG_SPIRAM` fetch/XIP options (Arduino 3.3 sdkconfig may already XIP).
- [ ] **Do NOT** move the LVGL adapter task to core 0 or raise its priority: tested priority 4/core 0 →
      screen refresh starves (tearing/jumping, 3 FPS) because the RGB bounce-buffer/refresh path lives on core 0.
      Known-good: priority 2, core = ARDUINO_RUNNING_CORE(1) → ~33 FPS at the widgets demo.
- [ ] Replace `lv_demo_widgets()` with first custom UI; keep `LV_USE_PERF_MONITOR` while iterating visuals.
- [ ] Decide whether native-USB CDC logs are worth it (would require EXIO5 handling + second cable for power).
- [ ] Serial monitor: VS Code command `PlatformIO: Monitor` or `pio device monitor --port /dev/cu.usbmodem5B901265491 -f esp32_exception_decoder`.

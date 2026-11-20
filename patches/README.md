# ESP-IDF driver patches

Source patch for the ESP-IDF `esp_lcd` RGB driver that fixes the
persistent screen ghost (espressif/esp-idf#19070).

## The bug

The board's RGB panel is driven by ESP-IDF's `esp_lcd` component, which
arduino-esp32 (and PlatformIO) ship **precompiled** as `libesp_lcd.a`.
That prebuilt driver (ESP-IDF v5.5.5) has a bounce-buffer desync bug: in
bounce-buffer mode with `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`, a DMA restart
relaunches from bounce buffer 0 but the software index (`bb_eof_count % 2`)
is never re-synchronized — one lost EOF corrupts the picture permanently.

The upstream fix resets `bb_eof_count` inside the common critical section
of `lcd_rgb_panel_try_restart_transmission()` so the software index
re-syncs to buffer 0 on every VSYNC restart.

## The patch

`esp_lcd_19070.patch` applies to ESP-IDF **v5.5.5** (the exact version
bundled by arduino-esp32 3.3.12):

    patch -p1 < patches/esp_lcd_19070.patch        # from the IDF root

## Rebuild + install (what we did)

The prebuilt `libesp_lcd.a` is rebuilt from patched source in Docker
(no host IDF install), then swapped into the PlatformIO package.

1. **Build in Docker** — image `espressif/idf:v5.5.5`, patch
   `/opt/esp/idf`, then build a minimal `esp32s3` project that pulls in
   `esp_lcd` (`REQUIRES esp_lcd`). Its `sdkconfig.defaults` mirrors the
   Arduino SDK: `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`,
   `CONFIG_LCD_RGB_ISR_IRAM_SAFE=n`, `CONFIG_LCD_ENABLE_DEBUG_LOG=n`.

2. **Swap the result:**

       LIB=~/.platformio/packages/framework-arduinoespressif32-esp32s3-libs/lib
       cp -p "$LIB/libesp_lcd.a" "$LIB/libesp_lcd.a.buggy_$(date +%Y%m%d_%H%M%S)"   # backup
       cp -p <build>/esp-idf/esp_lcd/libesp_lcd.a "$LIB/libesp_lcd.a"

3. **Rebuild + flash** the firmware (`just fw-flash`).

The swap target is the top-level `lib/` in the `-esp32s3-libs` package:
`libesp_lcd.a` is shared across all memory variants (only the
flash/PSRAM-sensitive libs differ per `qio_opi`/`opi_opi`/…).

## Sunset

The fix is upstream on ESP-IDF `release/v5.5` and `master`, but not yet in
a tagged release (v5.5.6 is still buggy). Once a tagged ESP-IDF release
and an arduino-esp32 core (3.3.13+) ship it, delete this patch and the
manual `.a` swap.

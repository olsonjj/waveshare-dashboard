# Migrate to Pure ESP-IDF

**Status:** Draft plan — not started
**Date:** 2026-11-20
**Scope:** Remove the Arduino core + PlatformIO dependencies and rebuild the Waveshare ESP32-S3-Touch-LCD-7 firmware directly on the Espressif ESP-IDF SDK.

---

## 1. Goal

Replace the current PlatformIO + Arduino-core toolchain with a pure ESP-IDF project built with `idf.py`, keeping the existing feature set (LVGL dashboard, RGB display, and — optionally later — the on-device Envoy data fetch).

## 2. Why this is worth doing

1. **The ghosting bug lives in a layer we can't touch today.** Root cause is ESP-IDF issue #19070, a one-line bug in the `esp_lcd` RGB driver (`esp_lcd_panel_rgb.c`). Under PlatformIO/Arduino that driver arrives as a precompiled `libesp_lcd.a`, so fixing it required a Docker rebuild + `.a` swap. Under pure IDF it is source in our tree — the fix is a one-line edit and rebuild.
2. **The Arduino layer is thin for this project.** The app is WiFi/TLS + LVGL + an RGB panel. Every one of those has a first-class IDF equivalent; Arduino was mostly providing ergonomic wrappers, not capability.
3. **Long-term control.** Source-level access to every driver, finer control over memory/threading/power, and no more precompiled-blob surprises.

## 3. Current vs target architecture

**Current:**

```
main.cpp + LVGL + ESP32_Display_Panel + ArduinoJson + Arduino WiFi
        ↓
Arduino core 3.3.12  (bundles precompiled ESP-IDF v5.5.5 as .a files)
        ↓
PlatformIO build system
```

**Target:**

```
IDF components (main/, lvgl, esp_lvgl_port, esp_lcd, esp_wifi, esp_http_client, ...)
        ↓
idf.py / CMake build system
        ↓
ESP-IDF (from source, in-tree)
```

## 4. Component mapping

| Current (Arduino/PlatformIO) | Pure ESP-IDF equivalent | Notes |
|---|---|---|
| Arduino `WiFi` / `Networking` 3.3.12 | `esp_wifi` + `esp_netif` | Arduino's is a thin wrapper over this |
| TLS + HTTP fetch (currently off-device) | `esp_http_client` (HTTPS via mbedTLS) | Only needed if on-device fetch returns |
| LVGL 8.4.0 | `lvgl` component + `esp_lvgl_port` | Pure C; UI code is fully portable |
| `esp_lcd` (precompiled `libesp_lcd.a`) | `esp_lcd` component (source in tree) | Same driver, now patchable |
| `ESP32_Display_Panel` 1.0.0 (vendored) | `esp-bsp` board support **or** direct `esp_lcd` init | Most Arduino-flavored piece; see §5.3 |
| ArduinoJson 7.4.3 | ArduinoJson (header-only, vendored) **or** cJSON | cJSON is built into IDF |
| `board_build.arduino.memory_type = qio_opi` | `CONFIG_SPIRAM_*` / flash mode in `sdkconfig` | Must mirror PSRAM/flash config |

## 5. Decision points (resolve before starting)

### 5.1 Pure IDF vs IDF + Arduino-as-component

- **Pure IDF** — full migration, drop Arduino entirely. More upfront work, most control.
- **IDF + Arduino-as-component** — use `idf.py` but add `arduino-esp32` as a component. Lighter migration; keeps Arduino-style code/APIs and libraries while gaining source-level IDF (including `esp_lcd`). Good intermediate step.

**Recommendation:** if the goal is to *reduce* churn, start with **IDF + Arduino-as-component** and only drop the Arduino component later if it's truly unused. If the goal is a clean long-term codebase, go **pure IDF** now.

### 5.2 ESP-IDF version + the #19070 fix

As of 2026-11-20:

| Ref | #19070 fix |
|---|---|
| `v5.5.5` tag | ❌ buggy (current bundled version) |
| `v5.5.6` tag (newest 5.5.x release) | ❌ buggy |
| `release/v5.5` branch head | ✅ fixed |
| `master` | ✅ fixed |

The fix has **not shipped in a tagged release**. Options:

- Pin `release/v5.5` (branch) — gets the fix, but is a moving target.
- Pin `v5.5.6` tag + cherry-pick the one-line fix — reproducible, matches a blessed release.

**Recommendation:** pin a tag and cherry-pick the fix (or wait for a `v5.5.7` tag if it lands). Keep the known-good runtime config: RGB565, PSRAM framebuffer, double buffering, bounce buffer ×10 (8000 px), PCLK 13 MHz, `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`.

### 5.3 Board init: esp-bsp vs manual

- **esp-bsp** has support for the Waveshare ESP32-S3-Touch-LCD-7 and encapsulates the RGB panel config, GPIO, touch, backlight, etc.
- **Manual** `esp_lcd` + `esp_lvgl_port` init gives full control and matches our known-good settings exactly.

**Recommendation:** start from esp-bsp for speed, then override the RGB timing/PCLK to match the known-good 13 MHz config.

## 6. Work breakdown + estimates

Estimates assume **one focused developer** (~6 productive hours/day). Ranges reflect uncertainty, not slop.

### Phase 0 — Spike / validation (0.5–1 day)
- Confirm esp-bsp (or Waveshare IDF example) actually targets this board and panel.
- Confirm LVGL + esp_lvgl_port versions resolve cleanly in IDF.
- Confirm the one-line #19070 fix applies cleanly to the chosen tag.
- **Deliverable:** proof that display init + LVGL render works on the target IDF version.

### Phase 1 — Project scaffold (0.5–1 day)
- Create `idf.py` project, top-level `CMakeLists.txt`, `sdkconfig.defaults`.
- Add component-registry dependencies (`lvgl/lvgl`, `espressif/esp_lvgl_port`, `espressif/esp-bsp` as needed).
- Mirror current `sdkconfig`: target `esp32s3`, PSRAM `qio_opi`-equivalent, flash size/partition, `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`, `CONFIG_LCD_RGB_ISR_IRAM_SAFE`/`CONFIG_LCD_ENABLE_DEBUG_LOG` off.
- **Deliverable:** clean build skeleton.

### Phase 2 — Display + LVGL bring-up (1–3 days) ⚠️ biggest risk
- RGB panel init (GPIO, timing, PCLK 13 MHz, RGB565), framebuffer in PSRAM, double buffering, bounce buffer ×10.
- LVGL port via `esp_lvgl_port` (tick source, flush callback, buffer management).
- Verify rendering visually matches current firmware.
- **Deliverable:** dashboard renders identically, no ghost (baseline idle).

### Phase 3 — Port app logic (1–3 days) ⚠️ risk depends on Arduino-isms in `main.cpp`
- Port renderer / socket-parse logic to IDF tasks (FreeRTOS).
- Replace Arduino-specific idioms (`String`, `delay()`, `millis()` etc.) with std/IDF/FreeRTOS.
- JSON: vendor ArduinoJson header (drop-in) or switch to cJSON.
- **Deliverable:** app logic runs on IDF, feature-parity with current firmware.

### Phase 4 — Networking (1–2 days) *(only if re-enabling on-device Envoy fetch)*
- WiFi connect via `esp_wifi` + `esp_netif`.
- TLS fetch via `esp_http_client` + mbedTLS (cert handling, time sync for TLS).
- **Note:** fetch is currently off-device; this phase is optional and should be a *separate* change/test from the migration.

### Phase 5 — Apply driver fix + validate (0.5–1 day + overnight watch)
- Apply/confirm the #19070 one-line fix (or rely on branch head).
- Rebuild, flash, run the corruption-triggering workload.
- Overnight ghost regression watch.
- **Deliverable:** fixed driver confirmed, no persistent ghost.

## 7. Rollup estimate

| Phase | Estimate |
|---|---|
| 0 Spike | 0.5–1 day |
| 1 Scaffold | 0.5–1 day |
| 2 Display + LVGL | 1–3 days |
| 3 App logic port | 1–3 days |
| 4 Networking (optional) | 1–2 days |
| 5 Fix + validate | 0.5–1 day (+ overnight) |
| **Total (happy path)** | **~4–5 days** |
| **Total (conservative)** | **~2 weeks** |

The two swing factors are **Phase 2** (display bring-up is fiddly) and **Phase 3** (how deeply Arduino APIs are woven into `main.cpp`).

## 8. Risks

- **Display bring-up regressions** — RGB panel timing/GPIO is the easiest thing to get subtly wrong. Mitigate by preserving the known-good values (13 MHz PCLK, ×10 bounce, double buffering) exactly.
- **Reintroducing the ghost** — if Phase 5 is skipped and a buggy tag is used, the bug returns. Mitigate by pinning the fix explicitly.
- **Scope creep** — re-enabling the on-device Envoy fetch must be its own change, not bundled into the migration.
- **Loss of Arduino ecosystem convenience** — some examples/libraries won't apply; expect to write more glue.

## 9. Open questions

- [ ] Which path: pure IDF, or IDF + Arduino-as-component?
- [ ] esp-bsp vs manual board init?
- [ ] Pin `release/v5.5` branch, or pin a tag + cherry-pick the fix?
- [ ] Is the on-device Envoy fetch in-scope for this migration, or a follow-up?
- [ ] cJSON vs vendored ArduinoJson?

## 10. Reference artifacts

- Firmware: `/Users/johnolson/Code/waveshare/firmware` (env `waveshare-esp32-s3-lcd7`)
- Board: Waveshare ESP32-S3-Touch-LCD-7 (800×480 RGB565, ESP32-S3)
- Current framework: `framework-arduinoespressif32` 4.30312.261024 (Arduino core 3.3.12) + `framework-arduinoespressif32-esp32s3-libs` 3.3.12 (ESP-IDF v5.5.5)
- Driver bug: https://github.com/espressif/esp-idf/issues/19070
- Ghost investigation: `docs/GHOSTING_INVESTIGATION.md`
- Prior fix workflow (for reference only): Docker `espressif/idf:v5.5.5`, patch `components/esp_lcd/rgb/esp_lcd_panel_rgb.c`, harvest `libesp_lcd.a`.

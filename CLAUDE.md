# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO/Arduino, C++) for the ESP32-2432S028R "Cheap Yellow
Display" (CYD): a 2.8" ILI9341 TFT + XPT2046 resistive touch, generic 30-pin
ESP32 dev module. It's an MP3 file browser: scans an SD card's root for
`.mp3` files and lists them on an LVGL touch UI, with a WiFi connection
manager (captive-portal setup) alongside.

## Commands

Build (`esp32-cyd` is the only environment defined):
```
pio run -e esp32-cyd
```
Flash to a connected board:
```
pio run -e esp32-cyd -t upload
```
Serial monitor (115200 baud, set in platformio.ini):
```
pio device monitor
```
No test suite exists yet (`test/` is the stock PlatformIO placeholder).

## Architecture

`src/main.cpp` wires the modules together in `setup()`, in an order that
matters — see the SPI note below. `loop()` is just `lv_timer_handler()`.

- **display.cpp/h** — TFT_eSPI panel + XPT2046 touch, bridged into LVGL v9.
  `display_init_panel()` brings up the raw TFT_eSPI panel only.
  `display_init_input()` brings up touch (running one-time two-corner
  calibration, cached in NVS via Preferences) and creates the LVGL display
  + input device (`disp_flush_cb` / `touchpad_read_cb`).
- **storage.cpp/h** — `load_mp3_catalog()` scans the SD root into the global
  `mp3Files`/`mp3FileCount` arrays (`storage.h`), filtering directories and
  dotfiles (macOS FAT litter like `._x.mp3`, `.DS_Store`).
- **ui.cpp/h** — `build_main_screen(sd_present)` builds the whole screen:
  file list (or an "insert an SD card" prompt), plus a WiFi status label
  and `ui_show_wifi_setup_dialog()`/`ui_hide_wifi_setup_dialog()` (a modal
  on `lv_layer_top()`, so it floats over the screen's content untouched).
  These setters force their own `lv_timer_handler()` repaint since they're
  called from `wifi_manager.cpp` while it may be blocking `loop()`.
- **wifi_manager.cpp/h** — `wifi_connect()` via tzapu/WiFiManager. Tries
  the network saved in NVS; if none works, opens a captive portal AP
  ("Annota-Setup", no password) with no timeout and shows the on-screen
  dialog, blocking until the user configures a network from a phone/laptop.
  Must be called after `build_main_screen()` so it has a screen to paint
  status onto.

### Shared SPI peripheral gotcha

The ESP32 has only two general-purpose SPI peripherals. The display panel
(`TFT_MISO`/`MOSI`/`SCLK` in `include/User_Setup.h`) occupies one full-time.
Touch (XPT2046, pins 25/32/39) and the SD card (pins 18/19/23) are wired to
**separate SPI buses from each other too** despite both being the "spare"
peripheral — they have to take turns on it. That's why `load_mp3_catalog()`
must run *before* `display_init_input()`: SD's `SPIClass` is released
(`SD.end()` / `sdSPI.end()`) before touch claims the same peripheral. Don't
reorder `main.cpp`'s `setup()` without preserving that.

### TFT_eSPI configuration

`include/User_Setup.h` is force-included ahead of every translation unit via
`-include include/User_Setup.h` in `platformio.ini` (TFT_eSPI's own
`User_Setup_Select.h` is skipped). Pin/driver changes go there, not in a
new file.

### LVGL configuration

`include/lv_conf.h` is pulled in via `-D LV_CONF_INCLUDE_SIMPLE=1` (`include/`
is added to the search path explicitly since PlatformIO doesn't do it for
lib_deps sources like lvgl itself). The pinned lvgl version (9.2.2) is a
config-compatible match for this v9.2.0-format `lv_conf.h` — don't bump it
without checking that.

### Filename gotcha (case-insensitive filesystem)

This repo is developed on macOS's default case-insensitive filesystem.
`#include <WiFi.h>` (the Arduino core header) will silently resolve to a
project file named `wifi.h`/`wifi.cpp` sitting in `-Isrc`, breaking the
build in confusing ways. That's why the WiFi module is named
`wifi_manager.*`, not `wifi.*` — keep that naming if you touch it.

### Dependency pin notes (see comments in platformio.ini)

- `XPT2046_Touchscreen` is pulled straight from GitHub at tag `v1.4` — the
  PlatformIO registry only mirrors an untagged `0.0.0-alpha` build that
  version-pin syntax can't resolve.
- `lvgl` is pinned to `9.2.2` — the registry mirrors lvgl's git tags, which
  jump from `9.2.2` straight to `9.3.0`; `9.2.2` is what's config-compatible
  with `lv_conf.h`.
- The platform itself is the `pioarduino` fork of `espressif32`, tracking
  newer `arduino-esp32` core releases than the stock PlatformIO platform.

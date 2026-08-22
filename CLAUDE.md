# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO/Arduino, C++) for the ESP32-2432S028R "Cheap Yellow
Display" (CYD): a 2.8" ILI9341 TFT + XPT2046 resistive touch, generic 30-pin
ESP32 dev module. It's an MP3 file browser: scans an SD card's root for
`.mp3` files and lists them on an LVGL touch UI, with a WiFi connection
manager (captive-portal setup) and an HTTP file manager for the SD card
alongside. Long-pressing an audio file's card offers to transcribe it via
OpenAI's API, saving the result as a sibling `.txt` file.

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
matters — see the SPI note below. `loop()` calls `lv_timer_handler()` first,
then, only after it returns, the deferred-work pumps that a nested LVGL
click handler can't safely trigger directly —
`wifi_process_pending_reconnect()` and `transcribe_process_pending()` (see
their bullets below) — then `web_server_handle()`.

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
  called from `wifi_manager.cpp` while it may be blocking `loop()`. The
  Settings view (same file) also holds the OpenAI API key field —
  `transcribe.h`'s `openai_set_api_key()`/`openai_get_api_key()` — with an
  `lv_keyboard` created lazily on first focus, parented to `lv_layer_top()`
  the same way the dialogs are so it floats instead of squeezing the
  layout. Long-pressing an audio card (only wired up while the list is
  showing `.mp3`s, not `.txt` transcripts) opens a Yes/No confirm dialog;
  Yes calls `transcribe.h`'s `transcribe_request()` rather than blocking
  right there — same reason as `wifi_request_reconnect()` below.
  `ui_show_transcribe_progress()`/`ui_show_transcribe_result()` are what
  `transcribe_process_pending()` calls back into once it's actually safe
  to block and repaint.
- **wifi_manager.cpp/h** — `wifi_connect()` via tzapu/WiFiManager. Two
  paths depending on whether a network is already saved in NVS: none
  saved opens a captive portal AP ("Annota-Setup", no password) with no
  timeout and shows the on-screen dialog, blocking until the user
  configures one from a phone/laptop; one saved reconnects to it directly
  (no portal) for up to 30 seconds. WiFiManager's "saved" check reads
  ESP-IDF's own NVS station config, which can be stale (left over from a
  different sketch ever flashed to this board) rather than something the
  user actually set up here, so a timeout there wipes the saved
  credentials and falls back to the same setup portal instead of
  stranding the device offline with no way back to configuring an AP.
  Only if that fallback portal itself is exited without connecting does
  it show a warning dialog with a Close button and let the device run
  offline — that dialog only dismisses itself; reconnecting again from
  there is a separate, explicit action via the main screen's Settings
  view "Reconnect WiFi" button, which only calls
  `wifi_request_reconnect()` (it fires from an LVGL click handler already
  nested inside `lv_timer_handler()`, which refuses to run itself again
  while it's running — so the actual retry can't happen there); `loop()`
  picks up the request via `wifi_process_pending_reconnect()`, called
  right after `lv_timer_handler()` returns, never nested inside it. Must
  be called after `build_main_screen()` so it has a screen to paint
  status onto.
- **transcribe.cpp/h** — OpenAI API key storage (NVS, `annota` namespace —
  same one `display.cpp` uses for touch calibration, different key) plus
  `transcribe_file()`, which uploads an SD-root `.mp3` to OpenAI's
  `/v1/audio/transcriptions` (`whisper-1`) and writes the result to a
  sibling `.txt`. The upload streams straight off the SD card through a
  custom `Stream` subclass wrapping the multipart preamble/file/trailer —
  the ESP32 doesn't have enough RAM to buffer a whole audio file first.
  TLS cert validation is skipped (`WiFiClientSecure::setInsecure()`); no
  root-CA bundle exists in this project. Like `wifi_manager.cpp`'s
  `wifi_request_reconnect()`/`wifi_process_pending_reconnect()` pair,
  `transcribe_request()`/`transcribe_process_pending()` split the "ask for
  it" (from `ui.cpp`'s LVGL click handler) from the "actually block and
  repaint" (from `main.cpp`'s `loop()`, after `lv_timer_handler()` returns)
  for the same reentrancy reason. Claims the SD card itself
  (`storage.h`'s `sd_begin()`/`sd_end()`) but leaves pausing/resuming touch
  to the caller — `transcribe_process_pending()` does that around the
  whole blocking call, same shared-SPI dance as everything else below.
- **web_server.cpp/h** — `web_server_start()`/`web_server_handle()`, an
  ESP32-core `WebServer` on port 80 serving a single-page file manager
  (list/download/upload/delete files on the SD root) at `/`, backed by
  `/api/files`, `/api/download`, `/api/upload`, `/api/delete`. Only started
  once `wifi_connect()` succeeds. Each handler that touches the card calls
  `display_suspend_touch()` + `storage.h`'s `sd_begin()` (and releases both
  after) to borrow the shared SPI peripheral from touch for that one
  request — see the SPI note below. Uploads/deletes don't refresh the
  on-screen MP3 list (`mp3Files`); that only happens on reboot.

### Shared SPI peripheral gotcha

The ESP32 has only two general-purpose SPI peripherals. The display panel
(`TFT_MISO`/`MOSI`/`SCLK` in `include/User_Setup.h`) occupies one full-time.
Touch (XPT2046, pins 25/32/39) and the SD card (pins 18/19/23) are wired to
**separate SPI buses from each other too** despite both being the "spare"
peripheral — they have to take turns on it. That's why `load_mp3_catalog()`
must run *before* `display_init_input()`: SD's `SPIClass` is released
(`SD.end()` / `sdSPI.end()`) before touch claims the same peripheral. Don't
reorder `main.cpp`'s `setup()` without preserving that.

Same constraint applies at runtime once touch owns the peripheral for good:
`web_server.cpp`'s handlers pause touch (`display_suspend_touch()`), borrow
the bus for SD (`storage.h`'s `sd_begin()`/`sd_end()`), then resume touch
(`display_resume_touch()`) — the touchscreen goes unresponsive for the
duration of whatever SD request is in flight. Any new code that touches the
SD card after boot needs the same pause/claim/release/resume dance.
`transcribe.cpp`'s `transcribe_process_pending()` does the same thing
around its own (much longer-running) `transcribe_file()` call, which keeps
the SD card claimed the whole time it's streaming the file to OpenAI.

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

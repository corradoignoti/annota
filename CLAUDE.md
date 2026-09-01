# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO/Arduino, C++), targeting two boards (see
`platformio.ini`'s two envs and "Board variants" below): the
ESP32-2432S028R "Cheap Yellow Display" (CYD) — a 2.8" ILI9341 TFT +
XPT2046 resistive touch, generic 30-pin ESP32 dev module — and the
Waveshare ESP32-S3-ePaper-1.54 — a 1.54" 200x200 mono e-paper panel + 2
onboard buttons, ESP32-S3, no touch. It's an MP3 file browser: scans an SD
card's root for `.mp3` files and lists them on an LVGL UI, with a WiFi
connection manager (captive-portal setup) and an HTTP file manager for the
SD card alongside. Long-pressing (CYD) or selecting (e-paper) an audio
file offers to transcribe it via an AI provider's API (OpenAI by default,
selected at compile time — see `transcribe.cpp/h` below), saving the
result as a sibling `.txt` file.

## Commands

Build (`esp32-cyd` and `esp32-s3-epaper154` are the two environments defined):
```
pio run -e esp32-cyd
pio run -e esp32-s3-epaper154
```
Flash to a connected board:
```
pio run -e esp32-cyd -t upload
pio run -e esp32-s3-epaper154 -t upload
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

- **display.cpp/h** (esp32-cyd only — see "Board variants") — TFT_eSPI
  panel + XPT2046 touch, bridged into LVGL v9. `display_init_panel()`
  brings up the raw TFT_eSPI panel only. `display_init_input()` brings up
  touch (running one-time two-corner calibration, cached in NVS via
  Preferences) and creates the LVGL display + input device (`disp_flush_cb`
  / `touchpad_read_cb`).
- **display_epaper.cpp** (esp32-s3-epaper154 only) — the same `display.h`
  interface's other implementation: an SSD1681-class e-paper panel driver
  (command/LUT sequence ported from Waveshare's own example repo,
  waveshareteam/ESP32-S3-ePaper-1.54) bridged into LVGL v9, plus the two
  onboard buttons (`display_button_poll()`, declared in `display.h` behind
  `#ifdef BOARD_ESP32S3_EPAPER154`). `disp_flush_cb` renders LVGL's normal
  RGB565 framebuffer (`LV_DISPLAY_RENDER_MODE_FULL`, so it always sees the
  whole 200x200 screen in one call — there's no such thing as updating a
  sub-rect on this controller) and thresholds it to 1bpp on the way out,
  rather than switching `lv_conf.h` to a monochrome color depth. No touch,
  no backlight, no shared-SPI peripheral to hand off (see "Board
  variants") — `display_suspend_touch()`/`display_resume_touch()`/
  `display_set_backlight()` are no-op stubs here so their callers
  (`web_server.cpp`, `transcribe.cpp`) don't need their own board `#ifdef`.
- **storage.cpp/h** — `load_mp3_catalog()` scans the SD root into the global
  `mp3Files`/`mp3FileCount` arrays (`storage.h`), filtering directories and
  dotfiles (macOS FAT litter like `._x.mp3`, `.DS_Store`). One file for
  both boards (unlike display/ui below) since only `sd_begin()`/`sd_end()`
  and the pin defines actually differ per board — see "Board variants".
- **ui.cpp/h** (esp32-cyd only — see "Board variants") —
  `build_main_screen(sd_present)` builds the whole screen:
  file list (or an "insert an SD card" prompt), plus a WiFi status label
  and `ui_show_wifi_setup_dialog()`/`ui_hide_wifi_setup_dialog()` (a modal
  on `lv_layer_top()`, so it floats over the screen's content untouched).
  These setters force their own `lv_timer_handler()` repaint since they're
  called from `wifi_manager.cpp` while it may be blocking `loop()`. The
  Settings view (same file) also holds the API key field for whichever AI
  provider is compiled in — `transcribe.h`'s `ai_provider_set_api_key()`/
  `ai_provider_get_api_key()`, labeled dynamically via `ai_provider_name()`
  — with an `lv_keyboard` created lazily on first focus, parented to
  `lv_layer_top()`
  the same way the dialogs are so it floats instead of squeezing the
  layout. Long-pressing an audio card (only wired up while the list is
  showing `.mp3`s, not `.txt` transcripts) opens a Yes/No confirm dialog;
  Yes calls `transcribe.h`'s `transcribe_request()` rather than blocking
  right there — same reason as `wifi_request_reconnect()` below.
  `ui_show_transcribe_progress()`/`ui_show_transcribe_result()` are what
  `transcribe_process_pending()` calls back into once it's actually safe
  to block and repaint.
- **ui_epaper.cpp** (esp32-s3-epaper154 only) — the same `ui.h` interface's
  other implementation, deliberately smaller in scope than `ui.cpp`: WiFi
  status, a scrollable file list, and per-file Transcribe/Delete — no
  on-screen Settings, WiFi credential entry, or text-file preview, all of
  which stay on `web_server.cpp`'s existing web UI (works unchanged on
  this board too). A small explicit state machine (`Screen` enum) driven
  by `display.h`'s `display_button_poll()` via `ui_process_input()` (a
  no-op stub on esp32-cyd, where input flows through LVGL's touch indev
  instead — `main.cpp`'s `loop()` calls it unconditionally either way),
  not a port of `ui.cpp`'s touch-driven widget tree: Next cycles the
  current selection/menu option, Select opens/confirms (short press) or
  backs out (long press). Every screen is rebuilt from scratch
  (`lv_obj_clean()` + repopulate) on each state change rather than kept as
  a tree of show/hide-toggled widgets — cheap next to the e-paper refresh
  itself dominating either way. Selecting Transcribe calls
  `transcribe.h`'s `transcribe_request()` directly rather than through a
  confirm dialog — safe here (unlike a hypothetical touch click handler)
  since `ui_process_input()` runs at `loop()`'s top level, not nested
  inside `lv_timer_handler()`; selecting Delete calls `storage.h`'s
  `delete_file()` directly, same reasoning.
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
- **transcribe.cpp/h + transcribe_&lt;provider&gt;.cpp** — AI transcription,
  split into a provider-agnostic half and a provider-specific half so a
  future second provider is a new file plus a new build flag, not a
  rewrite. `transcribe.h` declares the whole public surface — generic
  names only (`ai_provider_name()`, `ai_provider_has_api_key()`/
  `..._get_api_key()`/`..._set_api_key()`, `ai_transcribe_file()`) — and
  `ui.cpp`/`web_server.cpp` only ever call those, never anything
  provider-specific. `transcribe.cpp` implements the provider-agnostic
  part: `transcribe_request()`/`transcribe_process_pending()` mirror
  `wifi_manager.cpp`'s `wifi_request_reconnect()`/
  `wifi_process_pending_reconnect()` pair, splitting the "ask for it"
  (from `ui.cpp`'s LVGL click handler) from the "actually block and
  repaint" (from `main.cpp`'s `loop()`, after `lv_timer_handler()`
  returns) for the same reentrancy reason; it also `#error`s at compile
  time if no `AI_PROVIDER_*` build flag is defined, so a missing one fails
  loudly here instead of as a confusing link error. Exactly one
  `transcribe_<provider>.cpp` implements the rest (`ai_provider_name()`
  and `ai_transcribe_file()`) — each file's entire body is wrapped in
  `#ifdef AI_PROVIDER_<NAME>`, so every provider file can sit in `src/`
  at once and only the one selected by `platformio.ini`'s build_flags
  (currently `-D AI_PROVIDER_OPENAI=1`) compiles to anything. NVS keys are
  namespaced per provider (e.g. `openaiKey`) so switching the compiled-in
  provider doesn't feed it a stale key saved for a different one.
  `transcribe_openai.cpp` (`whisper-1`, `/v1/audio/transcriptions`)
  streams the upload straight off the SD card through a custom `Stream`
  subclass wrapping the multipart preamble/file/trailer — the ESP32
  doesn't have enough RAM to buffer a whole audio file first — and skips
  TLS cert validation (`WiFiClientSecure::setInsecure()`); no root-CA
  bundle exists in this project. `ai_transcribe_file()` claims the SD card
  itself (`storage.h`'s `sd_begin()`/`sd_end()`) but leaves pausing/
  resuming touch to the caller — `transcribe_process_pending()` does that
  around the whole blocking call, same shared-SPI dance as everything
  else below.
- **web_server.cpp/h** — `web_server_start()`/`web_server_handle()`, an
  ESP32-core `WebServer` on port 80. Two pages, same dark palette as
  `ui.cpp`: a file manager (list/download/upload/delete files on the SD
  root) at `/`, backed by `/api/files`, `/api/download`, `/api/upload`,
  `/api/delete`; and a `/settings` page mirroring `ui.cpp`'s on-screen
  Settings view (WiFi/clock status, SD capacity, Reconnect WiFi, Delete
  WiFi Setup, and the AI provider's API key field, labeled dynamically
  from `aiProviderName` in the JSON below), backed by `/api/settings`
  (GET, a status snapshot) and `/api/settings/reconnect`,
  `/api/settings/forget`, `/api/settings/ai-key` (POST). Only started
  once `wifi_connect()` succeeds. Each handler that touches the card calls
  `display_suspend_touch()` + `storage.h`'s `sd_begin()` (and releases both
  after) to borrow the shared SPI peripheral from touch for that one
  request — see the SPI note below; the AI key handler is the one
  exception, since `transcribe.h`'s `ai_provider_set_api_key()` is pure
  NVS and never touches the SD card. Like `ui.cpp`'s own field, this
  server is plain HTTP, so `/api/settings` reports only whether a key is
  saved, never the key itself — the web page can clear or overwrite it but
  never displays the current value. Uploads/deletes don't refresh the
  on-screen MP3 list (`mp3Files`); that only happens on reboot.

### Board variants

Two `platformio.ini` envs, selected by exactly one `BOARD_*` build flag
(`storage.cpp` `#error`s at compile time if neither is defined, same
pattern as `transcribe.cpp`'s `AI_PROVIDER_*` check): `esp32-cyd` defines
`BOARD_CYD=1`, `esp32-s3-epaper154` defines `BOARD_ESP32S3_EPAPER154=1`.
`display.h`/`ui.h` are the shared interfaces; which board-specific file
implements each is picked by `build_src_filter` per env (`display.cpp` vs
`display_epaper.cpp`, `ui.cpp` vs `ui_epaper.cpp`), *not* the
`#ifdef`-wrapped-single-file trick `transcribe_<provider>.cpp` uses — see
`platformio.ini`'s comment on why: those provider files all share the same
`lib_deps` regardless of which one compiles to something, so PlatformIO's
library dependency finder (LDF) is happy either way, but `display.cpp`
`#include`s TFT_eSPI/XPT2046_Touchscreen, which are only in `esp32-cyd`'s
`lib_deps` — the LDF's textual `#include` scan doesn't evaluate
`#ifdef`/`#endif` the way the compiler does, so it'd fail resolving those
headers if `esp32-s3-epaper154` tried to compile that file too. `lv_conf.h`
is shared by both envs (`LV_CONF_INCLUDE_SIMPLE=1` in both), so its
`LV_USE_TFT_ESPI` is itself `#ifdef BOARD_ESP32S3_EPAPER154`-gated to 0 for
the same reason — left at 1 unconditionally, lvgl's own
`drivers/display/tft_espi/lv_tft_espi.cpp` fails the same missing-header
way, deeper inside a lib_dep than `build_src_filter` can reach. Files with
only a small, genuinely-shared-dependency difference between boards
(`storage.cpp`'s SD-over-SPI vs SD-over-SDMMC; `antiburn.cpp`'s
CYD-backlight-only blanking) keep the single-file-with-`#ifdef` pattern,
same as the provider split - see each file's own comment.

`esp32-s3-epaper154`'s on-device UI (`ui_epaper.cpp`) is deliberately
smaller in scope than the CYD's touch UI - see its bullet above.

### Shared SPI peripheral gotcha (esp32-cyd only)

`esp32-s3-epaper154` has no equivalent constraint: its e-paper panel is on
its own dedicated `SPI2_HOST` and its SD card on the ESP32-S3's dedicated
SDMMC peripheral (1-bit mode, pins 39/41/40 - see `storage.cpp`) - two
separate peripherals, neither shared with anything, so
`display_suspend_touch()`/`display_resume_touch()` are no-ops there (see
`display_epaper.cpp`'s bullet above) and there's no boot-order constraint
between `load_mp3_catalog()` and `display_init_input()` either - `main.cpp`
just keeps the same call order as `esp32-cyd` regardless, so `setup()`
doesn't need its own board `#ifdef`.

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
around its own (much longer-running) `ai_transcribe_file()` call, which
keeps the SD card claimed the whole time it's streaming the file to
whichever provider is compiled in.

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

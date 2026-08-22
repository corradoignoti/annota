# Annota

Firmware for the ESP32-2432S028R — the "Cheap Yellow Display" (CYD): a
2.8" ILI9341 TFT with XPT2046 resistive touch, on a generic 30-pin ESP32
dev module. It turns the board into a standalone MP3 browser and
recorder companion: it scans an SD card's root for `.mp3` files, lists
them on a touch UI built with LVGL, and can send any of them off to an
AI provider for transcription, saving the result as a sibling `.txt`
file. A WiFi connection manager handles onboarding via a captive
portal, and a small HTTP file manager mirrors the on-device UI so the
SD card's contents are reachable from a browser on the same network.

Built with PlatformIO (Arduino framework, C++), against the
[pioarduino](https://github.com/pioarduino/platform-espressif32) fork
of the espressif32 platform.

## Hardware

- **Board**: ESP32-2432S028R ("CYD"), generic 30-pin ESP32 dev module
- **Display**: 2.8" ILI9341 TFT, driven by TFT_eSPI
- **Touch**: XPT2046 resistive touch controller
- **Storage**: microSD card slot on the board, FAT-formatted, `.mp3`
  files placed in its root
- **Partition scheme**: Huge App (3MB app / 1MB SPIFFS, no OTA)

The display and touch controller share pins with the SD card across
only two SPI peripherals total — see [Shared SPI
peripheral](#shared-spi-peripheral) below, since it constrains both
boot order and any new code that touches the card.

## Features

- **MP3 browser** — lists every `.mp3` in the SD card's root as a
  scrollable card list, skipping directories and dotfiles (macOS FAT
  litter like `._x.mp3`, `.DS_Store`).
- **WiFi onboarding** — first boot (or after credentials are forgotten)
  opens an open captive-portal access point, `Annota-Setup`, so a phone
  or laptop can configure a network without touching the device's own
  small screen. Once a network is saved, the device reconnects to it
  directly on subsequent boots.
- **AI transcription** — long-press an audio card to send that file to
  a configurable AI provider's transcription API; the result is saved
  next to the file as `<name>.txt` and appears in the file list. The
  provider is a compile-time choice (OpenAI Whisper or Google Gemini —
  see [AI transcription provider](#ai-transcription-provider)), with
  its API key entered either on-device or via the web UI and stored in
  NVS.
- **Web file manager** — once WiFi is up, an HTTP server on port 80
  serves a file manager (list/upload/download/delete against the SD
  root) and a settings page (WiFi status, SD capacity, reconnect/forget
  WiFi, AI provider API key) mirroring the on-device Settings view.

## Getting started

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code
extension).

```sh
# Build
pio run -e esp32-cyd

# Flash to a connected board
pio run -e esp32-cyd -t upload

# Serial monitor (115200 baud)
pio device monitor
```

`esp32-cyd` is the only environment defined. There's no test suite yet
(`test/` is the stock PlatformIO placeholder).

### First run

1. Put some `.mp3` files in the root of a FAT-formatted SD card and
   insert it (or boot without one — the UI shows an "insert an SD
   card" prompt instead of the file list).
2. Power on. If no WiFi network is saved, the device opens the
   `Annota-Setup` access point (no password) and shows an on-screen
   dialog; connect to it from a phone or laptop and use the captive
   portal to pick a network.
3. Once connected, the on-screen WiFi status label updates and the web
   file manager comes up on the device's IP, port 80.
4. To use transcription, open Settings (on-device or at `/settings` in
   the browser) and enter an API key for whichever provider is
   compiled in.

## Architecture

`src/main.cpp` wires everything together in `setup()`, in an order
that matters — see [Shared SPI peripheral](#shared-spi-peripheral).
`loop()` runs `lv_timer_handler()` first, then — only after it returns
— the deferred-work pumps that a nested LVGL click handler can't safely
trigger directly (`wifi_process_pending_reconnect()` and
`transcribe_process_pending()`), then `web_server_handle()`.

### `display.cpp` / `display.h`

Bridges the TFT_eSPI panel and XPT2046 touch into LVGL v9.
`display_init_panel()` brings up the raw TFT_eSPI panel only.
`display_init_input()` brings up touch — including a one-time
two-corner calibration cached in NVS via `Preferences` — and creates
the LVGL display and input device (`disp_flush_cb` /
`touchpad_read_cb`).

### `storage.cpp` / `storage.h`

`load_mp3_catalog()` scans the SD root into the global `mp3Files` /
`mp3FileCount` arrays, filtering out directories and dotfiles.

### `ui.cpp` / `ui.h`

Builds the whole screen via `build_main_screen(sd_present)`: the file
list (or the "insert an SD card" prompt), a WiFi status label, and
`ui_show_wifi_setup_dialog()` / `ui_hide_wifi_setup_dialog()` — a modal
on `lv_layer_top()` so it floats over the screen's content untouched.
Those setters force their own `lv_timer_handler()` repaint since
`wifi_manager.cpp` calls them while it may be blocking `loop()`.

The Settings view (same file) holds the API key field for whichever AI
provider is compiled in, via `transcribe.h`'s
`ai_provider_set_api_key()` / `ai_provider_get_api_key()`, labeled
dynamically from `ai_provider_name()`. An `lv_keyboard` is created
lazily on first focus, parented to `lv_layer_top()` like the dialogs
so it floats instead of squeezing the layout.

Long-pressing an audio card — only wired up while the list is showing
`.mp3`s, not `.txt` transcripts — opens a Yes/No confirm dialog; Yes
calls `transcribe.h`'s `transcribe_request()` rather than blocking
right there, for the same reentrancy reason as `wifi_request_reconnect()`
below. `ui_show_transcribe_progress()` / `ui_show_transcribe_result()`
are what `transcribe_process_pending()` calls back into once it's
actually safe to block and repaint.

### `wifi_manager.cpp` / `wifi_manager.h`

`wifi_connect()`, built on tzapu/WiFiManager, has two paths:

- **No network saved** — opens the `Annota-Setup` captive-portal AP (no
  password, no timeout) and shows the on-screen dialog, blocking until
  a network is configured from a phone or laptop.
- **Network saved** — reconnects directly (no portal), for up to 30
  seconds.

WiFiManager's "saved" check reads ESP-IDF's own NVS station config,
which can be stale — left over from a different sketch ever flashed to
the board — rather than something actually set up here. A timeout on
that path wipes the saved credentials and falls back to the same setup
portal, instead of stranding the device offline with no way back to
configuring an AP. Only if *that* fallback portal is itself exited
without connecting does it show a warning dialog with a Close button
and let the device run offline; that dialog only dismisses itself.
Reconnecting again from there is a separate, explicit action — the main
screen's Settings view "Reconnect WiFi" button, which only calls
`wifi_request_reconnect()` (it fires from an LVGL click handler already
nested inside `lv_timer_handler()`, which refuses to run itself again
while it's running, so the actual retry can't happen there). `loop()`
picks up the request via `wifi_process_pending_reconnect()`, called
right after `lv_timer_handler()` returns, never nested inside it.

Must be called after `build_main_screen()` so it has a screen to paint
status onto.

### `transcribe.cpp` / `transcribe.h` + `transcribe_<provider>.cpp`

AI transcription, split into a provider-agnostic half and a
provider-specific half so adding a future provider is a new file plus
a new build flag, not a rewrite.

- `transcribe.h` declares the whole public surface with generic names
  only (`ai_provider_name()`, `ai_provider_has_api_key()` /
  `..._get_api_key()` / `..._set_api_key()`, `ai_transcribe_file()`).
  `ui.cpp` and `web_server.cpp` only ever call those, never anything
  provider-specific.
- `transcribe.cpp` implements the provider-agnostic part:
  `transcribe_request()` / `transcribe_process_pending()` mirror
  `wifi_manager.cpp`'s `wifi_request_reconnect()` /
  `wifi_process_pending_reconnect()` pair, splitting "ask for it" (from
  `ui.cpp`'s LVGL click handler) from "actually block and repaint"
  (from `main.cpp`'s `loop()`, after `lv_timer_handler()` returns), for
  the same reentrancy reason. It `#error`s at compile time if no
  `AI_PROVIDER_*` build flag is defined, so a missing one fails loudly
  here instead of as a confusing link error.
- Exactly one `transcribe_<provider>.cpp` implements the rest
  (`ai_provider_name()` and `ai_transcribe_file()`). Each file's entire
  body is wrapped in `#ifdef AI_PROVIDER_<NAME>`, so every provider
  file can sit in `src/` at once and only the one selected by
  `platformio.ini`'s build flags compiles to anything.
- NVS keys are namespaced per provider (e.g. `openaiKey`) so switching
  the compiled-in provider doesn't feed it a stale key saved for a
  different one.

Two providers exist today:

- **`transcribe_openai.cpp`** (OpenAI Whisper, `whisper-1`, via
  `/v1/audio/transcriptions`) streams the upload straight off the SD
  card through a custom `Stream` subclass wrapping the multipart
  preamble/file/trailer — the ESP32 doesn't have enough RAM to buffer a
  whole audio file first — and skips TLS certificate validation
  (`WiFiClientSecure::setInsecure()`); no root-CA bundle exists in this
  project.
- **`transcribe_gemini.cpp`** (Google Gemini) implements the same
  interface for that provider.

`ai_transcribe_file()` claims the SD card itself (`storage.h`'s
`sd_begin()` / `sd_end()`) but leaves pausing/resuming touch to the
caller — `transcribe_process_pending()` does that around the whole
blocking call, the same shared-SPI dance as everything else (see
below).

### `web_server.cpp` / `web_server.h`

`web_server_start()` / `web_server_handle()` run an ESP32-core
`WebServer` on port 80, started only once `wifi_connect()` succeeds.
Two pages, in the same dark palette as `ui.cpp`:

- **`/`** — a file manager (list/download/upload/delete against the SD
  root), backed by `/api/files`, `/api/download`, `/api/upload`,
  `/api/delete`.
- **`/settings`** — mirrors `ui.cpp`'s on-screen Settings view (WiFi
  and clock status, SD capacity, Reconnect WiFi, Delete WiFi Setup, and
  the AI provider's API key field, labeled dynamically from
  `aiProviderName`), backed by `/api/settings` (GET, a status
  snapshot), `/api/settings/reconnect`, `/api/settings/forget`, and
  `/api/settings/ai-key` (POST).

Every handler that touches the card calls `display_suspend_touch()`
plus `storage.h`'s `sd_begin()` (releasing both afterward) to borrow
the shared SPI peripheral from touch for that one request — the AI key
handler is the one exception, since `ai_provider_set_api_key()` is pure
NVS and never touches the SD card. Like `ui.cpp`'s own field, this
server is plain HTTP, so `/api/settings` reports only whether a key is
saved, never the key itself — the web page can clear or overwrite it
but never displays the current value. Uploads and deletes don't
refresh the on-screen MP3 list; that only happens on reboot.

## Shared SPI peripheral

The ESP32 has only two general-purpose SPI peripherals. The display
panel (`TFT_MISO` / `MOSI` / `SCLK` in `include/User_Setup.h`) occupies
one full-time. Touch (XPT2046, pins 25/32/39) and the SD card (pins
18/19/23) are wired to **separate SPI buses from each other too**,
despite both being the "spare" peripheral — they have to take turns on
it.

That's why `load_mp3_catalog()` must run *before*
`display_init_input()` in `setup()`: SD's `SPIClass` is released
(`SD.end()` / `sdSPI.end()`) before touch claims the same peripheral.
Don't reorder `main.cpp`'s `setup()` without preserving that.

Once touch owns the peripheral for good, `web_server.cpp`'s handlers
pause touch (`display_suspend_touch()`), borrow the bus for SD
(`storage.h`'s `sd_begin()` / `sd_end()`), then resume touch
(`display_resume_touch()`) — the touchscreen goes unresponsive for the
duration of whatever SD request is in flight. `transcribe.cpp`'s
`transcribe_process_pending()` does the same thing around its own
(much longer-running) `ai_transcribe_file()` call, which keeps the SD
card claimed the whole time it's streaming the file to whichever
provider is compiled in. Any new code that touches the SD card after
boot needs the same pause/claim/release/resume dance.

## Configuration

### AI transcription provider

Selected at compile time in `platformio.ini`'s `build_flags` — exactly
one `AI_PROVIDER_*` flag must be defined:

```ini
-D AI_PROVIDER_OPENAI=1
; or
-D AI_PROVIDER_GEMINI=1
```

Swap the flag for a different provider instead of adding a second one.
The API key is entered afterward, at runtime, from Settings (on-device
or `/settings`) — it's stored in NVS under a key namespaced per
provider (e.g. `openaiKey`) so switching providers doesn't feed the new
one a stale key saved for the old one.

### TFT_eSPI panel

`include/User_Setup.h` is force-included ahead of every translation
unit via `-include include/User_Setup.h` in `platformio.ini`
(TFT_eSPI's own `User_Setup_Select.h` is skipped). Pin and driver
changes go there, not in a new file.

### LVGL

`include/lv_conf.h` is pulled in via `-D LV_CONF_INCLUDE_SIMPLE=1`
(`include/` is added to the search path explicitly, since PlatformIO
doesn't do that automatically for `lib_deps` sources like lvgl
itself). The pinned lvgl version (9.2.2) is a config-compatible match
for this v9.2.0-format `lv_conf.h` — don't bump it without checking
that.

## Dependencies

Declared in `platformio.ini`:

- `bblanchon/ArduinoJson @ 7.4.1`
- `tzapu/WiFiManager @ 2.0.17`
- `bodmer/TFT_eSPI @ 2.5.43`
- `PaulStoffregen/XPT2046_Touchscreen` — pinned to tag `v1.4`, pulled
  straight from GitHub (the PlatformIO registry only mirrors an
  untagged `0.0.0-alpha` build that version-pin syntax can't resolve)
- `lvgl/lvgl @ 9.2.2` — the registry mirrors lvgl's git tags, which
  jump from `9.2.2` straight to `9.3.0`; `9.2.2` is what's
  config-compatible with `lv_conf.h`

The platform itself is the `pioarduino` fork of `espressif32`, tracking
newer `arduino-esp32` core releases than the stock PlatformIO platform.

## Known gotcha: case-insensitive filesystems

This repo is developed on macOS's default case-insensitive filesystem.
`#include <WiFi.h>` (the Arduino core header) will silently resolve to
a project file named `wifi.h` / `wifi.cpp` sitting in `-Isrc`, breaking
the build in confusing ways. That's why the WiFi module here is named
`wifi_manager.*`, not `wifi.*` — keep that naming if you touch it.

## Repository layout

```
include/
  User_Setup.h   TFT_eSPI pin/driver config, force-included project-wide
  lv_conf.h      LVGL v9.2.0-format configuration
src/
  main.cpp               setup()/loop(), wires every module together
  display.cpp/.h         TFT_eSPI + XPT2046 → LVGL bridge
  storage.cpp/.h         SD card catalog scan
  ui.cpp/.h               LVGL screens: file list, settings, dialogs
  wifi_manager.cpp/.h    WiFiManager-based onboarding/reconnect
  transcribe.cpp/.h      provider-agnostic transcription request/dispatch
  transcribe_openai.cpp  OpenAI Whisper provider
  transcribe_gemini.cpp  Google Gemini provider
  web_server.cpp/.h      HTTP file manager + settings page
test/                    stock PlatformIO placeholder, unused
platformio.ini           build environment, dependencies, build flags
```

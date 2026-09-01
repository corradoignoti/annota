# Annota

Firmware for the Waveshare ESP32-S3-ePaper-1.54: a 1.54" 200x200 mono
e-paper panel, 2 onboard buttons, and an onboard ES8311 speaker/mic codec,
on an ESP32-S3. It turns the board into a standalone MP3/WAV browser and
recorder companion: it scans an SD card's root for audio files, lists them
on a small button-driven LVGL UI, can play them back or record a new voice
memo from the onboard mic, and can send any of them off to an AI provider
for transcription, saving the result as a sibling `.txt` file. A WiFi
connection manager handles onboarding via a captive portal, and a small
HTTP file manager mirrors the on-device UI so the SD card's contents are
reachable from a browser on the same network.

Built with PlatformIO (Arduino framework, C++), against the
[pioarduino](https://github.com/pioarduino/platform-espressif32) fork
of the espressif32 platform.

## Hardware

- **Board**: Waveshare ESP32-S3-ePaper-1.54
- **Display**: 1.54" 200x200 mono e-paper, SSD1681-class controller
- **Input**: 2 onboard buttons (BOOT/GPIO0 "Next", PWR/GPIO18 "Select") —
  no touch
- **Audio**: onboard ES8311 I2C codec + NS4150B amp (speaker) and mic ADC,
  over a shared I2S bus
- **Storage**: microSD card slot on the board, FAT-formatted, over the
  ESP32-S3's dedicated SDMMC peripheral (1-bit mode)
- **Partition scheme**: Huge App (3MB app / 1MB SPIFFS, no OTA)

## Features

- **Audio browser** — lists every `.mp3`/`.wav` in the SD card's root as a
  scrollable list, skipping directories and dotfiles (macOS FAT litter
  like `._x.mp3`, `.DS_Store`).
- **Playback & recording** — select a file to play it back through the
  onboard speaker, or start a new voice memo from the onboard mic,
  written straight to the card as uncompressed PCM WAV.
- **WiFi onboarding** — first boot (or after credentials are forgotten)
  opens an open captive-portal access point, `Annota-Setup`, so a phone
  or laptop can configure a network without touching the device's own
  small screen. Once a network is saved, the device reconnects to it
  directly on subsequent boots.
- **AI transcription** — select an audio file's Transcribe action to send
  it to a configurable AI provider's transcription API; the result is
  saved next to the file as `<name>.txt` and appears in the file list.
  The provider is a compile-time choice (OpenAI Whisper or Google Gemini
  — see [AI transcription provider](#ai-transcription-provider)), with
  its API key entered either on-device or via the web UI and stored in
  NVS.
- **Web file manager** — once WiFi is up, an HTTP server on port 80
  serves a file manager (list/upload/download/delete against the SD
  root) and a settings page (WiFi status, SD capacity, reconnect/forget
  WiFi, AI provider API key).

## Getting started

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code
extension).

```sh
# Build
pio run -e esp32-s3-epaper154

# Flash to a connected board
pio run -e esp32-s3-epaper154 -t upload

# Serial monitor (115200 baud)
pio device monitor
```

`esp32-s3-epaper154` is the only environment defined. There's no test
suite yet (`test/` is the stock PlatformIO placeholder).

### First run

1. Put some `.mp3`/`.wav` files in the root of a FAT-formatted SD card
   and insert it (or boot without one — the UI shows an "insert an SD
   card" prompt instead of the file list).
2. Power on. If no WiFi network is saved, the device opens the
   `Annota-Setup` access point (no password) and shows an on-screen
   dialog; connect to it from a phone or laptop and use the captive
   portal to pick a network.
3. Once connected, the on-screen WiFi status label updates and the web
   file manager comes up on the device's IP, port 80.
4. Use Next/Select to browse the file list, play/record/transcribe/delete
   an entry. To use transcription, open `/settings` in the browser and
   enter an API key for whichever provider is compiled in.

## Architecture

`src/main.cpp` wires everything together in `setup()`. `loop()` runs
`lv_timer_handler()` first, then `ui_process_input()` (the button-driven
nav below), then — only after `lv_timer_handler()` returns — the
deferred-work pumps that a nested LVGL click handler can't safely trigger
directly (`wifi_process_pending_reconnect()` and
`transcribe_process_pending()`), then `web_server_handle()`.

### `display_epaper.cpp` / `display.h`

Bridges the SSD1681-class e-paper panel and the two onboard buttons into
LVGL v9. `display_init_panel()` brings up the raw panel only.
`display_init_input()` brings up the buttons and creates the LVGL display
(no LVGL input device — button events are polled directly, see
`ui_epaper.cpp` below).

### `storage.cpp` / `storage.h`

`load_mp3_catalog()` scans the SD root into the global `mp3Files` /
`mp3FileCount` arrays, filtering out directories and dotfiles.

### `ui_epaper.cpp` / `ui.h`

A small explicit state machine (`Screen` enum) driven by
`display.h`'s `display_button_poll()`: Next cycles the current
selection/menu option, Select opens/confirms it (short press) or backs
out of it (long press). WiFi status, a scrollable file list, and
per-file Play/Record/Transcribe/Delete — deliberately no on-screen
Settings, WiFi credential entry, or text-file preview; those stay on the
web UI (see `web_server.cpp`). Every screen is rebuilt from scratch
(`lv_obj_clean()` + repopulate) on each state change rather than kept as
a tree of show/hide-toggled widgets — cheap next to the e-paper refresh
itself dominating either way. Selecting Transcribe calls `transcribe.h`'s
`transcribe_request()` directly; selecting Delete calls `storage.h`'s
`delete_file()` directly — both safe here since `ui_process_input()` runs
at `loop()`'s top level, not nested inside `lv_timer_handler()`.

### `speaker.cpp` / `speaker.h`

Owns the onboard audio hardware end to end: an ES8311 I2C codec
(`es8311.cpp`/`.h`) on a shared I2S bus, feeding an NS4150B amp for
playback and reading the codec's own mic ADC for recording. Playback
(`speaker_play()`/`speaker_process()`) decodes MP3 via ESP8266Audio or
streams WAV straight through; recording (`mic_start_recording()`/
`mic_process()`) writes plain 16-bit PCM WAV — no encoder needed. Both
share the same I2C/I2S/PA_EN hardware, so recording
always stops playback first, and both claim the SD card for their whole
duration, pumped once per `loop()` iteration via `ui_epaper.cpp`.

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
Reconnecting again from there is a separate, explicit action — the
`/settings` page's "Reconnect WiFi" button, which only calls
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
  `ui_epaper.cpp` and `web_server.cpp` only ever call those, never
  anything provider-specific.
- `transcribe.cpp` implements the provider-agnostic part:
  `transcribe_request()` / `transcribe_process_pending()` mirror
  `wifi_manager.cpp`'s `wifi_request_reconnect()` /
  `wifi_process_pending_reconnect()` pair, splitting "ask for it" (from
  `ui_epaper.cpp`'s button handler) from "actually block and repaint"
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
`sd_begin()` / `sd_end()`); `transcribe_process_pending()` brackets the
whole blocking call the same way `web_server.cpp`'s handlers do (see
below).

### `web_server.cpp` / `web_server.h`

`web_server_start()` / `web_server_handle()` run an ESP32-core
`WebServer` on port 80, started only once `wifi_connect()` succeeds.
Two pages, in the same dark palette as `ui_epaper.cpp`:

- **`/`** — a file manager (list/download/upload/delete against the SD
  root), backed by `/api/files`, `/api/download`, `/api/upload`,
  `/api/delete`.
- **`/settings`** — WiFi and clock status, SD capacity, Reconnect WiFi,
  Delete WiFi Setup, and the AI provider's API key field, labeled
  dynamically from `aiProviderName`; backed by `/api/settings` (GET, a
  status snapshot), `/api/settings/reconnect`, `/api/settings/forget`,
  and `/api/settings/ai-key` (POST).

Every handler that touches the card calls `display_suspend_touch()`
plus `storage.h`'s `sd_begin()` (releasing both afterward) — no-ops on
this board (there's no shared SPI peripheral to hand off), kept so a
future board with one wouldn't need new call sites. `/api/settings`
reports only whether a key is saved, never the key itself — the web
page can clear or overwrite it but never displays the current value.
Uploads and deletes don't refresh the on-screen MP3 list; that only
happens on reboot.

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
The API key is entered afterward, at runtime, from `/settings` — it's
stored in NVS under a key namespaced per provider (e.g. `openaiKey`) so
switching providers doesn't feed the new one a stale key saved for the
old one.

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
- `lvgl/lvgl @ 9.2.2` — the registry mirrors lvgl's git tags, which
  jump from `9.2.2` straight to `9.3.0`; `9.2.2` is what's
  config-compatible with `lv_conf.h`
- `earlephilhower/ESP8266Audio @ 2.4.1` — MP3/WAV decode for onboard
  speaker playback

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
  lv_conf.h              LVGL v9.2.0-format configuration
src/
  main.cpp                setup()/loop(), wires every module together
  display_epaper.cpp/.h   e-paper panel + button driver → LVGL bridge
  storage.cpp/.h          SD card catalog scan
  ui_epaper.cpp            LVGL screens: file list, dialogs, button nav
  speaker.cpp/.h           ES8311 codec: onboard playback + mic recording
  es8311.cpp/.h            ES8311 codec I2C register driver
  wifi_manager.cpp/.h     WiFiManager-based onboarding/reconnect
  transcribe.cpp/.h       provider-agnostic transcription request/dispatch
  transcribe_openai.cpp   OpenAI Whisper provider
  transcribe_gemini.cpp   Google Gemini provider
  web_server.cpp/.h       HTTP file manager + settings page
test/                     stock PlatformIO placeholder, unused
platformio.ini            build environment, dependencies, build flags
```

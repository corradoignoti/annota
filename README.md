# Annota

Firmware (PlatformIO, Arduino framework, C++) that turns an ESP32 board
with an SD card into a standalone MP3/voice-note browser: it scans the
card's root for audio files, lists them on an on-device UI, and can send
any of them off to an AI provider for transcription, saving the result as
a sibling `.txt` file. A WiFi connection manager handles onboarding via a
captive portal, and an HTTP file manager mirrors the on-device UI so the
SD card is reachable from a browser on the same network.

Two boards are supported, each with its own PlatformIO environment and its
own on-device UI implementation:

- **`esp32-cyd`** — ESP32-2432S028R, the "Cheap Yellow Display" (CYD): a
  2.8" ILI9341 TFT with XPT2046 resistive touch, on a generic 30-pin ESP32
  dev module. Touch-driven UI, on-device Settings (WiFi, AI provider API
  key), no onboard mic/speaker.
- **`esp32-s3-epaper154`** — Waveshare ESP32-S3-ePaper-1.54: a 1.54"
  200x200 mono e-paper panel (SSD1681-class) and 2 onboard buttons, no
  touch, ESP32-S3. Smaller button-driven UI (no on-screen Settings — use
  the web UI for that); adds onboard mic recording and speaker playback
  via an ES8311 codec.

Built against the [pioarduino](https://github.com/pioarduino/platform-espressif32)
fork of the espressif32 platform, which tracks newer `arduino-esp32` core
releases than the stock PlatformIO platform.

## Features

- **Audio browser** — lists every `.mp3`/`.wav` in the SD card's root as a
  scrollable list, skipping directories and dotfiles (macOS FAT litter
  like `._x.mp3`, `.DS_Store`).
- **WiFi onboarding** — first boot (or after credentials are forgotten)
  opens an open captive-portal access point, `Annota-Setup`, so a phone or
  laptop can configure a network without needing the device's own screen.
  Once a network is saved, the device reconnects to it directly on
  subsequent boots.
- **AI transcription** — long-press (CYD) or select (e-paper) an audio
  file to send it to a configurable AI provider's transcription API; the
  result is saved next to the file as `<name>.txt` and appears in the file
  list. The provider is a compile-time choice — OpenAI Whisper or Google
  Gemini, see [AI transcription provider](#ai-transcription-provider) —
  with its API key entered either on-device (CYD) or via the web UI and
  stored in NVS.
- **Web file manager** — once WiFi is up, an HTTP server on port 80 serves
  a file manager (list/upload/download/delete against the SD root) and a
  settings page (WiFi status, SD capacity, reconnect/forget WiFi, AI
  provider API key) mirroring the CYD's on-screen Settings view. This is
  the *only* way to manage those settings on the e-paper board.
- **Mic recording** (`esp32-s3-epaper154` only) — "+ Record new" in the
  file list uses the onboard ES8311 codec's mic to record a mono
  16kHz/16-bit `RECnnnn.wav` file straight to the SD card, no on-device
  encoder needed.
- **Speaker playback** (`esp32-s3-epaper154` only) — plays `.mp3` (via
  ESP8266Audio's `AudioGeneratorMP3`) and `.wav` files (`AudioGeneratorWAV`)
  through the onboard codec/amp.
- **Anti-burn-in** (`esp32-cyd` only) — dims/blanks the TFT backlight after
  an idle timeout; no-op on the e-paper board, which has no backlight and
  doesn't need one (see [antiburn.h](src/antiburn.h)).

## Getting started

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code
extension).

```sh
# Build
pio run -e esp32-cyd
pio run -e esp32-s3-epaper154

# Flash to a connected board
pio run -e esp32-cyd -t upload
pio run -e esp32-s3-epaper154 -t upload

# Serial monitor (115200 baud)
pio device monitor
```

There's no test suite yet (`test/` is the stock PlatformIO placeholder).

### First run

1. Put some `.mp3`/`.wav` files in the root of a FAT-formatted SD card and
   insert it (or boot without one — the UI shows an "insert an SD card"
   prompt instead of the file list).
2. Power on. If no WiFi network is saved, the device opens the
   `Annota-Setup` access point (no password) and shows an on-screen
   dialog (CYD) or waits at the WiFi-setup screen (e-paper); connect to it
   from a phone or laptop and use the captive portal to pick a network.
3. Once connected, the on-screen WiFi status updates and the web file
   manager comes up on the device's IP, port 80.
4. To use transcription, enter an API key for whichever provider is
   compiled in — on-device Settings on the CYD, or `/settings` in the
   browser on either board.

## Architecture

`src/main.cpp` wires everything together in `setup()`, in an order that
matters on the CYD — see [Shared SPI peripheral](#shared-spi-peripheral-esp32-cyd-only).
`loop()` runs `lv_timer_handler()` first, then `ui_process_input()` (the
e-paper board's button-driven navigation; a no-op on the CYD, where input
flows through LVGL's touch indev instead), then — only after those return
— the deferred-work pumps a nested LVGL click handler can't safely trigger
directly (`wifi_process_pending_reconnect()` and
`transcribe_process_pending()`), then `web_server_handle()` and
`antiburn_process()`.

### Board variants

Selected by exactly one `BOARD_*` build flag per PlatformIO environment
(`storage.cpp` `#error`s at compile time if neither is defined):
`esp32-cyd` defines `BOARD_CYD=1`, `esp32-s3-epaper154` defines
`BOARD_ESP32S3_EPAPER154=1`. `display.h`/`ui.h` are the shared interfaces;
which board-specific file implements each is picked per environment by
`platformio.ini`'s `build_src_filter` (`display.cpp` vs
`display_epaper.cpp`, `ui.cpp` vs `ui_epaper.cpp`) rather than an
`#ifdef`-wrapped single file — those two files `#include` libraries
(TFT_eSPI/XPT2046, ESP8266Audio/I2S) that only exist in one board's
`lib_deps`, and PlatformIO's library dependency finder does a textual
`#include` scan that doesn't evaluate `#ifdef`/`#endif`, so it would fail
resolving those headers if the other environment tried to compile the
file too. Files with only a small, genuinely-shared-dependency difference
between boards (`storage.cpp`'s SD-over-SPI vs SD-over-SDMMC;
`antiburn.cpp`'s CYD-backlight-only blanking; `speaker.cpp`'s CYD no-op
stubs) keep the single-file-with-`#ifdef` pattern instead.

### `display.cpp` (esp32-cyd) / `display_epaper.cpp` (esp32-s3-epaper154)

Both implement the shared `display.h` interface, bridged into LVGL v9.

- **CYD**: TFT_eSPI panel + XPT2046 touch. `display_init_panel()` brings
  up the raw TFT_eSPI panel only; `display_init_input()` brings up touch
  (one-time two-corner calibration, cached in NVS) and creates the LVGL
  display + input device.
- **e-paper**: an SSD1681-class panel driver (ported from Waveshare's own
  example repo) plus the two onboard buttons (`display_button_poll()`).
  `disp_flush_cb` renders LVGL's normal RGB565 framebuffer
  (`LV_DISPLAY_RENDER_MODE_FULL` — the controller has no concept of a
  sub-rect update, so it always sees the whole 200x200 screen at once) and
  thresholds it to 1bpp on the way out. No touch, no backlight, no shared
  SPI peripheral to hand off, so `display_suspend_touch()` /
  `display_resume_touch()` / `display_set_backlight()` are no-op stubs
  here — their CYD-only callers don't need their own board `#ifdef`.

### `storage.cpp` / `storage.h`

One file for both boards — only `sd_begin()`/`sd_end()` and the pin
defines differ per board (SD-over-SPI on the CYD vs SD-over-SDMMC 1-bit
mode on the e-paper board's dedicated peripheral). `load_mp3_catalog()`
scans the SD root into the global `mp3Files`/`mp3FileCount` arrays,
filtering out directories and dotfiles; `load_file_catalog()` is the
generic form behind it, used by the audio/text list toggle.

### `ui.cpp` (esp32-cyd) / `ui_epaper.cpp` (esp32-s3-epaper154)

Both implement the shared `ui.h` interface.

- **CYD**: `build_main_screen(sd_present)` builds the whole
  touch-driven screen — file list (or "insert an SD card" prompt), WiFi
  status label, and `ui_show_wifi_setup_dialog()` /
  `ui_hide_wifi_setup_dialog()` (a modal on `lv_layer_top()`). The
  Settings view holds the API key field for whichever AI provider is
  compiled in, labeled dynamically via `ai_provider_name()`, with an
  `lv_keyboard` created lazily on first focus. Long-pressing an audio card
  (only while the list shows `.mp3`/`.wav`, not `.txt` transcripts) opens
  a Yes/No confirm dialog; Yes calls `transcribe_request()` rather than
  blocking right there, so the actual blocking call happens later, outside
  the LVGL click handler (see `wifi_manager.cpp` below for why).
  `ui_show_transcribe_progress()` / `ui_show_transcribe_result()` are what
  `transcribe_process_pending()` calls back into once it's actually safe
  to block and repaint.
- **e-paper**: deliberately smaller in scope — WiFi status, a scrollable
  file list with a synthetic "+ Record new" row, and per-file
  Transcribe/Delete/Play — no on-screen Settings, WiFi credential entry,
  or text-file preview (all handled by the web UI instead, which works
  unchanged on this board too). A small explicit state machine (`Screen`
  enum) driven by `display_button_poll()` via `ui_process_input()`, rebuilt
  from scratch on each state change rather than kept as a tree of
  show/hide-toggled widgets. Next cycles the current selection; Select
  opens/confirms (short press) or backs out (long press). Selecting
  Transcribe/Record/Delete calls straight into `transcribe_request()` /
  `mic_start_recording()` / `delete_file()` rather than through a confirm
  dialog — safe here since `ui_process_input()` runs at `loop()`'s top
  level, never nested inside `lv_timer_handler()`.

### `wifi_manager.cpp` / `wifi_manager.h`

`wifi_connect()`, built on tzapu/WiFiManager, has two paths:

- **No network saved** — opens the `Annota-Setup` captive-portal AP (no
  password, no timeout) and shows the on-screen dialog, blocking until a
  network is configured from a phone or laptop.
- **Network saved** — reconnects directly (no portal), for up to 30
  seconds.

WiFiManager's "saved" check reads ESP-IDF's own NVS station config, which
can be stale — left over from a different sketch ever flashed to the
board — rather than something actually set up here. A timeout on that
path wipes the saved credentials and falls back to the same setup portal,
instead of stranding the device offline with no way back to configuring
an AP. Only if *that* fallback portal is itself exited without connecting
does it show a warning dialog and let the device run offline; reconnecting
again from there is a separate, explicit action (the CYD's Settings
"Reconnect WiFi" button, or `/api/settings/reconnect` on the web UI),
which only calls `wifi_request_reconnect()` — it fires from an LVGL click
handler already nested inside `lv_timer_handler()`, which refuses to run
itself again while it's running, so the actual retry can't happen there.
`loop()` picks up the request via `wifi_process_pending_reconnect()`,
called right after `lv_timer_handler()` returns, never nested inside it.

Must be called after `build_main_screen()` so it has a screen to paint
status onto.

### `transcribe.cpp` / `transcribe.h` + `transcribe_<provider>.cpp`

AI transcription, split into a provider-agnostic half and a
provider-specific half so adding a future provider is a new file plus a
new build flag, not a rewrite.

- `transcribe.h` declares the whole public surface with generic names
  only (`ai_provider_name()`, `ai_provider_has_api_key()` /
  `..._get_api_key()` / `..._set_api_key()`, `ai_transcribe_file()`).
  `ui.cpp`/`ui_epaper.cpp` and `web_server.cpp` only ever call those,
  never anything provider-specific.
- `transcribe.cpp` implements the provider-agnostic part:
  `transcribe_request()` / `transcribe_process_pending()` mirror
  `wifi_manager.cpp`'s `wifi_request_reconnect()` /
  `wifi_process_pending_reconnect()` pair, splitting "ask for it" from
  "actually block and repaint", for the same reentrancy reason. It
  `#error`s at compile time if no `AI_PROVIDER_*` build flag is defined,
  so a missing one fails loudly here instead of as a confusing link
  error.
- Exactly one `transcribe_<provider>.cpp` implements the rest
  (`ai_provider_name()` and `ai_transcribe_file()`). Each file's entire
  body is wrapped in `#ifdef AI_PROVIDER_<NAME>`, so every provider file
  can sit in `src/` at once and only the one selected by
  `platformio.ini`'s build flags compiles to anything. NVS keys are
  namespaced per provider (e.g. `openaiKey`) so switching the compiled-in
  provider doesn't feed it a stale key saved for a different one.

Two providers exist today:

- **`transcribe_openai.cpp`** (OpenAI Whisper, `whisper-1`, via
  `/v1/audio/transcriptions`) streams the upload straight off the SD card
  through a custom `Stream` subclass wrapping the multipart
  preamble/file/trailer — the ESP32 doesn't have enough RAM to buffer a
  whole audio file first — and skips TLS certificate validation
  (`WiFiClientSecure::setInsecure()`); no root-CA bundle exists in this
  project.
- **`transcribe_gemini.cpp`** implements the same interface for Google
  Gemini.

`ai_transcribe_file()` claims the SD card itself (`storage.h`'s
`sd_begin()`/`sd_end()`) but leaves pausing/resuming touch to the caller —
`transcribe_process_pending()` does that around the whole blocking call
(a no-op on the e-paper board, see above).

### `speaker.cpp` / `speaker.h` + `es8311.cpp` / `es8311.h` (esp32-s3-epaper154 only)

Owns the e-paper board's onboard audio hardware end to end: an ES8311 I2C
codec on a shared I2S bus, feeding an NS4150B amp for playback and reading
the codec's own mic ADC for recording. Playback (`speaker_*`) and
recording (`mic_*`) live in one module because they're the same physical
peripherals (one I2C bus, one I2S controller, one power rail) taking
turns, never both at once — `mic_start_recording()` always stops playback
first. Recording writes plain mono 16kHz/16-bit PCM WAV directly
(`write_wav_header()`/`patch_wav_header()`), no encoder library needed;
`speaker_play()` picks `AudioGeneratorMP3` or `AudioGeneratorWAV`
(ESP8266Audio) by file extension. `es8311.cpp` is the codec's register-map
driver (I2C only — the I2S side is `speaker.cpp`'s job), ported from
Espressif's own `esp-bsp` driver. Both files are excluded from the CYD's
build (it has no such hardware); `speaker.h` gives `esp32-cyd` inline
no-op stubs instead, so `ui_epaper.cpp` never needs its own board
`#ifdef`.

### `web_server.cpp` / `web_server.h`

`web_server_start()`/`web_server_handle()` run an ESP32-core `WebServer`
on port 80, started only once `wifi_connect()` succeeds. Two pages, same
dark palette as the on-device UI:

- **`/`** — a file manager (list/download/upload/delete against the SD
  root), backed by `/api/files`, `/api/download`, `/api/upload`,
  `/api/delete`.
- **`/settings`** — WiFi and clock status, SD capacity, Reconnect WiFi,
  Delete WiFi Setup, and the AI provider's API key field (labeled
  dynamically), backed by `/api/settings` (GET, a status snapshot),
  `/api/settings/reconnect`, `/api/settings/forget`, and
  `/api/settings/ai-key` (POST).

Every handler that touches the card calls `display_suspend_touch()` plus
`storage.h`'s `sd_begin()` (releasing both afterward) to borrow the shared
SPI peripheral from touch for that one request (a no-op on the e-paper
board) — the AI key handler is the one exception, since
`ai_provider_set_api_key()` is pure NVS and never touches the SD card.
This server is plain HTTP, so `/api/settings` reports only whether a key
is saved, never the key itself. Uploads and deletes don't refresh the
on-device file list; that only happens on reboot.

## Shared SPI peripheral (esp32-cyd only)

The e-paper board has no equivalent constraint: its panel is on its own
dedicated `SPI2_HOST` and its SD card on the ESP32-S3's dedicated SDMMC
peripheral (1-bit mode) — two peripherals, neither shared with anything.

The ESP32 (CYD) has only two general-purpose SPI peripherals. The display
panel (`TFT_MISO`/`MOSI`/`SCLK` in `include/User_Setup.h`) occupies one
full-time. Touch (XPT2046, pins 25/32/39) and the SD card (pins 18/19/23)
are wired to **separate SPI buses from each other too**, despite both
being the "spare" peripheral — they have to take turns on it.

That's why `load_mp3_catalog()` must run *before* `display_init_input()`
in `setup()`: SD's `SPIClass` is released (`SD.end()`/`sdSPI.end()`)
before touch claims the same peripheral. Don't reorder `main.cpp`'s
`setup()` without preserving that.

Once touch owns the peripheral for good, `web_server.cpp`'s handlers
pause touch (`display_suspend_touch()`), borrow the bus for SD
(`storage.h`'s `sd_begin()`/`sd_end()`), then resume touch
(`display_resume_touch()`) — the touchscreen goes unresponsive for the
duration of whatever SD request is in flight. `transcribe.cpp`'s
`transcribe_process_pending()` does the same thing around its own
(much longer-running) `ai_transcribe_file()` call. Any new code that
touches the SD card after boot needs the same pause/claim/release/resume
dance.

## Configuration

### AI transcription provider

Selected at compile time in `platformio.ini`'s `build_flags`, per
environment — exactly one `AI_PROVIDER_*` flag must be defined:

```ini
-D AI_PROVIDER_OPENAI=1
; or
-D AI_PROVIDER_GEMINI=1
```

Swap the flag for a different provider instead of adding a second one.
The API key is entered afterward, at runtime — on-device Settings on the
CYD, or `/settings` on either board — and stored in NVS under a key
namespaced per provider (e.g. `openaiKey`) so switching providers doesn't
feed the new one a stale key saved for the old one.

### TFT_eSPI panel (esp32-cyd)

`include/User_Setup.h` is force-included ahead of every translation unit
via `-include include/User_Setup.h` in `platformio.ini` (TFT_eSPI's own
`User_Setup_Select.h` is skipped). Pin and driver changes go there, not
in a new file.

### LVGL

`include/lv_conf.h` is shared by both environments (both set
`-D LV_CONF_INCLUDE_SIMPLE=1`) and force-includes `include/` onto the
search path. `LV_USE_TFT_ESPI` is itself gated off on
`esp32-s3-epaper154` for the same LDF reason as the board-variant files
above. The pinned lvgl version (9.2.2) is a config-compatible match for
this v9.2.0-format `lv_conf.h` — don't bump it without checking that.

## Dependencies

Declared in `platformio.ini`, per environment:

**Both boards**: `bblanchon/ArduinoJson @ 7.4.1`, `tzapu/WiFiManager @ 2.0.17`,
`lvgl/lvgl @ 9.2.2` (the registry mirrors lvgl's git tags, which jump from
`9.2.2` straight to `9.3.0`; `9.2.2` is what's config-compatible with
`lv_conf.h`).

**`esp32-cyd` only**: `bodmer/TFT_eSPI @ 2.5.43`;
`PaulStoffregen/XPT2046_Touchscreen` pinned to tag `v1.4`, pulled straight
from GitHub (the PlatformIO registry only mirrors an untagged
`0.0.0-alpha` build that version-pin syntax can't resolve).

**`esp32-s3-epaper154` only**: `earlephilhower/ESP8266Audio @ 2.4.1`
(MP3/WAV decode for speaker playback).

The platform itself is the `pioarduino` fork of `espressif32`, tracking
newer `arduino-esp32` core releases than the stock PlatformIO platform.

## Known gotcha: case-insensitive filesystems

This repo is developed on macOS's default case-insensitive filesystem.
`#include <WiFi.h>` (the Arduino core header) will silently resolve to a
project file named `wifi.h`/`wifi.cpp` sitting in `-Isrc`, breaking the
build in confusing ways. That's why the WiFi module here is named
`wifi_manager.*`, not `wifi.*` — keep that naming if you touch it.

## Repository layout

```
include/
  User_Setup.h      TFT_eSPI pin/driver config (esp32-cyd), force-included project-wide
  lv_conf.h         LVGL v9.2.0-format configuration, shared by both boards
src/
  main.cpp                setup()/loop(), wires every module together
  display.cpp/.h          TFT_eSPI + XPT2046 -> LVGL bridge (esp32-cyd)
  display_epaper.cpp      SSD1681 e-paper -> LVGL bridge + buttons (esp32-s3-epaper154)
  storage.cpp/.h          SD card catalog scan (both boards)
  ui.cpp/.h                touch-driven LVGL screens (esp32-cyd)
  ui_epaper.cpp            button-driven screen state machine (esp32-s3-epaper154)
  wifi_manager.cpp/.h     WiFiManager-based onboarding/reconnect (both boards)
  transcribe.cpp/.h       provider-agnostic transcription request/dispatch (both boards)
  transcribe_openai.cpp   OpenAI Whisper provider
  transcribe_gemini.cpp   Google Gemini provider
  speaker.cpp/.h          onboard mic/speaker via ES8311 (esp32-s3-epaper154)
  es8311.cpp/.h           ES8311 codec register driver (esp32-s3-epaper154)
  antiburn.cpp/.h         backlight idle blanking (esp32-cyd)
  web_server.cpp/.h       HTTP file manager + settings page (both boards)
test/                     stock PlatformIO placeholder, unused
platformio.ini            build environments, dependencies, build flags
```
</content>

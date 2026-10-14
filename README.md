# Annota

Firmware for two Waveshare ESP32-S3 e-paper boards — the
ESP32-S3-ePaper-1.54 (1.54" 200x200 panel, 2 buttons) and the
ESP32-S3-ePaper-3.97 (3.97" 800x480 panel used in portrait, rotary switch +
BOOT) — each with an onboard ES8311 speaker/mic codec. It turns the board
into a pocket voice-note recorder: record memos from the onboard mic, browse
and play the SD card's MP3/WAV notes on a button-driven LVGL UI, and send any
of them to an AI provider for transcription, saved as a sibling `.txt` with an
AI title and abstract on top. A web file manager mirrors the device's Notes
list from any browser on the network, and the whole SD card can be mounted on
a computer as a USB drive.

User documentation: [docs/user-manual.md](docs/user-manual.md) (also
published as `user-manual.html` on the project site, alongside the browser
flasher). Architecture notes for contributors live in
[CLAUDE.md](CLAUDE.md).

Built with PlatformIO (Arduino framework, C++), against the
[pioarduino](https://github.com/pioarduino/platform-espressif32) fork
of the espressif32 platform.

## Hardware

One PlatformIO environment per board; pins live in `src/board.h`.

| | `esp32-s3-epaper154` | `esp32-s3-epaper397` |
|---|---|---|
| Board | Waveshare ESP32-S3-ePaper-1.54 | Waveshare ESP32-S3-ePaper-3.97 |
| Display | 1.54" 200x200 mono, SSD1681-class | 3.97" 800x480 mono, SSD1677-class, used in portrait (480x800) |
| Input | BOOT/GPIO0 "Next", PWR/GPIO18 "Select" | rotary Down "Next", Up "Previous", Press "Select", BOOT "Back" |
| Power | GPIO17 latch, battery on ADC GPIO4 | AXP2101 PMIC (battery gauge over I2C) |
| Storage | microSD, SDMMC 1-bit | microSD, SDMMC 4-bit |
| Flash | 8MB | 16MB |

Both: no touch; onboard ES8311 I2C codec + NS4150B amp (speaker) and mic
ADC over a shared I2S bus; Huge App partition scheme (3MB app / 1MB
SPIFFS, no OTA).

On the 3.97, Up/Down move through lists and menus, Press opens/confirms
(long press backs out, as on the 1.54), BOOT goes back (from the file
list: to Home). Hold Press + BOOT for 5 s to reboot; press the knob to
wake from idle sleep.

## Features

- **Notes list** — every `.mp3`/`.wav` in the SD card's root (directories
  and dotfiles skipped), untranscribed first, then by transcript date, each
  shown under its transcript's AI title.
- **Playback & recording** — play a note through the onboard speaker, or
  record a new one (`RECnnnn.wav`, 16-bit PCM). 3.97: Low/Medium/High
  volume menu during playback.
- **AI transcription** — on-device (OpenAI Whisper by default, Gemini as a
  compile-time alternative), with step-by-step progress and a
  `<name>_error.txt` diagnostic log on failure. The OpenAI provider adds a
  `gpt-4o-mini` title and abstract above the transcript and verifies
  api.openai.com's certificate against embedded root CAs. On-device uploads
  are capped at 2 MB (1.54) / 4 MB (3.97); the web UI has no cap.
- **Web file manager** — Notes table with transcripts folded in (play,
  transcribe, view, download, delete, batch actions, a Sync button that
  transcribes everything still pending — browser-side, straight to the AI
  provider), an Other files table, a Settings page (language, WiFi
  networks, idle timeout, API key) and an About page. No browser upload.
- **USB drive mode** — Home → USB drive exposes the whole SD card as USB
  mass storage; ejecting it reboots the device.
- **WiFi on demand** — captive-portal setup (`Annota-Setup`) on first
  boot, up to 5 saved networks, radio off by default and only brought up
  for a transcription, File transfer, or a manual Online. On-device
  scan-and-join and a standalone `Annota-AP` access point when no saved
  network is in range.
- **Idle deep sleep** — 1–180 min (default 30), logo sleep screen (3.97:
  plus notes/battery/SD summary), peripherals powered down first.
- **English, Italian, French** — one device-wide language for the e-paper
  UI and the web pages.
- **Reboot gesture** — hold both buttons 5 s (1.54: Next + Select; 3.97:
  Press + BOOT), handled on core 0 so it works even when the UI is stuck.

## Getting started

### Flash a release from the browser

Each GitHub release ships `firmware.bin` (1.54) and
`firmware-epaper397.bin`, plus ESP Web Tools manifests and the
bootloader/partition images `tools/package_release.sh` packages for the
project site's browser flasher (`flash.html`, desktop Chrome or Edge).

### Build from source

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code
extension).

```sh
# Build (pick the env for your board: esp32-s3-epaper154 or esp32-s3-epaper397)
pio run -e esp32-s3-epaper154

# Flash to a connected board
pio run -e esp32-s3-epaper154 -t upload

# Serial monitor (115200 baud)
pio device monitor
```

Build both envs after touching shared code. There's no test suite yet
(`test/` is the stock PlatformIO placeholder).

### First run

1. Put some `.mp3`/`.wav` files in the root of a FAT32 SD card and insert
   it (without one the UI shows an "insert an SD card" prompt).
2. Power on. With no WiFi network saved, the device opens the
   `Annota-Setup` access point (no password); connect from a phone or
   laptop and pick a network in the captive portal — or skip it to work
   offline.
3. From then on WiFi stays off until needed. Home → File transfer
   connects and shows the web file manager's address (text + QR code).
4. Open `/settings` there and enter the AI provider's API key, then
   Transcribe from a note's action menu on the device or from the web page.

See the [user manual](docs/user-manual.md) for every screen and control.

## Configuration

### AI transcription provider

Selected at compile time in `platformio.ini`'s `build_flags` — exactly
one `AI_PROVIDER_*` flag must be defined:

```ini
-D AI_PROVIDER_OPENAI=1
; or
-D AI_PROVIDER_GEMINI=1
```

Swap the flag instead of adding a second one. The API key is entered at
runtime from `/settings` and stored in NVS under a key namespaced per
provider (e.g. `openaiKey`), so switching providers doesn't feed the new
one a stale key.

### On-device transcription cap

`TRANSCRIBE_MAX_FILE_BYTES`, set per env in `platformio.ini` (2 MB on the
1.54, 4 MB on the 3.97). Bigger notes are refused on the device with a
pointer to the web UI's Transcribe button.

### LVGL

`include/lv_conf.h` is pulled in via `-D LV_CONF_INCLUDE_SIMPLE=1`
(`include/` is added to the search path explicitly, since PlatformIO
doesn't do that for `lib_deps` sources like lvgl itself). The pinned lvgl
version (9.2.2) is a config-compatible match for this v9.2.0-format
`lv_conf.h` — don't bump it without checking that. `src/fonts/` holds
Montserrat rebuilds with Latin-1 accents (the built-in LVGL fonts are
ASCII only).

## Translations

Every user-visible string lives in `src/i18n_strings.def` with English,
Italian and French columns (`TR` rows for the e-paper UI, `TRW` rows for
the web pages). Run `python3 tools/check_i18n.py` before committing; CI
runs it too.

## Releases

Fully automated by semantic-release on every merge to `main`: Conventional
Commit messages pick the version, `include/version.h` is bumped, both envs
are built, and a GitHub Release is published with one firmware image per
board plus the browser-flasher assets. Never bump versions or tags by hand.

## Dependencies

Declared in `platformio.ini`:

- `bblanchon/ArduinoJson @ 7.4.1`
- `tzapu/WiFiManager @ 2.0.17` — captive-portal setup
- `lvgl/lvgl @ 9.2.2` — the registry mirrors lvgl's git tags, which jump
  from `9.2.2` straight to `9.3.0`; `9.2.2` is what's config-compatible
  with `lv_conf.h`
- `earlephilhower/ESP8266Audio @ 2.4.1` — MP3/WAV decode for playback
- `ricmoo/QRCode @ 0.0.1` — on-screen QR codes
- `lewisxhe/XPowersLib @ 0.3.3` — AXP2101 PMIC (3.97 only)

The platform is the `pioarduino` fork of `espressif32`, tracking newer
`arduino-esp32` core releases than the stock PlatformIO platform.

## Known gotcha: case-insensitive filesystems

On macOS's default case-insensitive filesystem, `#include <WiFi.h>` (the
Arduino core header) silently resolves to a project file named `wifi.h`
in `-Isrc`. That's why the WiFi module is named `wifi_manager.*` — keep
that naming if you touch it.

## Repository layout

```
include/
  lv_conf.h               LVGL v9.2.0-format configuration
  version.h               FW_VERSION, bumped by semantic-release
  fonts_it.h              declarations for src/fonts/
src/
  main.cpp                setup()/loop(), wires every module together
  board.h                 per-board pins and capabilities (BOARD_* flags)
  display.h               panel + button interface shared by both boards
  display_epaper.cpp      1.54 e-paper panel driver -> LVGL bridge
  display_epaper397.cpp   3.97 e-paper panel driver -> LVGL bridge
  display_buttons.cpp     button/rotary polling (both boards)
  ui_epaper.cpp, ui.h     LVGL screens and button-driven state machine
  storage.cpp/.h          SD card mount, Notes catalog, file helpers
  speaker.cpp/.h          ES8311 codec: playback + mic recording
  es8311.cpp/.h           ES8311 I2C register driver
  battery.cpp/.h          battery ADC (1.54) / AXP2101 PMIC (3.97)
  sleep.cpp/.h            idle deep sleep and its timeout setting
  reboot_combo.cpp/.h     hold-both-buttons reboot task
  usb_drive.cpp/.h        SD card as USB mass storage
  wifi_manager.cpp/.h     WiFi setup portal, saved networks, on-demand connect
  transcribe.cpp/.h       provider-agnostic transcription request/progress
  transcribe_openai.cpp   OpenAI Whisper + title/abstract provider
  openai_ca.h             embedded root CAs for api.openai.com
  transcribe_gemini.cpp   Google Gemini provider
  web_server.cpp/.h       HTTP file manager, settings and about pages
  i18n.cpp/.h             language setting and lookups
  i18n_strings.def        EN/IT/FR string table
  fonts/                  Montserrat with Latin-1 accents
docs/                     project site pages and user manual
tools/
  check_i18n.py           translation checker (also run in CI)
  package_release.sh      browser-flasher assets for each release
platformio.ini            one env per board, dependencies, build flags
```

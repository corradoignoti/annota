# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO/Arduino, C++) for the Waveshare ESP32-S3-ePaper-1.54
— a 1.54" 200x200 mono e-paper panel + 2 onboard buttons + an onboard
ES8311 speaker/mic codec, ESP32-S3, no touch (see `platformio.ini`'s
`esp32-s3-epaper154` env). It's an MP3/WAV file browser: scans an SD
card's root for audio files and lists them on an LVGL UI, with a WiFi
connection manager (captive-portal setup) and an HTTP file manager for the
SD card alongside. Selecting an audio file offers to transcribe it via an
AI provider's API (OpenAI by default, selected at compile time — see
`transcribe.cpp/h` below), saving the result as a sibling `.txt` file; it
can also be played back or deleted, and a new voice memo can be recorded
straight from the on-device UI (see `speaker.cpp/h`).

## Commands

Build (`esp32-s3-epaper154` is the only environment defined):
```
pio run -e esp32-s3-epaper154
```
Flash to a connected board:
```
pio run -e esp32-s3-epaper154 -t upload
```
Serial monitor (115200 baud, set in platformio.ini):
```
pio device monitor
```
No test suite exists yet (`test/` is the stock PlatformIO placeholder).

## Architecture

`src/main.cpp` wires the modules together in `setup()`. `loop()` calls
`lv_timer_handler()` first, then `ui_process_input()` (button-driven nav —
see its bullet below), then, only after `lv_timer_handler()` has returned,
the deferred-work pumps that a nested LVGL click handler can't safely
trigger directly — `wifi_process_pending_reconnect()`, `wifi_process_boot_connect()`
(the background boot-time connect `setup()` kicks off — see
`wifi_manager.cpp/h` below) and `transcribe_process_pending()` (see their
bullets below) — then `web_server_handle()`, then `sleep_process_idle()`
(see `sleep.cpp/h` below) last, once everything else that could count as
activity this pass has had a chance to reset its clock.

- **sleep.cpp/h** — idle-timeout deep sleep, ported from the pala_note
  sibling project's `enterUltraSleep()`/`resetActivity()` (same board
  family: same `PWR_HOLD_PIN` battery latch, same battery ADC pin, same
  button GPIOs, so the same approach applies unchanged).
  `sleep_process_idle()`, called last in `loop()`, deep-sleeps
  (`esp_deep_sleep_start()`, ext1 wakeup armed on the Select/PWR button
  only, `ESP_EXT1_WAKEUP_ANY_LOW` — BOOT/Next deliberately left out of the
  mask so the sleep screen's "Hold Select to wake" stays true) once
  `sleep_get_idle_timeout_minutes()` (default 30, persisted in NVS,
  clamped to 1–180 — see `IDLE_TIMEOUT_MIN_DEFAULT`/`_MIN`/`_MAX` in
  sleep.cpp) have passed with no activity, unless `ui.h`'s
  `ui_is_sleep_blocked()`
  says a foreground operation (recording/playing/transcribing) is in
  progress, or `web_server.h`'s `web_transcribe_in_progress()` says a
  browser-initiated transcription is in flight — that flow runs entirely
  between the browser and the AI provider (see web_server.cpp's bullet
  below), with no request landing on this device for the whole duration,
  so it needs its own explicit guard rather than relying on request
  traffic; it self-clears on a safety-net timeout if the browser never
  calls back. `sleep_reset_activity()` is called from `main.cpp`'s
  `setup()` (starts the clock at boot) and from two activity sources:
  `ui_epaper.cpp`'s `ui_process_input()` on any onboard button edge, and
  `web_server.cpp`'s route registrations (each wrapped in a
  `with_activity()` helper) on any served HTTP request — so the device
  won't deep-sleep out from under someone actively browsing, uploading to,
  or downloading from the web file manager just because no button was
  pressed. Waking from deep sleep is a full MCU reset — `setup()` runs
  again from scratch like a fresh boot, so there's no wake-cause branching
  here (unlike pala_note, which distinguishes which button woke it); the
  normal boot path already re-scans the SD card, reconnects WiFi, and
  rebuilds the main screen. `sleep_get_idle_timeout_minutes()`/
  `sleep_set_idle_timeout_minutes()` read/persist the timeout itself
  (NVS Preferences, namespace `"annota"` — same as
  `transcribe_openai.cpp`'s API key), lazily loaded once and cached
  after that; `web_server.cpp`'s Settings page exposes it as a slider
  (GET `/api/settings`'s `idleTimeoutMinutes`, POST
  `/api/settings/idle-timeout`), takes effect on the very next
  `sleep_process_idle()` call, no reboot needed.

- **display_epaper.cpp** (`display.h`'s implementation) — an SSD1681-class
  e-paper panel driver (command/LUT sequence ported from Waveshare's own
  example repo, waveshareteam/ESP32-S3-ePaper-1.54) bridged into LVGL v9,
  plus the two onboard buttons (`display_button_poll()`, declared in
  `display.h`). `disp_flush_cb` renders LVGL's normal RGB565 framebuffer
  (`LV_DISPLAY_RENDER_MODE_FULL`, so it always sees the whole 200x200
  screen in one call — there's no such thing as updating a sub-rect on
  this controller) and thresholds it to 1bpp on the way out, rather than
  switching `lv_conf.h` to a monochrome color depth. No touch, no
  backlight, no shared-SPI peripheral to hand off —
  `display_suspend_touch()`/`display_resume_touch()` are no-op stubs so
  their callers (`web_server.cpp`, `transcribe.cpp`) don't need a special
  case around their SD access.
- **storage.cpp/h** — `load_mp3_catalog()` scans the SD root into the global
  `mp3Files`/`mp3FileCount` arrays (`storage.h`), filtering directories and
  dotfiles (macOS FAT litter like `._x.mp3`, `.DS_Store`). `sd_begin()`/
  `sd_end()` mount/unmount the card over the ESP32-S3's dedicated SDMMC
  peripheral (1-bit mode, pins 39/41/40).
- **ui_epaper.cpp** (`ui.h`'s implementation) — WiFi status, a scrollable
  file list, and per-file Play/Record/Transcribe/Delete — no on-screen
  Settings, WiFi credential entry, or text-file preview, all of which stay
  on `web_server.cpp`'s existing web UI. A small explicit state machine
  (`Screen` enum) driven by `display.h`'s `display_button_poll()` via
  `ui_process_input()`: Next cycles the current selection/menu option,
  Select opens/confirms (short press) or backs out (long press). Every
  screen is rebuilt from scratch (`lv_obj_clean()` + repopulate) on each
  state change rather than kept as a tree of show/hide-toggled widgets —
  cheap next to the e-paper refresh itself dominating either way.
  Selecting Transcribe calls `transcribe.h`'s `transcribe_request()`
  directly rather than through a confirm dialog — safe here since
  `ui_process_input()` runs at `loop()`'s top level, not nested inside
  `lv_timer_handler()`; selecting Delete calls `storage.h`'s
  `delete_file()` directly, same reasoning. A long Select press on the
  list opens a small Refresh/Offline↔Online/Reboot/Close menu instead —
  Offline↔Online is driven by two new `wifi_manager.h` calls,
  `wifi_is_connected()` (labels the option) and `wifi_go_offline()`
  (drops the AP association without touching the saved NVS network or
  powering off the radio, unlike `wifi_forget_and_reboot()` — see that
  function's comment on why); switching back online reuses the existing
  `wifi_request_reconnect()`. Reboot goes through the same
  confirm-then-act pattern as Delete/Forget-WiFi before calling
  `ESP.restart()`.
- **speaker.cpp/h** — owns the onboard audio hardware end to end: an
  ES8311 I2C codec (`es8311.cpp/h`) on a shared I2S bus, feeding an
  NS4150B amp for playback (`speaker_play()`/`speaker_process()`, decoding
  MP3 via ESP8266Audio or streaming WAV straight through) and reading the
  codec's own mic ADC for recording (`mic_start_recording()`/
  `mic_process()`, writing plain 16-bit PCM WAV — no encoder, no working
  set to allocate). Playback and
  recording share one module since they're the same physical peripherals
  (one I2C bus, one I2S controller, one PA_EN power rail) taking turns,
  never both at once — `mic_start_recording()` always stops playback
  first. Both claim the SD card for their whole duration (`storage.h`'s
  `sd_begin()`/`sd_end()`) and must be pumped every `loop()` iteration via
  `ui_epaper.cpp`'s `ui_process_input()`.
- **wifi_manager.cpp/h** — tzapu/WiFiManager underneath. Two paths at
  boot depending on whether a network is already saved in NVS: none saved
  opens a captive portal AP ("Annota-Setup", no password) with no timeout
  and shows the on-screen dialog, blocking `setup()` until the user
  configures one from a phone/laptop (`wifi_start_boot_connect()` returns
  false once that's resolved either way — nothing left to poll); one
  saved instead kicks off `WiFi.begin()` and returns immediately, true,
  so `setup()` can build the UI and start the SD scan without the screen
  sitting frozen for however long the router takes to answer.
  `wifi_process_boot_connect()`, called from `loop()` right after
  `wifi_process_pending_reconnect()`, polls that reconnect to completion —
  up to `WIFI_RECONNECT_TIMEOUT_SECONDS` (wifi_manager.cpp, currently 10s)
  — and, on success, is what `main.cpp` calls `web_server_start()` off of.
  The setup portal never reappears on its own once a network is saved — a
  reconnect timeout just leaves the device offline, said only via the
  header status line (`ui_set_wifi_status()`) rather than a modal, so
  whatever's on screen (the file list, say) isn't interrupted — rather
  than wiping the saved credentials, since WiFiManager's "saved" check
  reading stale NVS state isn't reason enough to drop the user back into
  AP setup out from under them. The device stays fully usable offline —
  record, delete, and preview all work with no network — and this same
  logic runs unchanged on every boot, including a deep-sleep wakeup
  (waking is a full MCU reset, see `sleep.cpp/h` above, so there's no
  separate wake-time path). The only way back to the setup portal is the
  explicit, irreversible "Delete WiFi Setup" button
  (`wifi_forget_and_reboot()`); a plain reconnect retry is the Settings
  page's "Reconnect WiFi" button, which only calls
  `wifi_request_reconnect()` (it fires from an LVGL click handler already
  nested inside `lv_timer_handler()`, which refuses to run itself again
  while it's running — so the actual retry can't happen there); `loop()`
  picks up the request via `wifi_process_pending_reconnect()`, which does
  block (unlike the boot-time connect, this is a wait the user explicitly
  asked for by pressing the button) until it connects or times out. Must
  be called after `build_main_screen()` so it has a screen to paint
  status onto. `wifi_ensure_connected()` is a third entry point: a
  no-portal, single blocking reconnect attempt, used by
  `transcribe.cpp`'s `transcribe_process_pending()` to retry the saved
  network before a transcription rather than failing outright just
  because the device booted offline. `wifi_process_periodic_check()`,
  called from `loop()` alongside the other `wifi_process_*()` functions,
  is a fourth: every 15 minutes (`WIFI_HEALTH_CHECK_INTERVAL_MS`), if the
  radio's on but not connected — the AP's gone, not just the boot-time
  reconnect having failed once — it calls `wifi_go_offline()` (radio off,
  saved network kept, no dialog) rather than leaving the radio burning
  power retrying against nothing.
- **transcribe.cpp/h + transcribe_&lt;provider&gt;.cpp** — AI transcription,
  split into a provider-agnostic half and a provider-specific half so a
  future second provider is a new file plus a new build flag, not a
  rewrite. `transcribe.h` declares the whole public surface — generic
  names only (`ai_provider_name()`, `ai_provider_has_api_key()`/
  `..._get_api_key()`/`..._set_api_key()`, `ai_transcribe_file()`) — and
  `ui_epaper.cpp`/`web_server.cpp` only ever call those, never anything
  provider-specific. `transcribe.cpp` implements the provider-agnostic
  part: `transcribe_request()`/`transcribe_process_pending()` mirror
  `wifi_manager.cpp`'s `wifi_request_reconnect()`/
  `wifi_process_pending_reconnect()` pair, splitting the "ask for it"
  (from `ui_epaper.cpp`'s button handler) from the "actually block and
  repaint" (from `main.cpp`'s `loop()`, after `lv_timer_handler()`
  returns) for the same reentrancy reason; it also `#error`s at compile
  time if no `AI_PROVIDER_*` build flag is defined, so a missing one fails
  loudly here instead of as a confusing link error. `transcribe_process_pending()`
  first checks `WiFi.status()` and, if offline, calls
  `wifi_manager.h`'s `wifi_ensure_connected()` for one blocking retry
  against the saved network before giving up with "No WiFi connection." —
  the device can otherwise be offline going into this (see
  `wifi_manager.cpp/h` above) since recording/deleting/previewing don't
  need a network but transcription does. `transcribe_process_pending()`
  calls `sleep.h`'s `sleep_reset_activity()` right after the blocking
  `ai_transcribe_file()` call returns, before showing the result screen -
  without it, a transcription slow enough to outlast `sleep.cpp`'s idle
  timeout on its own would deep-sleep the device the instant
  `ui_is_sleep_blocked()` stops seeing `kTranscribeProgress`, before the
  user ever got to read "Transcription saved." Exactly one
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
  around the whole blocking call (a no-op on this board, but kept for
  symmetry with `web_server.cpp`'s SD handlers).
- **web_server.cpp/h** — `web_server_start()`/`web_server_handle()`, an
  ESP32-core `WebServer` on port 80. Two pages, same dark palette as
  `ui_epaper.cpp`: a file manager (list/download/upload/delete files on
  the SD root, plus a per-file Transcribe button for audio files) at `/`,
  backed by `/api/files`, `/api/download`, `/api/upload`, `/api/delete`,
  `/api/transcript-key`, `/api/transcript`; and a `/settings` page
  (WiFi/clock status, SD capacity, Reconnect WiFi, Delete WiFi Setup, an
  idle-sleep-timeout slider, and the AI provider's API key field, labeled
  dynamically from `aiProviderName` in the JSON below), backed by
  `/api/settings` (GET, a status snapshot) and `/api/settings/reconnect`,
  `/api/settings/forget`, `/api/settings/ai-key`,
  `/api/settings/idle-timeout` (POST, minutes — see `sleep.cpp/h` above).
  Only started once WiFi is up — either
  synchronously from `setup()` (first-boot portal case) or from `loop()`
  once `wifi_process_boot_connect()` reports the background boot-time
  connect landed (see `wifi_manager.cpp/h` above). Each handler that
  touches the card calls
  `display_suspend_touch()` + `storage.h`'s `sd_begin()` (and releases both
  after) — no-ops on this board, kept so a future board with a shared SPI
  peripheral wouldn't need new call sites; the AI key handler is the one
  exception, since `transcribe.h`'s `ai_provider_set_api_key()` is pure
  NVS and never touches the SD card. This server is plain HTTP, so
  `/api/settings` reports only whether a key is saved, never the key
  itself — the web page can clear or overwrite it but never displays the
  current value. The web file manager's Transcribe button is a second,
  independent transcription path alongside `transcribe.cpp`'s on-device
  one (`ui_epaper.cpp`'s button, which uploads from the ESP32 itself):
  its JS calls `GET /api/transcript-key` to get the raw saved key (the
  one deliberate exception to the "never the key itself" rule above,
  since the actual OpenAI request is made client-side, from the user's
  own browser, straight to `api.openai.com`, hardcoded to match whichever
  `transcribe_<provider>.cpp` is compiled in rather than going through
  `transcribe.h`'s generic surface — offloading the upload from the
  ESP32's own flaky TLS stack, see `transcribe_openai.cpp`'s retry-loop
  comment), downloads the audio via the existing `/api/download`, then
  `POST /api/transcript?name=...` writes the resulting text to `name`'s
  sibling `.txt` file, the same output `ai_transcribe_file()` produces.
  Uploads/deletes don't refresh the on-screen MP3 list (`mp3Files`); that
  only happens on reboot. `web_transcribe_in_progress()` tracks the window
  between those two calls (set on the key request, cleared on the final
  POST, self-clearing on a timeout otherwise) purely so `sleep.cpp` knows
  not to deep-sleep mid-flight — no request lands here while the browser
  is talking to the AI provider directly.

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

- `lvgl` is pinned to `9.2.2` — the registry mirrors lvgl's git tags, which
  jump from `9.2.2` straight to `9.3.0`; `9.2.2` is what's config-compatible
  with `lv_conf.h`.
- The platform itself is the `pioarduino` fork of `espressif32`, tracking
  newer `arduino-esp32` core releases than the stock PlatformIO platform.

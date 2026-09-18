# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO/Arduino, C++) for two Waveshare ESP32-S3 e-paper
boards, both built from this one repo via `platformio.ini`'s two envs:

- `esp32-s3-epaper154`: the Waveshare ESP32-S3-ePaper-1.54 — a 1.54"
  200x200 mono e-paper panel (SSD1681-class) + 2 onboard buttons + an
  onboard ES8311 speaker/mic codec, on an ESP32-S3-PICO-1-N8R8 module, no
  touch.
- `esp32-s3-epaper397`: the Waveshare ESP32-S3-ePaper-3.97 — an 800x480
  e-paper panel (rendered 1bpp-thresholded, not its native 4-gray — see
  `display_epaper397.cpp` below) + a 3-way rotary nav switch + a Boot
  button + the same ES8311 codec, on an ESP32-S3-WROOM-1-N16R8 module, no
  touch. This board also carries a QMI8658 IMU, an SHTC3 temp/humidity
  sensor, and a PCF85063 RTC that this firmware does not initialize or
  use — see "Board pin map" below.

Every board-specific source file is gated at compile time on a
`BOARD_EPAPER_154`/`BOARD_EPAPER_397` build flag (one per env, see
`platformio.ini`) — the same mechanism `transcribe.cpp/h`'s
`AI_PROVIDER_*` flags use, see that bullet below. `src/display.h` declares
the shared, board-agnostic surface (`SCREEN_W`/`SCREEN_H`, the
`DisplayButton` enum, `display_init_panel()`/`display_init_input()`, ...)
that each board's driver file implements.

It's an MP3/WAV file browser: scans an SD card's root for audio files and
lists them on an LVGL UI, with a WiFi connection manager (captive-portal
setup) and an HTTP file manager for the SD card alongside. Selecting an
audio file offers to transcribe it via an AI provider's API (OpenAI by
default, selected at compile time — see `transcribe.cpp/h` below), saving
the result as a sibling `.txt` file; it can also be played back or
deleted, and a new voice memo can be recorded straight from the on-device
UI (see `speaker.cpp/h`).

## Commands

Build:
```
pio run -e esp32-s3-epaper154
pio run -e esp32-s3-epaper397
```
Flash to a connected board:
```
pio run -e esp32-s3-epaper154 -t upload
pio run -e esp32-s3-epaper397 -t upload
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
  sibling project's `enterUltraSleep()`/`resetActivity()` (154 board only:
  same `PWR_HOLD_PIN` battery latch, same battery ADC pin, same button
  GPIOs, so the same approach applies unchanged there — the 397 board
  shares none of that, see "Board pin map" below).
  `sleep_process_idle()`, called last in `loop()`, deep-sleeps
  (`esp_deep_sleep_start()`, ext1 wakeup armed on the Select button's pin
  only — `SELECT_BUTTON_PIN` in sleep.cpp, board-conditional (154:
  PWR/GPIO18, 397: Function/GPIO5) — `ESP_EXT1_WAKEUP_ANY_LOW`, every
  other button deliberately left out of the mask so the sleep screen's
  "Hold Select to wake" stays true) once
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

- **display_epaper.cpp** / **display_epaper397.cpp** (`display.h`'s two
  implementations, each wrapped in `#ifdef BOARD_EPAPER_154`/`_397` for
  its whole body — same "whole-file `#ifdef`, one compiles, one doesn't"
  shape as `transcribe_<provider>.cpp`'s `AI_PROVIDER_*` gating, see that
  bullet below) — both bridge their panel into LVGL v9 the same way:
  `disp_flush_cb` renders LVGL's normal RGB565 framebuffer
  (`LV_DISPLAY_RENDER_MODE_FULL`, so it always sees the whole screen in
  one call every time) and thresholds it to 1bpp on the way out, rather
  than switching `lv_conf.h` to a monochrome color depth — the 397 panel's
  native 4-gray capability is deliberately unused, kept for parity with
  the 154 panel's rendering rather than a UI-quality upgrade. No touch, no
  backlight on either board, no shared-SPI peripheral to hand off —
  `display_suspend_touch()`/`display_resume_touch()` are no-op stubs so
  their callers (`web_server.cpp`, `transcribe.cpp`) don't need a special
  case around their SD access.
  - `display_epaper.cpp`: an SSD1681-class driver (command/LUT sequence
    ported from Waveshare's own example repo,
    waveshareteam/ESP32-S3-ePaper-1.54) for the 154 board's 200x200 panel.
  - `display_epaper397.cpp`: a different, LUT-less controller (command
    sequence ported from waveshareteam/ESP32-S3-ePaper-3.97's own
    `EPD_3IN97.c/h` example — refresh quality is selected per-call via a
    single "display update control" byte instead of an uploaded waveform
    table) for the 397 board's panel. The panel's own RAM is physically
    800x480 (landscape) at the controller level (`PANEL_W`/`PANEL_H` in
    this file — used for the panel's RAM-window commands, `epd_buf`'s
    1bpp packing, and `epd_set_pixel()`), but the board is used in
    portrait, so `display.h`'s `SCREEN_W`/`SCREEN_H` for this board are
    480x800 — the *logical*, rotated dimensions LVGL/`ui_epaper.cpp`
    actually see. `disp_flush_cb()` rotates every pixel 90 degrees
    counter-clockwise from logical to physical coordinates before calling
    `epd_set_pixel()` — confirmed against real hardware which direction is
    correct (clockwise came out upside-down/mirrored); flip the two lines
    right before that call if a different panel/mounting ever needs the
    other direction. Its RGB565 LVGL render buffer (`draw_buf`, 768,000
    bytes — same total regardless of logical vs. physical orientation)
    doesn't fit in internal SRAM the way the 154 board's does — allocated
    from PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` in
    `display_init_input()` instead. `display_init_panel()`'s first call is
    `epd_power_on()`:
    unlike the 154 board's `EPD_PWR_PIN` GPIO gate, this panel's analog
    drive rail (VSH/VSL/VGH/VGL — what actually moves the electrophoretic
    ink, as opposed to the panel's digital logic, which runs off a
    separate always-on rail) is switched by the onboard AXP2101 PMIC's
    ALDO1+ALDO2+ALDO3 outputs together (I2C address `0x34`, register `0x90`
    bits 0-2) — confirmed against real hardware, not derived from a
    datasheet: without it, the panel accepts every SPI command and BUSY
    toggles with plausible, spec-matching refresh durations, so it looks
    like it's working, but the glass never visibly changes, since the rail
    that moves ink is off. Register values are copied from
    78/xiaozhi-esp32's `main/boards/waveshare/esp32-s3-epaper-3.97/waveshare-s3-epaper-3.97.cc`
    (the actual factory-shipped firmware source for this board) — enabling
    only ALDO3 alone (as Waveshare's own simpler ESP-IDF example,
    `epaper_port.c`'s `EPD_Power_ON()`, does) was tried first and did not
    bring the panel up; all three rails together, exactly as that
    reference does, is what real hardware needed. `epd_display_partial()`
    (called from every LVGL flush) also deliberately does *not*
    reset/re-address the panel on each call the way the barebones Arduino
    example (and the 154 board's SSD1681 partial-update path) does — that
    reset-every-call shape came from an untested example and left the
    panel blank despite correct-looking BUSY timing; the proven shipped
    firmware just resends `0x24` + the buffer and triggers the update,
    reusing the RAM window/border-waveform state `epd_init_full()`
    established once at boot. SPI clock (20MHz) and the 500ms post-power-on
    settle delay before reset are likewise copied from that same proven
    source rather than guessed conservatively.
  - Both files' button-polling halves implement `display.h`'s
    `display_button_poll()`/`display_button_raw_pressed()`/
    `display_forget_wifi_combo_poll()` over their own board's physical
    buttons — see "Board pin map" below for which pin is which.
- **storage.cpp/h** — `load_mp3_catalog()` scans the SD root into the global
  `mp3Files`/`mp3FileCount` arrays (`storage.h`), filtering directories and
  dotfiles (macOS FAT litter like `._x.mp3`, `.DS_Store`). `sd_begin()`/
  `sd_end()` mount/unmount the card over the ESP32-S3's dedicated SDMMC
  peripheral — 1-bit mode (CLK/CMD/D0 only) on the 154 board, full 4-bit
  mode (CLK/CMD/D0-D3) on the 397 board, board-conditional pin constants
  and `SD_MMC.setPins()`/`begin()` calls — see "Board pin map" below.
- **ui_epaper.cpp** (`ui.h`'s implementation) — WiFi status, a scrollable
  file list, and per-file Play/Record/Transcribe/Delete/View (`.txt`
  transcripts only — `Screen::kTextView`, opened from the top of a `.txt`
  file's action menu via `storage.h`'s `read_text_file_preview()`; Select
  short-press scrolls down, Next short-press scrolls up — reversed from
  every other screen's Next-cycles/Select-confirms convention, since here
  Select doubles as both the scroll-down and the long-press-to-go-back
  action — and a Select long-press closes straight back to `kList` (not the
  action menu it was opened from) so the menu doesn't reappear on exit) —
  no on-screen Settings or WiFi credential entry, which stay on
  `web_server.cpp`'s existing web UI. A small explicit state machine
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
  (one I2C bus, one I2S controller, one amp-enable power rail) taking
  turns, never both at once — `mic_start_recording()` always stops
  playback first. Both claim the SD card for their whole duration
  (`storage.h`'s `sd_begin()`/`sd_end()`) and must be pumped every
  `loop()` iteration via `ui_epaper.cpp`'s `ui_process_input()`. Pin
  constants are board-conditional (see "Board pin map" below); the 154
  board has two separate amp-control pins (`PA_EN_PIN`, active-low rail
  switch; `PA_CTRL_PIN`, active-high shutdown), the 397 board has only one
  (`PA_CTRL_PIN`) doing both jobs — its polarity is an unverified guess
  (active-low, matching the 154 board's `PA_EN` convention) pending real
  hardware, flagged with a `TODO(board-397)` at `speaker_begin()`'s
  bring-up sequence; this is the exact class of bug (wrong enable
  polarity, I2C still ACKing, audio silently never powered) that already
  cost real debugging time on the 154 board once — see that pin's own
  comment.
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
  bundle exists in this project. `WiFiClientSecure` itself is wolfSSL-backed
  here, not arduino-esp32's built-in mbedTLS —
  `xorlent/ESP32-EasyWolfSSL`'s `WolfSSLClient` (`#include <WolfSSLClient.h>`
  instead of `<WiFiClientSecure.h>`; typedef'd back to the name
  `WiFiClientSecure` so call sites don't change) on top of
  `wolfssl/Arduino-wolfSSL` (not the plain `wolfssl/wolfssl` PlatformIO
  package — see `platformio.ini`'s `lib_ignore` comment for why the plain
  one has to be excluded once both are reachable via the dependency
  finder). `WolfSSLClient` has no equivalent to
  `WiFiClientSecure::lastError()`, so the negative-HTTP-code error branch
  here (and `transcribe_gemini.cpp`'s identical one) can no longer append a
  TLS-specific reason on top of `HTTPClient::errorToString()`.
  `scripts/patch_wolfssl.py` (`platformio.ini`'s `extra_scripts`) patches
  the installed `Arduino-wolfSSL`/`ESP32-EasyWolfSSL` packages every build,
  fixing two bugs neither project owns: a link error in `Arduino-wolfSSL`'s
  own `wolfssl.h` (see that script's top comment for the multiple-
  definition/undefined-reference story and why `transcribe_openai.cpp`/
  `transcribe_gemini.cpp` both `#define
  ANNOTA_WOLFSSL_SKIP_SERIAL_PRINT_DEFINITION` before including
  `WolfSSLClient.h`), and `WolfSSLClient::connect()` never sending an SNI
  extension in its TLS ClientHello at all — harmless against most hosts,
  but Cloudflare-fronted ones (api.openai.com included) drop the handshake
  outright without it, which surfaces here as a bare connect()
  failure/"connection refused" with no further detail (see the
  `lastError()` paragraph above for why). Needs `platformio.ini`'s
  `-D HAVE_SNI` build flag too, since `Arduino-wolfSSL`'s own
  `user_settings.h` compiles wolfSSL's SNI support out entirely otherwise.
  `ai_transcribe_file()` claims the SD card
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

`src/fonts/lv_font_it_{10,12,14,20,28,40}.c` (declared in
`include/fonts_it.h`) are custom-built replacements for lvgl's own
`lv_font_montserrat_<size>` — the built-in ones only bake in ASCII, so
accented letters (e.g. Italian's è à ò) silently render blank with them.
`ui_epaper.cpp` never references an `lv_font_it_<size>` directly — it
uses `fonts_it.h`'s board-conditional `FONT_HINT`/`FONT_HEADER`/
`FONT_BODY`/`FONT_ICON` role macros instead, so the same source picks the
right size per board without any `#ifdef` of its own:
- 154 (200x200 panel): the original `{10,12,14,28}` set — `FONT_HINT`=10,
  `FONT_HEADER`=12 (header/battery/SD status row), `FONT_BODY`=14 (list
  rows, menu options, message/details screens), `FONT_ICON`=28 (the one
  large centered icon glyph).
- 397 (much larger panel): bumped in three rounds of real-hardware
  iteration - first a uniform 1.2x scale of the 154 set, then hint/header
  unified at 12 with the rest scaled further to keep the visual hierarchy
  gap, then `FONT_HEADER` raised to match `FONT_BODY` (both bigger read
  better on this panel's header bar) - landing on `FONT_HINT`=12,
  `FONT_HEADER`=`FONT_BODY`=20, `FONT_ICON`=40 (reusing the same
  `lv_font_it_20`/`_40` files for two roles each, so only three distinct
  sizes are actually generated for this board despite four roles).
  `ui_epaper.cpp`'s `HEADER_H` is likewise board-conditional (154: 20,
  397: 32) to fit the taller header font without clipping.

Regenerated with `lv_font_conv` (via `npx`) from the exact same source
`Montserrat-Medium.ttf` + `FontAwesome5-Solid+Brands+Regular.woff` lvgl
itself ships at `<lvgl_lib_dep>/scripts/built_in_font/`, same options as
each original font's own `Opts:` header-comment (still present, unchanged,
at the top of each generated file here) plus one added `-r 0xC0-0xFF`
range on the Montserrat font to pull in Latin-1 Supplement, and (as of the
lv_font_conv version this project's `npx` currently resolves) a manual
fix-up of the generated `#include "lvgl/lvgl.h"` line back to
`#include <lvgl.h>` to match this project's `LV_CONF_INCLUDE_SIMPLE`
setup — same size/metrics/icon-glyph coverage otherwise, so they're
drop-in replacements for the originals. If a call site needs a font size
outside this set, either regenerate one more this same way or fall back
to the plain `lv_font_montserrat_<size>` (which will just be missing
accented glyphs for that one spot).

### Filename gotcha (case-insensitive filesystem)

This repo is developed on macOS's default case-insensitive filesystem.
`#include <WiFi.h>` (the Arduino core header) will silently resolve to a
project file named `wifi.h`/`wifi.cpp` sitting in `-Isrc`, breaking the
build in confusing ways. That's why the WiFi module is named
`wifi_manager.*`, not `wifi.*` — keep that naming if you touch it.

### Board pin map

Pin constants live scattered across each board-conditional source file
(`#if defined(BOARD_EPAPER_154)` / `#elif defined(BOARD_EPAPER_397)`, see
each file's own bullet above) - collected here for reference rather than
re-derived from six different `.cpp` files:

|                       | 154 board            | 397 board                     |
|-----------------------|-----------------------|--------------------------------|
| e-paper SPI           | SCK 12, MOSI 13, CS 11, DC 10, RST 9, BUSY 8, PWR 6 (active-low) | SCK 11, MOSI 12, CS 10, DC 9, RST 46, BUSY 3, no GPIO PWR pin - see "Panel power (PMIC)" below |
| Buttons                | Boot/GPIO0 ("next"), PWR/GPIO18 ("select") | Down/GPIO6 ("next"), Function/GPIO5 ("select"), Up/GPIO4 ("prev"), Boot/GPIO0 (combo-only) |
| SD (SDMMC)             | CLK 39, CMD 41, D0 40 (1-bit) | CLK 16, CMD 17, D0 15, D1 7, D2 8, D3 18 (4-bit) |
| I2S (ES8311)           | MCLK 14, BCLK 15, WS 38, DOUT 45, DIN 16 | MCLK 13, BCLK 14, WS 47, DOUT 48, DIN 21 |
| I2C (codec + ...)      | SDA 47, SCL 48 (codec only - RTC/SHTC3 unused) | SDA 41, SCL 42 (codec + AXP2101 PMIC + unused QMI8658/SHTC3/PCF85063) |
| Amp enable             | PA_EN 42 (active-low), PA_CTRL 46 (active-high) | PA_CTRL 39 (polarity unverified guess - active-low; not yet tested against real hardware) |
| Battery level          | ADC GPIO4 (200K/200K divider) | AXP2101 PMIC fuel gauge over I2C (not a GPIO) - unimplemented, stubbed |
| Power latch            | `PWR_HOLD_PIN` GPIO17 (`main.cpp`) | none - AXP2101 PMIC handles power sequencing itself in hardware, confirmed no-op is safe on real hardware |

**Panel power (PMIC, 397 board only):** the e-paper panel's analog drive
rail is switched by the onboard AXP2101 PMIC (I2C address `0x34`) rather
than any GPIO - `display_epaper397.cpp`'s `epd_power_on()` enables
ALDO1+ALDO2+ALDO3 together (register `0x90` = `0x07`) before touching the
panel over SPI at all. This was found the hard way against real
hardware - see that function's own comment for the full story (the panel
silently accepted every command and BUSY toggled with plausible timing
the whole time it was unpowered, which is what made this take several
bring-up iterations to isolate) and for where the exact register sequence
came from (78/xiaozhi-esp32's factory-shipped board source, not a guess).

The 397 board's battery-level reading is unimplemented (would need an
AXP2101 register read via the same I2C bus, not `analogReadMilliVolts()`
- `battery.cpp` returns `BATTERY_PERCENT_UNKNOWN`, which hides the
header's battery readout rather than showing a bogus number) - stubbed as
a feature addition beyond this port's scope, not because the answer is
unknown.

### Dependency pin notes (see comments in platformio.ini)

- `lvgl` is pinned to `9.2.2` — the registry mirrors lvgl's git tags, which
  jump from `9.2.2` straight to `9.3.0`; `9.2.2` is what's config-compatible
  with `lv_conf.h`.
- The platform itself is the `pioarduino` fork of `espressif32`, tracking
  newer `arduino-esp32` core releases than the stock PlatformIO platform.

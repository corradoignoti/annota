# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO/Arduino, C++) for two Waveshare boards, one
`platformio.ini` env each, sharing `[env]`:
- `esp32-s3-epaper154` — ESP32-S3-ePaper-1.54: 1.54" 200x200 mono e-paper
  + 2 onboard buttons + ES8311 codec, no touch.
- `esp32-s3-epaper397` — ESP32-S3-ePaper-3.97: 3.97" 800x480 mono e-paper
  used in portrait (UI is 480x800) + 3-way rotary switch (Up/Press/Down) + BOOT + the same
  ES8311 codec + an AXP2101 PMIC, 16MB flash.

The env sets `-D BOARD_EPAPER_154=1` or `-D BOARD_EPAPER_397=1`;
`src/board.h` maps it to pins and capability flags (`BOARD_HAS_KNOB`,
`BOARD_HAS_PWR_LATCH`, `BOARD_HAS_PMIC`, `SD_BUS_WIDTH`, `AUDIO_RAIL_PIN`
-1 = none, ...) and `#error`s if neither is set. Every pin lives there —
never hardcode one in a module. Board-specific code goes behind those
flags; the 1.54 must keep behaving exactly as before any 3.97 change.
**New features: the user states which envs get them** — gate per-env
code on `BOARD_*`. Same app on both: It's an MP3/WAV file browser: scans an SD
card's root for audio files and lists them on an LVGL UI, with a WiFi
connection manager (captive-portal setup) and an HTTP file manager for the
SD card alongside. Selecting an audio file offers to transcribe it via an
AI provider's API (OpenAI by default, selected at compile time — see
`transcribe.cpp/h` below), saving the result as a sibling `.txt` file; it
can also be played back or deleted, and a new voice memo can be recorded
straight from the on-device UI (see `speaker.cpp/h`).

## Commands

Build (`esp32-s3-epaper154` or `esp32-s3-epaper397` — build both after
touching shared code):
```
pio run -e esp32-s3-epaper154
pio run -e esp32-s3-epaper397
```
Flash to a connected board:
```
pio run -e esp32-s3-epaper154 -t upload
```
`.releaserc.json` builds both and attaches `firmware.bin` (1.54) and
`firmware-epaper397.bin`.
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
trigger directly — `wifi_process_pending_reconnect()` and
`transcribe_process_pending()` (see their bullets below) — then
`web_server_handle()`, then `sleep_process_idle()` (see `sleep.cpp/h`
below) last, once everything else that could count as activity this pass
has had a chance to reset its clock.
While `usb_drive.h`'s `usb_drive_active()` is true, `loop()` stops right
after `ui_process_input()` and only pumps `usb_drive_process()` (see
`usb_drive.cpp/h` below) — the USB host owns the SD card, so no other pump
may touch it.

- **sleep.cpp/h** — idle-timeout deep sleep, ported from the pala_note
  sibling project's `enterUltraSleep()`/`resetActivity()` (same board
  family: same `PWR_HOLD_PIN` battery latch, same battery ADC pin, same
  button GPIOs, so the same approach applies unchanged).
  `sleep_process_idle()`, called last in `loop()`, deep-sleeps
  (`esp_deep_sleep_start()`, ext1 wakeup armed on the Select/PWR button
  only, `ESP_EXT1_WAKEUP_ANY_LOW` — BOOT/Next deliberately left out of the
  mask so the sleep screen's "Hold Select to wake" stays true — that screen,
  `ui_epaper.cpp`'s `add_sleep_screen()`, is full-screen: the
  `assets/icon_200x200.svg` logo redrawn from LVGL arcs/rects scaled to
  the per-board `LOGO_SIZE`, painted with a full refresh on the 3.97 via
  `display.h`'s `display_request_full_refresh()`; 3.97 only, a summary box
  below it, `add_sleep_stats()`: notes, untranscribed notes, battery %, SD
  used/total) once
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
  calls back. Right before `esp_deep_sleep_start()` it powers down what
  the ESP32-S3's own deep sleep doesn't: `speaker_prepare_deep_sleep()`
  (codec power-down via `es8311_power_down()`; on the 3.97, which has no
  audio rail switch, the non-RTC amp pin GPIO39 latched LOW with
  `gpio_hold_en()` — `speaker_begin()` releases it after wake-up), then
  `display_prepare_deep_sleep()` (3.97 panel's deep-sleep command), then
  `battery_prepare_deep_sleep()` (3.97: AXP2101 ALDO1-3 rails off, turned
  back on by the next boot's `battery_init()`). All no-ops on the 1.54
  except the codec power-down. Left on, those 3.97 rails drained the
  battery behind the sleep screen. `sleep_reset_activity()` is called from `main.cpp`'s
  `setup()` (starts the clock at boot) and from two activity sources:
  `ui_epaper.cpp`'s `ui_process_input()` on any onboard button edge, and
  `web_server.cpp`'s route registrations (each wrapped in a
  `with_activity()` helper) on any served HTTP request — so the device
  won't deep-sleep out from under someone actively browsing or
  downloading from the web file manager just because no button was
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

- **i18n.cpp/h + i18n_strings.def** — translations (English, Italian,
  French) for everything user-visible, one device-wide language picked on
  the web Settings page (`POST /api/settings/language`, `"en"/"it"/"fr"`)
  and persisted in NVS (namespace `"annota"`, key `"lang"`, lazily loaded
  like `sleep.cpp`'s timeout). `i18n_strings.def` is an X-macro table:
  `TR(ID, en, it, fr)` rows are e-paper strings, looked up with
  `tr(Str::ID)` (also used as printf formats); `TRW(key, en, it, fr)` rows
  are web strings, served as `GET /i18n.js` (`i18n_write_web_js()`:
  `window.I18N`, `window.I18N_LANG`, and the `t(key, ...args)`/
  `applyI18n()` helpers). A language change repaints the e-paper UI via
  `ui.h`'s `ui_request_rerender()`, no reboot. See "Translations" below.
- **usb_drive.cpp/h** — USB drive mode: the whole SD card exposed to a
  computer as a USB mass-storage device (arduino-esp32's `USBMSC`, i.e.
  TinyUSB MSC), entered from the Home carousel's 3rd card ("USB drive",
  `ui_epaper.cpp`'s `kHome` -> `Screen::kUsbDrive`; WiFi is taken offline
  first so the web file manager can't touch the card). No build-config
  change: the board runs `ARDUINO_USB_MODE=1` (Serial on the
  USB-Serial-JTAG peripheral), but that peripheral and USB-OTG share one
  internal PHY on GPIO19/20, and `usb_drive_start()`'s `USB.begin()`
  switches it to OTG at runtime — Serial goes silent while the mode is
  active. There's no `USB.end()` to switch back, so every exit reboots via
  `reboot_now()` (which also brings the JTAG serial port back and re-scans
  a card the host may have changed — the PHY selection lives in an RTC
  register a software reset doesn't clear, so `usb_drive_start()` registers
  an `esp_restart()` shutdown handler that switches it back to JTAG and
  forces a host re-enumeration first, covering the both-buttons reboot
  too): the host ejecting the drive (SCSI
  START STOP UNIT with eject), a Select long-press on `kUsbDrive`
  (`usb_drive_request_exit()`), or the host connection staying
  unmounted/suspended for 2 s after having been seen (cable pulled on
  battery). Sector I/O goes through `storage.h`'s `sd_read_sectors()`/
  `sd_write_sectors()` (single-sector `SD_MMC.readRAW()`/`writeRAW()`
  loops, so throughput is modest) with the card claimed via `sd_begin()`
  for the whole session. `ui_is_sleep_blocked()` covers `kUsbDrive`.
- **reboot_combo.cpp/h** — "hold both buttons 5s to reboot"
  (`REBOOT_COMBO_HOLD_MS`). `reboot_combo_start()`, called in `setup()`
  right after the battery latch and before anything that could hang,
  configures the button GPIOs itself and starts a FreeRTOS task pinned to
  core 0 (Arduino's `loop()` runs on core 1) that polls them directly, so
  the combo works even when `loop()`/`setup()` is stuck. Only armed after
  both buttons have been seen released since boot (no reboot loop while
  still held). `reboot_now()` is the shared reboot path (also used by the
  on-device Reboot menu item and `wifi_forget_and_reboot()`): it
  `gpio_hold_en()`s `PWR_HOLD_PIN` (defined in `board.h`, 1.54 only) so the
  battery latch doesn't glitch low during the reset, then `esp_restart()`;
  `main.cpp`'s `keepBatteryPowerOn()` releases the hold after driving the
  pin HIGH again. `ui_process_input()` still skips per-button polling while
  both are held, so the gesture never fires a single-button long press.
- **display_epaper397.cpp** (3.97 only, whole body in `#ifdef
  BOARD_EPAPER_397`) — the 3.97's SSD1677-class 800x480 panel, ported
  from Waveshare's waveshareteam/ESP32-S3-ePaper-3.97 examples
  (EPD_3in97.cpp / factory firmware's epaper_port.c). Portrait: LVGL
  renders 480x800 (board.h's `BOARD_SCREEN_W/H`) and `disp_flush_cb`
  rotates each pixel onto the 800x480 panel (`PANEL_W/H`; flip the
  direction with `EPD_PORTRAIT_FLIP`). Waveforms are in the
  controller's OTP (no LUT upload): 0x22 0xF7 = full refresh, 0xFF =
  partial. Unlike the 1.54, LVGL renders in `LV_DISPLAY_RENDER_MODE_PARTIAL`
  into a 20-line RGB565 strip buffer in internal RAM (rendering a full
  frame into PSRAM was the slowest step of a screen change); each strip is
  thresholded/rotated straight into the PSRAM 48 KB 1bpp `epd_buf`, and
  the panel is refreshed once per frame, on the last strip
  (`lv_display_flush_is_last()`). SPI at 10 MHz, 20/2/20 ms panel reset
  before each partial refresh; every `EPD_FULL_REFRESH_EVERY`th frame is a
  full refresh to clear ghosting. `EPD_LOG_TIMING` prints one Serial line
  per frame (render+convert vs panel ms). The panel's supply is the PMIC's
  ALDO3, which `battery.h`'s `battery_init()` (called before
  `display_init_panel()`) makes sure is on.
- **display_buttons.cpp** — `display.h`'s button polling for both boards:
  `DisplayButton` is `kNext`/`kSelect` plus `kPrev`/`kBack`, the latter two
  only wired on knob boards (3.97: rotary Up and BOOT; Down = Next, Press =
  Select) and always `kNone` on the 1.54. In `ui_epaper.cpp`'s
  `ui_process_input()`, Back short is folded into a long Select (every
  screen's existing back-out path) except on `kList`, where it goes Home;
  Prev short is the mirror of Next on every cycling screen; `kList` opens a
  file's action menu on Select immediately (no double-press window) and
  Up held jumps to the top instead; on `kTextView`
  Down/Up scroll down/up. `wifi_manager.cpp`'s setup-portal loop also
  accepts Back. The reboot combo is Press + BOOT on the 3.97
  (`reboot_combo.cpp`), and idle-sleep wake is the knob press (board.h's
  `WAKE_BUTTON_GPIO`, RTC pull-up kept on in `sleep.cpp`).
- **battery.cpp/h** — 1.54: ADC on GPIO4 through a divider, voltage-curve
  percentage. 3.97 (`BOARD_HAS_PMIC`): AXP2101 via lewisxhe/XPowersLib on
  the shared I2C bus (SDA41/SCL42, also the ES8311's) — fuel-gauge
  percentage (100 with no cell, i.e. USB-only); `battery_init()` sets
  ALDO1-3 to 3.3 V/on with raw I2C writes (not via XPowersLib, whose
  begin() bails on a chip-ID mismatch) on every boot - ALDO3 is the panel
  supply, and Waveshare's factory firmware turns it off when idle, a state
  the PMIC keeps across ESP32 resets/reflashes (no-op on the 1.54). There's no GPIO power latch on the 3.97
  (GPIO17 is SD CMD there) — `main.cpp`'s `keepBatteryPowerOn()` and
  `reboot_now()`'s pad hold are `BOARD_HAS_PWR_LATCH`-only.
- **display_epaper.cpp** (1.54 only, whole body in `#ifdef
  BOARD_EPAPER_154`; `display.h`'s implementation there) — an SSD1681-class
  e-paper panel driver (command/LUT sequence ported from Waveshare's own
  example repo, waveshareteam/ESP32-S3-ePaper-1.54) bridged into LVGL v9,
  (buttons: `display_buttons.cpp`). `disp_flush_cb` renders LVGL's normal RGB565 framebuffer
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
  dotfiles (macOS FAT litter like `._x.mp3`, `.DS_Store`). The audio
  list is sorted: files with no sibling `<basename>.txt` first, then by
  that transcript's last-write time, newest first (`audio_before()`).
  `sd_begin()`/
  `sd_end()` mount/unmount the card over the ESP32-S3's dedicated SDMMC
  peripheral (board.h's `SDMMC_*_PIN`/`SD_BUS_WIDTH`: 1-bit on the 1.54,
  4-bit on the 3.97).
- **ui_epaper.cpp** (`ui.h`'s implementation) — shared by both boards:
  every size, offset and font comes from the per-board layout block at the
  top of the file (`HEADER_H`/`ROW_H`/`HINT_H`, `FONT_HINT`/`FONT_SMALL`/
  `FONT_BODY`/`FONT_ICON`, `PAD_X`, `CARD_*`, `MENU_*`, ...; the 1.54
  column holds the original values) — never add a raw pixel literal or
  `&lv_font_it_N` at a call site. Hints/strings naming buttons go through
  `knob_tr(Str::X, Str::X_K)` (see Translations). The 3.97's QR canvas
  buffer is PSRAM-allocated on first use. WiFi status, a scrollable
  audio file list ("Notes" — the device no longer lists `.txt` files on
  their own), and per-file Play/Record/Transcribe/Delete/View transcription
  (`Screen::kTextView`, opened from an audio file's action menu when it has
  a sibling transcript, via `storage.h`'s `read_text_file_preview()`; Select
  short-press scrolls down, Next short-press scrolls up — reversed from
  every other screen's Next-cycles/Select-confirms convention, since here
  Select doubles as both the scroll-down and the long-press-to-go-back
  action — and a Select long-press closes straight back to `kList` (not the
  action menu it was opened from) so the menu doesn't reappear on exit),
  plus a Details item in the action menu
  (`Screen::kDetails` — name, size, and the audio file's playing time from
  `storage.h`'s `get_audio_duration_seconds()`, which parses the WAV
  chunk list or the MP3 Xing/Info/VBRI tag, falling back to a CBR
  estimate — plus, for an audio file with a sibling `<basename>.txt`,
  that transcript's size and date via `storage.h`'s `get_file_info()`,
  shown in a compact icon-less 12pt card to fit; Select goes back to the
  action menu; `render_option_menu()` switches to a compact layout
  whenever a menu's rows wouldn't otherwise clear the hint bar) —
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
  `lv_timer_handler()` — unless the file's `Mp3Entry::size` exceeds
  `transcribe.h`'s `TRANSCRIBE_MAX_FILE_BYTES` (2 MB on the 1.54, 4 MB on the 3.97, set per env via
  `platformio.ini`'s build_flags), in which case it
  goes straight to `kTranscribeResult` with a "use the web interface"
  message and nothing is queued. If the audio file already has a sibling
  `<basename>.txt` (`storage.h`'s `find_sibling_transcript()`, checked
  once when the action menu opens), Transcribe's slot shows View
  transcription instead, opening that `.txt` in `kTextView`; re-transcribing
  then means deleting the `.txt` or using the web UI. Selecting Delete calls `storage.h`'s
  `delete_file()` directly, same reasoning — on an audio file it also
  deletes the sibling `<basename>.txt` transcript if one exists
  (`storage.h`'s `find_sibling_transcript()`, checked when the action menu
  opens), and `kDeleteConfirm` warns about that first (confirm label
  "Delete audio + .txt", plus a wrapped warning line inside the menu panel,
  between title and options, that the named `.txt` file will also be
  deleted — `render_option_menu()`'s optional `note`). A long Select press on the
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
  Playback volume is a `VolumeLevel` preset (Low/Medium/High = software
  gain 0.25/0.5/1.0 via `AudioOutput::SetGain()`, on top of the codec's
  fixed `DEFAULT_VOLUME`; High, the old output, is the default), persisted
  in NVS (namespace `"annota"`, key `"vol"`), applied mid-track.
  3.97 only: holding Up or Down 3 s (`VOLUME_HOLD_MS`, timed off the raw
  pin, since the button poll's long press fires at 700 ms) on `kPlaying`
  opens `Screen::kVolumeMenu` over the still-playing track — Up/Down move
  the highlight, Select applies + saves that level (menu stays open),
  Back closes it.
- **wifi_manager.cpp/h** — tzapu/WiFiManager underneath. WiFi is off by
  default: `wifi_start_boot_connect()` takes two paths at boot depending on
  whether a network is already saved in NVS: none saved opens a captive
  portal AP ("Annota-Setup", no password) with no timeout and shows the
  on-screen dialog, blocking `setup()` until the user configures one from a
  phone/laptop (unchanged from before); one saved instead just confirms
  that (briefly initializing the WiFi driver so the saved-network check
  reads real NVS state, not uninitialized garbage) and immediately powers
  the radio back off via `wifi_go_offline()` — no background connect is
  attempted, so there's no boot-time polling in `loop()` any more. Going
  online afterwards is always on-demand or explicit: `wifi_ensure_connected()`
  (used by `transcribe.cpp`'s `transcribe_process_pending()` before a
  transcription — see that bullet) makes one blocking reconnect attempt to
  the saved network, never opens the portal, and powers the radio back off
  again itself if that attempt fails (a failure here only ever happens on a
  radio the call itself just touched, so there's nothing left initialized-
  but-unassociated to clean up elsewhere); the on-device Offline↔Online
  menu item and the Settings page's "Reconnect WiFi" button both go through
  `wifi_request_reconnect()`/`wifi_process_pending_reconnect()` (it fires
  from an LVGL click handler already nested inside `lv_timer_handler()`,
  which refuses to run itself again while it's running — so the actual
  retry can't happen there; `loop()` picks it up and blocks until it
  connects or times out, since this is a wait the user explicitly asked
  for) and start the web file manager (`web_server_start()`, idempotent —
  see `web_server.cpp/h` below) on success. The setup portal never
  reappears on its own once a network is saved — a reconnect timeout just
  leaves the device offline, said only via the header status line
  (`ui_set_wifi_status()`, left blank rather than any warning once WiFi is
  off by design — see `wifi_go_offline()`) rather than a modal, so
  whatever's on screen (the file list, say) isn't interrupted — rather than
  wiping the saved credentials, since WiFiManager's "saved" check reading
  stale NVS state isn't reason enough to drop the user back into AP setup
  out from under them. The device stays fully usable with WiFi off —
  record, delete, and preview all work with no network — and this same
  boot logic runs unchanged on every boot, including a deep-sleep wakeup
  (waking is a full MCU reset, see `sleep.cpp/h` above, so there's no
  separate wake-time path). The only way back to the setup portal is the
  explicit, irreversible "Delete WiFi Setup" button
  (`wifi_forget_and_reboot()`). `wifi_go_offline()` (`WiFi.disconnect(true)`,
  powering the radio off for battery life, saved network untouched) has
  three callers: the on-device Offline menu item, `wifi_start_boot_connect()`'s
  stay-off-at-boot path, and `transcribe_process_pending()`'s post-
  transcription auto-off (only when that call is the one that turned WiFi
  on in the first place — see `transcribe.cpp/h` below for the guard
  against cutting a manual Online session out from under the user).
  `wifi_process_periodic_check()`, called from `loop()` alongside the other
  `wifi_process_*()` functions, is a last entry point: every 15 minutes
  (`WIFI_HEALTH_CHECK_INTERVAL_MS`), if the radio's on but not connected —
  the AP's gone, not just a reconnect attempt having failed once — it calls
  `wifi_go_offline()` silently, same as the on-device "Offline" item;
  mostly moot now that the radio is normally off already, but harmless.
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
  loudly here instead of as a confusing link error. WiFi is off by default
  (see `wifi_manager.cpp/h` above), so `transcribe_process_pending()` first
  checks `wifi_is_connected()`; if it was already true (e.g. the user is
  manually Online, browsing the web file manager) it leaves WiFi exactly as
  found and never turns it off afterward — only when it was false does it
  call `wifi_manager.h`'s `wifi_ensure_connected()` for one blocking retry
  against the saved network. If that fails (no saved network reachable)
  it's not treated as a transcription failure — no `_error.txt` is
  written: the request is parked (`transcribe_is_waiting_for_wifi()`) and
  `ui.h`'s `ui_show_wifi_join_for_transcribe()` jumps straight to the
  on-device scan-and-join list (`kWifiScanning` -> `kWifiJoinList`,
  skipping `kWifiManage`'s Join/Create menu since a standalone AP has no
  internet). Picking a network lets `wifi_process_pending_join()` call
  `transcribe_resume_after_join()`, which re-queues the transcription
  (runs later that same `loop()` pass, and still powers WiFi off after,
  since it was turned on for it) instead of showing the web QR screen, or
  shows "Could not join the WiFi network." if the join fails; backing out
  of the list calls `transcribe_cancel_wifi_wait()` and returns to
  `kList`. Otherwise it goes on to call the
  now-idempotent `web_server_start()` (this may be the first WiFi
  connection since boot, since boot no longer auto-connects), and then,
  once `ai_transcribe_file()` returns either way, call
  `wifi_manager.h`'s `wifi_go_offline()` before painting the result screen
  — recording/deleting/previewing don't need a network but transcription
  does, and this is what keeps WiFi on only for the duration of an actual
  transcription. `transcribe_process_pending()`
  calls `sleep.h`'s `sleep_reset_activity()` right after the blocking
  `ai_transcribe_file()` call returns, before showing the result screen -
  without it, a transcription slow enough to outlast `sleep.cpp`'s idle
  timeout on its own would deep-sleep the device the instant
  `ui_is_sleep_blocked()` stops seeing `kTranscribeProgress`, before the
  user ever got to read "Transcription saved." Progress feedback and
  diagnostics go through three provider-facing hooks in `transcribe.h`
  (implemented in `transcribe.cpp`, so providers never include `ui.h`):
  `transcribe_report_phase()` (`TranscribePhase` — Connecting WiFi /
  Uploading / Waiting for transcription / Saving, shown step-numbered on
  `kTranscribeProgress`), `transcribe_report_upload()` (drives that
  screen's upload bar via `ui.h`'s `ui_update_transcribe_progress()`,
  throttled to one e-paper repaint per 10% step or retry-attempt change;
  each provider's body `Stream` calls it from `readBytes()` and flips to
  the Waiting phase once its last byte is served), and `transcribe_log()`
  (appends to a ≤4 KB in-RAM log, echoed to Serial: header, per-attempt
  HTTP code/bytes sent/duration/RSSI, the first 1 KB of any error
  response body — never the API key, and never Gemini's request URL,
  which carries it). On failure `transcribe_process_pending()` saves that
  log as `<basename>_error.txt` next to the audio file (`storage.h`'s
  `write_text_file()`, after the provider's own `sd_end()` — SD claims
  aren't ref-counted) and appends "Details: <name>" to the result message;
  a later successful run deletes the stale log. Exactly one
  `transcribe_<provider>.cpp` implements the rest (`ai_provider_name()`
  and `ai_transcribe_file()`) — each file's entire body is wrapped in
  `#ifdef AI_PROVIDER_<NAME>`, so every provider file can sit in `src/`
  at once and only the one selected by `platformio.ini`'s build_flags
  (currently `-D AI_PROVIDER_OPENAI=1`) compiles to anything. NVS keys are
  namespaced per provider (e.g. `openaiKey`) so switching the compiled-in
  provider doesn't feed it a stale key saved for a different one.
  `transcribe_openai.cpp` (`whisper-1`, `/v1/audio/transcriptions`)
  then, on success, a second `gpt-4o-mini` `/v1/chat/completions` call
  (JSON mode, `summarize_transcript()`) that writes an AI title and
  abstract on top of the `.txt` — title, blank line, abstract, blank line,
  transcript, same language as the transcript; if that second call fails
  it only logs and saves the plain transcript (reported as the optional
  `TranscribePhase::kSummarizing`, "4/5" on the progress screen). It
  streams the upload straight off the SD card through a custom `Stream`
  subclass wrapping the multipart preamble/file/trailer — the ESP32
  doesn't have enough RAM to buffer a whole audio file first — and skips
  TLS cert validation (`WiFiClientSecure::setInsecure()`); no root-CA
  bundle exists in this project. `WiFiClientSecure` is arduino-esp32's
  stock mbedTLS client (hardware AES/SHA on the S3; an earlier wolfSSL
  swap was reverted — uploads through it topped out around 30 KB/s). The Whisper upload goes through `UploadClient`, a
  `WiFiClientSecure` subclass that coalesces HTTPClient's 1460-byte body
  writes into 16 KB TLS records and rides out socket stalls instead of
  failing on HTTPClient's single 1 ms retry; the radio's modem sleep is
  also switched off for the upload's duration. Throughput matters beyond
  speed: api.openai.com resets a request whose body is still arriving
  after ~100-120 s. mbedTLS allocates its record buffers from internal
  RAM, which is why `display_epaper.cpp`'s 120 KB LVGL `draw_buf` lives in
  PSRAM — as a static array it plus TinyUSB's static buffers (linked in by
  `usb_drive.cpp`) left too little internal heap for the upload.
  `ai_transcribe_file()` claims the SD card
  itself (`storage.h`'s `sd_begin()`/`sd_end()`) but leaves pausing/
  resuming touch to the caller — `transcribe_process_pending()` does that
  around the whole blocking call (a no-op on this board, but kept for
  symmetry with `web_server.cpp`'s SD handlers).
- **web_server.cpp/h** — `web_server_start()`/`web_server_handle()`, an
  ESP32-core `WebServer` on port 80. Three pages, same dark palette as
  `ui_epaper.cpp`: a file manager at `/` mirroring the device's Notes
  list — a Notes table with one row per audio file (`.mp3`/`.wav`, both
  playable via `/api/play`) and its sibling `<basename>.txt` folded in
  (`buildNotes()`: the transcript's AI title, from `/api/files`'s `title`
  field — `storage.h`'s `read_transcript_title()`, raw UTF-8 — under the
  filename; View transcription in place of Transcribe once transcribed;
  Delete, Download and their batch versions cover both files; default order is the
  device's, untranscribed first then newest transcript, with Name/Date
  headers cycling asc/desc/default), plus an "Other files" table for
  everything else (orphan `.txt`, `_error.txt` logs, files copied in over USB) with
  view/download/delete (no browser upload) — backed by `/api/files`, `/api/download`, `/api/delete`,
  `/api/transcript-key`, `/api/transcript`; and a `/settings` page
  (WiFi/clock status, SD capacity, Reconnect WiFi, Delete WiFi Setup, an
  idle-sleep-timeout slider, and the AI provider's API key field, labeled
  dynamically from `aiProviderName` in the JSON below), backed by
  `/api/settings` (GET, a status snapshot) and `/api/settings/reconnect`,
  `/api/settings/forget`, `/api/settings/ai-key`,
  `/api/settings/idle-timeout` (POST, minutes — see `sleep.cpp/h` above),
  `/api/settings/language` (POST, see `i18n.cpp/h` above); and a static
  `/about` page (what Annota is, plus credits for third-party icons/fonts —
  add one there when bringing in a new asset). All three pages load
  `/i18n.js` synchronously in `<head>`; static markup carries
  `data-i18n`/`data-i18n-ph`/`data-i18n-title` keys (filled by
  `applyI18n()` at the top of each page's main script), and every JS
  string goes through `t("key", ...)`.
  `web_server_start()` is idempotent (a file-scoped `static bool` guard —
  a no-op past the first call), since WiFi is off by default and can newly
  become connected from several places, each of which calls it
  opportunistically: `setup()` (first-boot portal case), a successful
  `wifi_process_pending_reconnect()` (manual Online toggle / Settings
  page's "Reconnect WiFi"), and `transcribe_process_pending()`'s on-demand
  connect before a transcription (see `wifi_manager.cpp/h` and
  `transcribe.cpp/h` above). Each handler that touches the card calls
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
  sibling `.txt` file, the same output `ai_transcribe_file()` produces
  (the OpenAI `callProvider()` makes the same title/abstract
  chat-completions call via its `summarize()`, with the same two attempts
  and plain-transcript fallback; `callProvider()` returns `{ text, note }`
  and a skipped header is reported in the page's status line, not only
  the browser console). While it runs, the page shows the same
  step-numbered phases as the device's `kTranscribeProgress` screen
  (`reportPhase()` in `INDEX_HTML_TAIL`: Downloading from device /
  Uploading to AI provider / Waiting for transcription / Writing title &
  abstract / Saving — each provider's `PROVIDER_PHASES` picks its subset
  for the "n/total" numbering) in `#status`, with `#progress` showing
  download and upload percentages; the download and provider upload go
  through XHR (`downloadWithProgress()`/`xhrWithUploadProgress()`) since
  `fetch()` has no progress events. A Sync bar above the list (`#syncBar`,
  `syncTranscribe()`) runs that same browser-side path over every audio
  file with no sibling `<basename>.txt` (case-insensitive; a leftover
  `_error.txt` doesn't count, so failed files get retried), sharing
  `runTranscribeBatch()` with the batch bar's Transcribe button; it
  re-fetches `/api/transcript-key` before each file so
  `web_transcribe_in_progress()` stays armed for the whole batch.
  Web deletes don't refresh the on-screen MP3 list (`mp3Files`); that
  only happens on reboot. `web_transcribe_in_progress()` tracks the window
  between those two calls (set on the key request, cleared on the final
  POST, self-clearing on a timeout otherwise) purely so `sleep.cpp` knows
  not to deep-sleep mid-flight — no request lands here while the browser
  is talking to the AI provider directly.

### Translations

Every user-visible string — e-paper UI, error messages that reach the
screen, both web pages — lives in `src/i18n_strings.def`, never as a
literal at the call site. When adding or changing one:
- Text that names buttons needs a `<ID>_K` twin row for knob boards
  (Up/Down/Select/BOOT), picked via `ui_epaper.cpp`'s `knob_tr()`.
  Board-specific web text (About page) uses `<key>_397` rows, chosen with
  `#if` inside the raw-literal HTML.
- Add/edit the row with **all three** languages (EN, IT, FR). A missing
  column is a compile error by design; don't paper over it with a copy of
  the English text.
- `TR` (device) strings: keep printf specifiers identical across columns,
  use only ASCII + Latin-1 letters (U+00C0–U+00FF — what `lv_font_it_*`
  carry: no œ, « », ’, …), and keep hints short (two lines at 200 px).
- `TRW` (web) strings: `{0}`, `{1}` placeholders, any Unicode. Use
  `t("key")` with a literal key (write `c ? t("a") : t("b")`, not
  `t(c ? "a" : "b")`) so the checker can see it.
- Run `python3 tools/check_i18n.py` (also run in CI by
  `.github/workflows/i18n.yml`): placeholder/specifier mismatches, font
  coverage, undefined/unused keys, and literals passed straight to the UI
  text helpers.
Not translated: `transcribe_log()`/`_error.txt` diagnostics, Serial output,
server plain-text error bodies, AI prompts, and tzapu/WiFiManager's
captive-portal pages (third-party).

### LVGL configuration

`include/lv_conf.h` is pulled in via `-D LV_CONF_INCLUDE_SIMPLE=1` (`include/`
is added to the search path explicitly since PlatformIO doesn't do it for
lib_deps sources like lvgl itself). The pinned lvgl version (9.2.2) is a
config-compatible match for this v9.2.0-format `lv_conf.h` — don't bump it
without checking that.

`src/fonts/lv_font_it_{10,12,14,28}.c` (1.54) and `{16,20,24,48}.c` (3.97's
layout; unreferenced on the 1.54, so not linked there) (declared in `include/fonts_it.h`,
used everywhere `ui_epaper.cpp` sets a text font) are custom-built
replacements for lvgl's own `lv_font_montserrat_{10,12,14,28}` — the
built-in ones only bake in ASCII, so accented letters (e.g. Italian's è à ò)
silently render blank with them. Regenerated with `lv_font_conv` (via `npx`)
from the exact same source `Montserrat-Medium.ttf` +
`FontAwesome5-Solid+Brands+Regular.woff` lvgl itself ships at
`<lvgl_lib_dep>/scripts/built_in_font/`, same options as each original font's
own `Opts:` header-comment (still present, unchanged, at the top of each
generated file here) plus one added `-r 0xC0-0xFF` range on the Montserrat
font to pull in Latin-1 Supplement — same size/metrics/icon-glyph coverage,
so they're drop-in replacements for the originals. If a call site needs a
font size outside this set of four, either regenerate one more this same way
or fall back to the plain `lv_font_montserrat_<size>` (which will just be
missing accented glyphs for that one spot).

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

#pragma once

#include <cstdint>

// Idle-timeout deep sleep - after the configurable idle timeout (see
// sleep_get_idle_timeout_minutes()/sleep_set_idle_timeout_minutes() below;
// IDLE_TIMEOUT_MIN_DEFAULT in sleep.cpp is 30 minutes) with no button press
// and no web-server request, the device deep-sleeps to save battery, waking
// only on the Select/PWR button (see display_epaper.cpp's PWR_BUTTON_PIN) -
// BOOT/Next deliberately left out of the wake mask so
// ui_show_sleep_screen()'s "Hold Select to wake" stays the one true way to
// wake it. Ported from the pala_note sibling project's
// enterUltraSleep()/resetActivity() pair - same board family (same
// PWR_HOLD_PIN battery latch, same battery ADC pin, same button GPIOs), so
// the same deep-sleep/ext1-wakeup approach applies unchanged.
//
// Waking from deep sleep is a full MCU reset - setup() runs again from
// scratch, same as a fresh boot/power-on. There's no state to restore
// here, so unlike pala_note this doesn't need to inspect
// esp_sleep_get_wakeup_cause() - the normal boot path already re-scans the
// SD card, reconnects WiFi, and rebuilds the main screen.

// Call whenever user or network activity happens - resets the idle clock.
// Cheap (one millis() call). Wired into ui_process_input() (button edges)
// and web_server.cpp (served HTTP requests); call it directly too if a
// future caller needs to count some other activity as "not idle".
void sleep_reset_activity();

// Call once per loop() iteration, after everything else that could count
// as activity this pass (ui_process_input(), web_server_handle()) has run.
// Deep-sleeps (never returns) once idle for longer than
// sleep_get_idle_timeout_minutes(), unless ui.h's ui_is_sleep_blocked()
// says a foreground operation (recording/playing/transcribing) is in
// progress - mirrors pala_note's loop() guard against sleeping
// mid-recording/transfer.
void sleep_process_idle();

// Current idle timeout in minutes - persisted in NVS (Preferences,
// namespace "annota", same as transcribe_openai.cpp's API key), so it
// survives reboots and deep-sleep wakeups. Lazily loads from NVS on first
// call (IDLE_TIMEOUT_MIN_DEFAULT in sleep.cpp if nothing was ever saved),
// then serves the cached value - cheap enough to call from
// sleep_process_idle() every loop() iteration. Exposed so the web UI's
// Settings page (web_server.cpp) can show the current value on its
// slider.
uint16_t sleep_get_idle_timeout_minutes();

// Sets the idle timeout in minutes, clamped to
// [IDLE_TIMEOUT_MIN_MIN, IDLE_TIMEOUT_MIN_MAX] (sleep.cpp), and persists it
// to NVS immediately so it survives a reboot. Wired to the web UI's
// Settings page slider (web_server.cpp's handle_settings_set_idle_timeout()) -
// takes effect on the very next sleep_process_idle() call, no reboot
// needed.
void sleep_set_idle_timeout_minutes(uint16_t minutes);

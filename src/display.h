#pragma once

#include <cstdint>

// Panel resolution: 200x200 square mono e-paper (Waveshare
// ESP32-S3-ePaper-1.54) - see display_epaper.cpp for the panel driver.
constexpr uint16_t SCREEN_W = 200;
constexpr uint16_t SCREEN_H = 200;

// Brings up just the display panel. Call first, before anything else
// touches the screen or SPI.
void display_init_panel();

// Brings up input (the two onboard buttons) and LVGL's display + input
// device. Call once storage.h's SD scan (if any) is done - the call order
// is kept the same as it always was so main.cpp's setup() doesn't need to
// special-case it.
void display_init_input();

// No shared SPI peripheral to hand off on this board (the e-paper panel and
// the SD card are on separate dedicated peripherals) - no-op stubs so
// callers (web_server.cpp, transcribe.cpp) that bracket their SD access
// with these don't need a special case.
void display_suspend_touch();
void display_resume_touch();

// The board's two onboard buttons, driving ui_epaper.cpp's list/menu nav.
// kNext advances the current selection/menu option, kSelect opens/confirms
// it (short press) or backs out of it (long press). See ui_epaper.cpp for
// the actual nav scheme built on top of these.
enum class DisplayButton { kNext, kSelect };
enum class DisplayButtonEvent { kNone, kShort, kLong };

// Debounced, edge-triggered: returns kShort/kLong at most once per
// press/hold, kNone otherwise (including for the whole duration of a held
// press before it crosses the long-press threshold). Call once per button
// per loop() iteration - ui_epaper.cpp's ui_process_input() does, no other
// caller needed.
DisplayButtonEvent display_button_poll(DisplayButton b);

// Raw current pin state (active-low), no debounce/edge logic - unlike
// display_button_poll(), safe to call any number of times per loop()
// iteration without consuming/altering that function's own per-button
// press-tracking state. Used only to detect whether both buttons are
// currently held down together, ahead of calling display_button_poll() -
// see display_forget_wifi_combo_poll()'s comment for why that ordering
// matters.
bool display_button_raw_pressed(DisplayButton b);

// Edge-triggered, fires (returns true) exactly once per qualifying hold,
// the moment both buttons have been held down together continuously for
// FORGET_WIFI_COMBO_HOLD_MS (display_epaper.cpp) - false otherwise,
// including for the whole duration of the hold before that threshold and
// after it fires, until both are released and pressed together again.
// Tracks its own independent hold timer via display_button_raw_pressed()
// rather than display_button_poll()'s per-button one, so a long
// two-button hold doesn't also fire a spurious single-button kLong at
// that state machine's own (shorter) long-press threshold along the way -
// ui_epaper.cpp's ui_process_input() checks this before polling either
// button individually, and skips that individual poll entirely for any
// iteration where both are currently held, so a hold that's abandoned
// before 5s leaves no half-consumed single-button press behind either.
// Wired to the "hold both buttons 5s to forget the saved WiFi network and
// reboot" gesture - ui_epaper.cpp shows an on-screen confirm/cancel menu
// on the fire edge rather than acting immediately, same as every other
// destructive action in this UI (see Screen::kDeleteConfirm).
bool display_forget_wifi_combo_poll();

#pragma once

#include <cstdint>

// Logical screen resolution LVGL/ui_epaper.cpp see, board-selected via
// platformio.ini's BOARD_EPAPER_* flag (same mechanism as transcribe.h's
// AI_PROVIDER_* - see its top comment). 154: 200x200 square mono
// e-paper, see display_epaper.cpp. 397: the panel's physical RAM is
// 800x480 (landscape) at the controller level, but it's mounted/used in
// portrait - display_epaper397.cpp rotates every pixel 90 degrees between
// this logical 480x800 canvas and the panel's physical 800x480 buffer, so
// SCREEN_W/SCREEN_H here are the rotated (portrait) dimensions, not the
// panel's native ones.
#if defined(BOARD_EPAPER_154)
constexpr uint16_t SCREEN_W = 200;
constexpr uint16_t SCREEN_H = 200;
#elif defined(BOARD_EPAPER_397)
constexpr uint16_t SCREEN_W = 480;
constexpr uint16_t SCREEN_H = 800;
#else
#error "No BOARD_EPAPER_* build flag defined - add one (e.g. -D BOARD_EPAPER_154=1) to platformio.ini's build_flags."
#endif

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

// The board's onboard buttons, driving ui_epaper.cpp's list/menu nav.
// kNext advances the current selection/menu option, kSelect opens/confirms
// it (short press) or backs out of it (long press). kPrev moves the
// selection backward - only physically wired on the 397 board (its Up
// button); the 154 driver reports kNone for it permanently, since that
// board has no third button. kBoot exists only so
// display_forget_wifi_combo_poll() can name "the other combo button" per
// board (397: its dedicated Boot button; 154: aliased to the same pin as
// kNext, since that board has no separate Boot button and the combo has
// always been its two buttons held together - see each driver's own
// buttonStates table). See ui_epaper.cpp for the actual nav scheme built
// on top of these.
enum class DisplayButton { kNext, kSelect, kPrev, kBoot };
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
// press-tracking state. Used only to detect whether kBoot and kSelect are
// currently held down together, ahead of calling display_button_poll() -
// see display_forget_wifi_combo_poll()'s comment for why that ordering
// matters.
bool display_button_raw_pressed(DisplayButton b);

// Edge-triggered, fires (returns true) exactly once per qualifying hold,
// the moment kBoot and kSelect have been held down together continuously
// for FORGET_WIFI_COMBO_HOLD_MS (display_epaper.cpp) - false otherwise,
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
// Wired to the "hold Boot+Select 5s to forget the saved WiFi network and
// reboot" gesture - ui_epaper.cpp shows an on-screen confirm/cancel menu
// on the fire edge rather than acting immediately, same as every other
// destructive action in this UI (see Screen::kDeleteConfirm).
bool display_forget_wifi_combo_poll();

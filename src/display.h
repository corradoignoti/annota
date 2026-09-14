#pragma once

#include <cstdint>

#include <GxEPD2_BW.h>
#include <epd/GxEPD2_154_D67.h>

// Panel resolution: 200x200 square mono e-paper (Waveshare
// ESP32-S3-ePaper-1.54) - see display_epaper.cpp for the panel driver.
constexpr uint16_t SCREEN_W = 200;
constexpr uint16_t SCREEN_H = 200;

// The shared GxEPD2 display object - GxEPD2_BW's second template param
// (the page height) is the full panel HEIGHT rather than some fraction of
// it, so GxEPD2 holds one full 200x200 1bpp frame buffer internally and no
// firstPage()/nextPage() paged-drawing loop is needed (that pattern only
// exists to save RAM on tighter MCUs than this board's). ui_epaper.cpp
// draws into it directly via Adafruit_GFX calls (it publicly inherits from
// Adafruit_GFX), then calls display_present() below to push the frame to
// the panel.
using EpdDisplay = GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT>;
EpdDisplay &display_epd();

// Brings up just the display panel. Call first, before anything else
// touches the screen or SPI.
void display_init_panel();

// Brings up input (the two onboard buttons). Call once storage.h's SD scan
// (if any) is done - the call order is kept the same as it always was so
// main.cpp's setup() doesn't need to special-case it.
void display_init_input();

// Pushes whatever's currently drawn into display_epd()'s buffer to the
// panel. partial=true uses the SSD1681's faster/lower-ghosting waveform -
// still a full-panel RAM rewrite either way, not a sub-rect update (see
// display_epaper.cpp's comment) - and is what every UI update after the
// first boot paint uses; partial=false is only for that first paint.
// Synchronous - safe to call from anywhere, including code that's already
// blocking loop() (e.g. wifi_manager.cpp's captive-portal setup), since
// there's no timer/event pump to reenter.
void display_present(bool partial);

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

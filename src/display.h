#pragma once

#include <cstdint>

// Panel resolution. esp32-cyd: 240x320 portrait ILI9341. esp32-s3-epaper154:
// 200x200 square mono e-paper - see platformio.ini for which BOARD_* build
// flag selects which, and display.cpp/display_epaper.cpp for which of the
// two implements everything below.
#ifdef BOARD_ESP32S3_EPAPER154
constexpr uint16_t SCREEN_W = 200;
constexpr uint16_t SCREEN_H = 200;
#else
constexpr uint16_t SCREEN_W = 240;
constexpr uint16_t SCREEN_H = 320;
#endif

// Brings up just the display panel. Call first, before anything else
// touches the screen or SPI.
void display_init_panel();

// Brings up input (touch on esp32-cyd, running one-time calibration if none
// is cached yet; the two onboard buttons on esp32-s3-epaper154) and LVGL's
// display + input device. Call once storage.h's SD scan (if any) is done -
// on esp32-cyd, touch and SD share the ESP32's second SPI peripheral (the
// display panel occupies the other one full-time), so SD needs to finish
// with it first; see storage.cpp for why. No such constraint on
// esp32-s3-epaper154 (SD is on its own dedicated SDMMC peripheral there),
// but the call order is kept the same for both boards so main.cpp doesn't
// need a board-specific setup() path.
void display_init_input();

// esp32-cyd only: releases touch's hold on the shared SPI peripheral so
// storage.h's sd_begin() can borrow it (e.g. for a web_server.cpp file
// operation after boot). Touch reads return no-press while suspended. Pair
// with display_resume_touch() once the SD operation is done - don't leave
// touch suspended, the UI reads garbage until resumed. No-op on
// esp32-s3-epaper154 - SD there is on its own dedicated SDMMC peripheral,
// nothing to hand off - but every caller (web_server.cpp, transcribe.cpp)
// calls this unconditionally around its SD access, so it needs to exist
// either way.
void display_suspend_touch();

// Reclaims the shared SPI peripheral for touch after display_suspend_touch().
// No-op on esp32-s3-epaper154 - see display_suspend_touch()'s comment.
void display_resume_touch();

// esp32-cyd only: backlight on/off (TFT_BL pin). Used by antiburn.cpp to
// blank the screen after an idle timeout - cuts power draw and how long a
// static image stays lit. Idempotent: safe to call every loop() tick
// regardless of current state. No-op on esp32-s3-epaper154 - e-paper holds
// its image with no backlight to blank, and antiburn.cpp's own board gate
// means this is never actually called there anyway (see antiburn.cpp).
void display_set_backlight(bool on);

// Milliseconds since the last touch contact (esp32-cyd) or button press
// (esp32-s3-epaper154), i.e. how long the screen's been idle. antiburn.cpp
// polls this to decide when to blank/wake (esp32-cyd only - see its board
// gate).
uint32_t display_idle_ms();

#ifdef BOARD_ESP32S3_EPAPER154
// esp32-s3-epaper154 only: the board's two onboard buttons, driving
// ui_epaper.cpp's list/menu nav in place of touch - kNext advances the
// current selection/menu option, kSelect opens/confirms it (short press)
// or backs out of it (long press). See ui_epaper.cpp for the actual nav
// scheme built on top of these.
enum class DisplayButton { kNext, kSelect };
enum class DisplayButtonEvent { kNone, kShort, kLong };

// Debounced, edge-triggered: returns kShort/kLong at most once per
// press/hold, kNone otherwise (including for the whole duration of a held
// press before it crosses the long-press threshold). Call once per button
// per loop() iteration - ui_epaper.cpp's ui_process_input() does, no other
// caller needed.
DisplayButtonEvent display_button_poll(DisplayButton b);
#endif

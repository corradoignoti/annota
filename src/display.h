#pragma once

#include <cstdint>

// Panel resolution, portrait orientation.
constexpr uint16_t SCREEN_W = 240;
constexpr uint16_t SCREEN_H = 320;

// Brings up just the TFT panel. Call first, before anything else touches
// the screen or SPI.
void display_init_panel();

// Brings up the touch controller (running one-time calibration if none is
// cached yet) and LVGL's display + input device. Call once storage.h's SD
// scan (if any) is done - touch and SD both run on the ESP32's second SPI
// peripheral (the display panel occupies the other one full-time), so SD
// needs to finish with it first. See storage.cpp for why.
void display_init_input();

// Releases touch's hold on the shared SPI peripheral so storage.h's
// sd_begin() can borrow it (e.g. for a web_server.cpp file operation after
// boot). Touch reads return no-press while suspended. Pair with
// display_resume_touch() once the SD operation is done - don't leave touch
// suspended, the UI reads garbage until resumed.
void display_suspend_touch();

// Reclaims the shared SPI peripheral for touch after display_suspend_touch().
void display_resume_touch();

// Backlight on/off (TFT_BL pin). Used by antiburn.cpp to blank the screen
// after an idle timeout - cuts power draw and how long a static image
// stays lit. Idempotent: safe to call every loop() tick regardless of
// current state.
void display_set_backlight(bool on);

// Milliseconds since the last touch contact, i.e. how long the screen's
// been idle. antiburn.cpp polls this to decide when to blank/wake.
uint32_t display_idle_ms();

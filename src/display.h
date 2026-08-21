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

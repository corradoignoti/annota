#pragma once

#include <cstdint>

// Battery charge estimation - reads the board's own voltage-divider ADC pin
// (GPIO4 on this Waveshare schematic) and maps the reading onto a Li-ion
// discharge curve. No charge-detect pin is broken out on this board, so
// there's no "currently charging" state here, only a percentage - see
// ui.h's ui_set_battery_percent() for how it reaches the header.

// Raw battery terminal voltage in millivolts, corrected for the board's
// 200K/200K divider (the ADC pin itself only ever sees half of it). Uses
// analogReadMilliVolts()'s factory ADC calibration rather than a raw
// analogRead() count, so this stays accurate without per-board trimming.
uint16_t battery_read_millivolts();

// Estimated charge, 0-100, from battery_read_millivolts() linearly mapped
// across a typical Li-ion's usable range (3.30V empty, 4.20V full under
// light load) and clamped to that range - not a fuel-gauge IC reading, just
// a voltage-based estimate, so expect it to sag under load (e.g. mid
// playback) and recover at rest.
uint8_t battery_read_percent();

#pragma once

#include <cstdint>

// Battery charge estimation. On the 1.54 it reads the board's own
// voltage-divider ADC pin (GPIO4 on this Waveshare schematic) and maps the
// reading onto a Li-ion discharge curve - the comments below describe that
// path. On the 3.97 both calls read the AXP2101 PMIC's own measurement and
// fuel gauge instead (see battery.cpp). No charge-detect pin is broken out on this board, so
// there's no "currently charging" state here, only a percentage - see
// ui.h's ui_set_battery_percent() for how it reaches the header.

// Call once at the very start of setup(), before display_init_panel(): on
// PMIC boards (the 3.97) it brings the PMIC up and makes sure the
// peripheral rails (e-paper panel included) are on. A no-op on the 1.54.
void battery_init();

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

// Call right before esp_deep_sleep_start() (sleep.cpp), after everything
// else that still talks to the panel/codec. PMIC boards (the 3.97): turns
// the ALDO1-3 peripheral rails battery_init() enabled back off - left on,
// they keep the e-paper panel's supply (and whatever else hangs off them)
// drawing from the battery the whole time the ESP32-S3 sleeps. The next
// boot's battery_init() turns them on again. A no-op on the 1.54.
void battery_prepare_deep_sleep();

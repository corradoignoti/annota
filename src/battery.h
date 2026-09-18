#pragma once

#include <cstdint>

// Battery charge estimation - on the 154 board, reads its voltage-divider
// ADC pin (GPIO4 on that Waveshare schematic) and maps the reading onto a
// Li-ion discharge curve. No charge-detect pin is broken out on that
// board, so there's no "currently charging" state here, only a
// percentage - see ui.h's ui_set_battery_percent() for how it reaches the
// header.
//
// The 397 board has no GPIO battery-ADC pin at all - both functions
// return a stub value on that board. Confirmed via 78/xiaozhi-esp32's
// factory-shipped board source (see display_epaper397.cpp's epd_power_on()
// comment for that same source's role in the panel-power fix): its
// GetBatteryLevel() reads the AXP2101 PMIC's own fuel gauge over I2C
// (pmic_->GetBatteryLevel()), not a raw voltage-divider pin - implementing
// this for real would mean an AXP2101 register read (battery %/voltage
// registers), not analogReadMilliVolts(), a different shape than the 154
// board's function bodies below. Left stubbed since wiring that up is a
// feature addition beyond this port's scope, not because the pin is
// unknown.

// Returned by battery_read_percent() when the board has no confirmed
// battery-voltage ADC pin - callers (ui_epaper.cpp's
// ui_set_battery_percent()) treat this as "hide the battery readout"
// rather than displaying a bogus percentage.
constexpr uint8_t BATTERY_PERCENT_UNKNOWN = 0xFF;

// Raw battery terminal voltage in millivolts, corrected for the board's
// 200K/200K divider (the ADC pin itself only ever sees half of it). Uses
// analogReadMilliVolts()'s factory ADC calibration rather than a raw
// analogRead() count, so this stays accurate without per-board trimming.
// Returns 0 on boards with no confirmed ADC pin - see this file's top
// comment.
uint16_t battery_read_millivolts();

// Estimated charge, 0-100, from battery_read_millivolts() linearly mapped
// across a typical Li-ion's usable range (3.30V empty, 4.20V full under
// light load) and clamped to that range - not a fuel-gauge IC reading, just
// a voltage-based estimate, so expect it to sag under load (e.g. mid
// playback) and recover at rest. Returns BATTERY_PERCENT_UNKNOWN on boards
// with no confirmed ADC pin - see this file's top comment.
uint8_t battery_read_percent();

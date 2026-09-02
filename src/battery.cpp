#include "battery.h"

#include <Arduino.h>

// GPIO4: battery cell voltage through this board's 200K/200K divider
// (Waveshare schematic) - the ADC pin itself sees only half the real
// voltage, corrected below. Left at the core's default attenuation
// (already full 0-3.3V range on this chip), so nothing to configure at
// startup - no battery_init() needed.
static const int BAT_ADC_PIN = 4;

// Rough Li-ion discharge bounds under light load - matches Waveshare's own
// reference firmware for this board. Not a true fuel-gauge curve (real
// cells sag nonlinearly near empty), just enough for a header estimate.
static const float BAT_MIN_MV = 3300.0f;
static const float BAT_MAX_MV = 4200.0f;

uint16_t battery_read_millivolts() {
    // analogReadMilliVolts() applies the SoC's factory ADC calibration
    // (eFuse-stored) instead of a linear guess from a raw analogRead()
    // count - meaningfully more accurate for a voltage this close to the
    // ADC's rails. x2 undoes the 200K/200K divider above.
    return (uint16_t)(analogReadMilliVolts(BAT_ADC_PIN) * 2);
}

uint8_t battery_read_percent() {
    float mv = (float)battery_read_millivolts();
    float pct = (mv - BAT_MIN_MV) / (BAT_MAX_MV - BAT_MIN_MV) * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)(pct + 0.5f);
}

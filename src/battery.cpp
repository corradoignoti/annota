#include "battery.h"

#include <Arduino.h>

// GPIO4: battery cell voltage through this board's 200K/200K divider
// (Waveshare schematic) - the ADC pin itself sees only half the real
// voltage, corrected below. Left at the core's default attenuation
// (already full 0-3.3V range on this chip), so nothing to configure at
// startup - no battery_init() needed.
static const int BAT_ADC_PIN = 4;

uint16_t battery_read_millivolts() {
    // analogReadMilliVolts() applies the SoC's factory ADC calibration
    // (eFuse-stored) instead of a linear guess from a raw analogRead()
    // count - meaningfully more accurate for a voltage this close to the
    // ADC's rails. x2 undoes the 200K/200K divider above.
    return (uint16_t)(analogReadMilliVolts(BAT_ADC_PIN) * 2);
}

uint8_t battery_read_percent() {
    // Piecewise-linear approximation of a Li-ion discharge curve (steeper
    // in the 3.60-4.10V band, flatter below it where real cells sag),
    // shifted 100mV down from the textbook 3.30-4.20V Li-ion range - this
    // board's divider/ADC chain measures a full (charger-terminated,
    // resting) cell at ~4.10V, not 4.20V, so the un-shifted curve topped
    // out at 90% and never reached 100.
    float v = (float)battery_read_millivolts() / 1000.0f;
    float pct;
    if (v >= 4.10f) pct = 100.0f;
    else if (v >= 4.00f) pct = 90.0f + (v - 4.00f) * (10.0f / 0.10f);
    else if (v >= 3.90f) pct = 75.0f + (v - 3.90f) * (15.0f / 0.10f);
    else if (v >= 3.75f) pct = 50.0f + (v - 3.75f) * (25.0f / 0.15f);
    else if (v >= 3.60f) pct = 25.0f + (v - 3.60f) * (25.0f / 0.15f);
    else if (v >= 3.40f) pct = 5.0f + (v - 3.40f) * (20.0f / 0.20f);
    else if (v >= 3.20f) pct = (v - 3.20f) * (5.0f / 0.20f);
    else pct = 0.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)(pct + 0.5f);
}

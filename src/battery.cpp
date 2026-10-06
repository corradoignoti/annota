#include "battery.h"

#include <Arduino.h>

#include "board.h"

#if BOARD_HAS_PMIC
// -----------------------------------------------------------------------
// 3.97: the AXP2101-class PMIC ("TG28" on Waveshare's docs, register-
// compatible) measures the cell and runs its own fuel gauge (enabled by
// default after power-on), so both readings below are plain register reads
// over the shared I2C bus (board.h's I2C_SDA_PIN/I2C_SCL_PIN - also the
// ES8311's, see speaker.cpp; Wire.begin() on the same pins twice is a
// harmless no-op). battery_init() powers the peripheral rails (e-paper
// panel included) and brings the gauge up; the readings also init the
// gauge lazily in case it wasn't called.
// -----------------------------------------------------------------------
#define XPOWERS_CHIP_AXP2101
#include <Wire.h>
#include <XPowersLib.h>

static XPowersPMU pmu;
static bool pmuReady = false;

static bool pmu_reg_read(uint8_t reg, uint8_t &val) {
    Wire.beginTransmission(AXP2101_SLAVE_ADDRESS);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)AXP2101_SLAVE_ADDRESS, (uint8_t)1) != 1) return false;
    val = Wire.read();
    return true;
}

static bool pmu_reg_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(AXP2101_SLAVE_ADDRESS);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// Peripheral rails ALDO1-3 at 3.3 V and on, same as Waveshare's factory
// firmware (axp_prot.cpp's axp_init()). ALDO3 is the e-paper panel's
// supply (epaper_port.c's EPD_Power_ON()) - and that factory firmware
// switches it *off* whenever its screen idles (EPD_Sleep()), a setting the
// PMIC keeps across ESP32 resets and reflashes since it never loses power
// itself. So this must happen on every boot, before display_init_panel().
// Raw register writes rather than XPowersLib: the library's begin()
// refuses to do anything unless the chip-ID register reads exactly 0x4A,
// and the panel rail must not hinge on that (the factory code ignores a
// failed begin() too). DC1 (the ESP32-S3 itself) is left alone.
static const uint8_t ALDO_ONOFF = 0x90; // bit0 ALDO1, bit1 ALDO2, bit2 ALDO3

static void pmu_enable_rails() {
    static const uint8_t ALDO_VOL[] = {0x92, 0x93, 0x94};  // ALDO1..3 voltage
    static const uint8_t VOL_3V3 = (3300 - 500) / 100;     // 100 mV steps from 0.5 V
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    bool ok = true;
    for (uint8_t reg : ALDO_VOL) {
        uint8_t v = 0;
        ok &= pmu_reg_read(reg, v) && pmu_reg_write(reg, (v & 0xE0) | VOL_3V3);
    }
    uint8_t on = 0;
    ok &= pmu_reg_read(ALDO_ONOFF, on) && pmu_reg_write(ALDO_ONOFF, on | 0x07);
    Serial.printf("battery: PMIC rails ALDO1-3 %s (ALDO on/off was 0x%02X)\n", ok ? "on" : "FAILED", on);
    delay(10); // let the panel rail settle before its reset (factory firmware does the same)
}

static bool pmu_begin() {
    if (pmuReady) return true;
    if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN)) {
        Serial.println("battery: AXP2101 not recognized - no battery readings");
        return false;
    }
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmuReady = true;
    return true;
}

void battery_init() {
    pmu_enable_rails();
    pmu_begin();
}

void battery_prepare_deep_sleep() {
    // Same raw-register approach as pmu_enable_rails(), and the same thing
    // Waveshare's factory firmware does to ALDO3 whenever its screen idles.
    uint8_t on = 0;
    bool ok = pmu_reg_read(ALDO_ONOFF, on) && pmu_reg_write(ALDO_ONOFF, on & ~0x07);
    Serial.printf("battery: PMIC rails ALDO1-3 %s\n", ok ? "off" : "off FAILED");
    Serial.flush();
}

uint16_t battery_read_millivolts() {
    if (!pmu_begin()) return 0;
    return pmu.getBattVoltage(); // 0 if no cell is connected
}

uint8_t battery_read_percent() {
    if (!pmu_begin()) return 0;
    int pct = pmu.getBatteryPercent();
    // -1: no cell connected, i.e. running off USB alone - shown as full
    // rather than as an empty battery about to die.
    if (pct < 0) return 100;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

#else
// BAT_ADC_PIN (board.h, GPIO4 on the 1.54): battery cell voltage through
// this board's 200K/200K divider (Waveshare schematic) - the ADC pin itself
// sees only half the real voltage, corrected below. Left at the core's
// default attenuation (already full 0-3.3V range on this chip), so nothing
// to configure at startup.
void battery_init() {}

void battery_prepare_deep_sleep() {}

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

#endif // BOARD_HAS_PMIC

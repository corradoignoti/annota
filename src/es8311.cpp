#include "es8311.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t ES8311_ADDR = 0x18;

// Register addresses - see es8311.h's top comment for where these come
// from. Only the ones this driver actually touches.
constexpr uint8_t REG_RESET = 0x00;
constexpr uint8_t REG_CLK01 = 0x01;
constexpr uint8_t REG_CLK02 = 0x02;
constexpr uint8_t REG_CLK03 = 0x03;
constexpr uint8_t REG_CLK04 = 0x04;
constexpr uint8_t REG_CLK05 = 0x05;
constexpr uint8_t REG_CLK06 = 0x06;
constexpr uint8_t REG_CLK07 = 0x07;
constexpr uint8_t REG_CLK08 = 0x08;
constexpr uint8_t REG_SDPIN = 0x09;
constexpr uint8_t REG_SDPOUT = 0x0A;
constexpr uint8_t REG_SYSTEM_0D = 0x0D;
constexpr uint8_t REG_SYSTEM_0E = 0x0E;
constexpr uint8_t REG_SYSTEM_12 = 0x12;
constexpr uint8_t REG_SYSTEM_13 = 0x13;
constexpr uint8_t REG_ADC_1C = 0x1C;
constexpr uint8_t REG_DAC_MUTE_31 = 0x31;
constexpr uint8_t REG_DAC_VOL_32 = 0x32;
constexpr uint8_t REG_DAC_37 = 0x37;

bool write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool read_reg(uint8_t reg, uint8_t &val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false; // repeated start
    if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return false;
    val = Wire.read();
    return true;
}

// Read-modify-write helper for the several registers below that only own
// part of their byte - mirrors the reference driver's own pattern.
bool rmw_reg(uint8_t reg, uint8_t clearMask, uint8_t setBits) {
    uint8_t v;
    if (!read_reg(reg, v)) return false;
    v = (v & clearMask) | setBits;
    return write_reg(reg, v);
}

} // namespace

bool es8311_init(uint32_t sampleRate, int volume) {
    (void)sampleRate; // only relevant to the coefficient table this driver
                       // doesn't need - see es8311.h's top comment.
    bool ok = true;

    // Reset to default. CSM_ON (power-on of the chip's internal state
    // machine) is deliberately NOT written here - see the bottom of this
    // function for why it has to come last.
    ok &= write_reg(REG_RESET, 0x1F);
    delay(20);
    ok &= write_reg(REG_RESET, 0x00);

    // Clock: MCLK from the dedicated MCLK pin (not derived from BCLK),
    // enable all internal clocks, no inversion.
    ok &= write_reg(REG_CLK01, 0x3F);
    // Fixed 256x-MCLK coefficients (pre_div=1, pre_multi=0, adc_div=1,
    // dac_div=1, fs_mode=0, lrck=0x00ff, bclk_div=4, osr=0x10) - identical
    // for every standard MP3 rate at this ratio, see es8311.h.
    ok &= rmw_reg(REG_CLK02, 0x07, 0x00);
    ok &= write_reg(REG_CLK03, 0x10); // fs_mode<<6 | adc_osr
    ok &= write_reg(REG_CLK04, 0x10); // dac_osr
    ok &= write_reg(REG_CLK05, 0x00); // (adc_div-1)<<4 | (dac_div-1)
    ok &= rmw_reg(REG_CLK06, 0xE0, 0x03); // bclk_div-1
    ok &= rmw_reg(REG_CLK07, 0xC0, 0x00); // lrck_h
    ok &= write_reg(REG_CLK08, 0xFF); // lrck_l

    // Format: slave mode, 16-bit in/out serial port.
    ok &= rmw_reg(REG_RESET, 0xBF, 0x00);
    ok &= write_reg(REG_SDPIN, 0x0C); // 16-bit
    ok &= write_reg(REG_SDPOUT, 0x0C); // 16-bit

    // Set volume before power-up, matching the order below (esphome's
    // own es8311 driver - github.com/esphome/esphome, components/es8311/
    // es8311.cpp - does the same, and is a good reference here since it's
    // deployed across many real ES8311 boards, unlike Espressif's own
    // reference driver which nobody here could get real hardware to sing
    // from).
    es8311_set_volume(volume);

    // Power up analog circuitry. A previous version of this driver tried
    // to be clever here - split this 0x0D write into a two-step VMID
    // ramp on a theory that bit 2 (PDN_VREF) is polarity-inverted from
    // its siblings - but esphome's driver above uses the plain 0x01 this
    // reverted back to, and still produced no audio, so that theory (and
    // the "fix" it justified) is retired: treat 0x0D like every other
    // PDN_* register here, one write, no ramp.
    ok &= write_reg(REG_SYSTEM_0D, 0x01);
    ok &= write_reg(REG_SYSTEM_0E, 0x02);
    ok &= write_reg(REG_SYSTEM_12, 0x00);
    ok &= write_reg(REG_SYSTEM_13, 0x10); // enable output to HP drive
    ok &= write_reg(REG_ADC_1C, 0x6A); // unused ADC path, quiesced
    ok &= write_reg(REG_DAC_37, 0x08); // bypass DAC equalizer

    // CSM_ON (bit 7 of the RESET register, chip state machine power-on)
    // has to be the LAST write, after every analog/format register above
    // is already in place - the state machine drives its own internal
    // power-up ramp off whatever those registers say the moment it's
    // switched on, so switching it on any earlier (this driver used to,
    // right after the two reset writes at the top) races that ramp
    // against registers this function hasn't gotten to yet. Matches
    // esphome's driver's own ordering.
    ok &= write_reg(REG_RESET, 0x80);

    es8311_set_mute(false);

    return ok;
}

void es8311_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    uint8_t reg32 = (volume == 0) ? 0 : (uint8_t)(((volume * 256) / 100) - 1);
    write_reg(REG_DAC_VOL_32, reg32);
}

void es8311_set_mute(bool mute) {
    rmw_reg(REG_DAC_MUTE_31, (uint8_t)~0x60, mute ? 0x60 : 0x00);
}

#pragma once

#include <cstdint>

// -----------------------------------------------------------------------
// Minimal driver for the ES8311 audio codec - esp32-s3-epaper154's onboard
// codec (see speaker.h/.cpp), used by nothing else. Drives both the DAC
// (speaker playback) and ADC (mic recording) paths - the two are never
// active at once on this board (speaker.cpp's mic_start_recording() always
// stops playback first), so es8311_set_mic_enabled() below just toggles
// the ADC path on top of whatever es8311_init() already set up for
// playback, rather than needing a real simultaneous-duplex mode. Plain
// functions over static state rather than a handle, matching storage.cpp's
// style - there's only ever one ES8311 on this board.
//
// I2C-only; the codec's I2S side is speaker.cpp's job. Register map and
// the clock-divider coefficients are ported from Espressif's own
// Apache-2.0 driver (espressif/esp-bsp, components/es8311), trimmed to the
// one clock plan speaker.cpp always uses: MCLK = 256 x sample rate, fed
// from the ESP32's I2S peripheral on the dedicated MCLK pin (not derived
// from BCLK). Every standard MP3 sample rate (8k/11.025k/12k/16k/22.05k/
// 24k/32k/44.1k/48k) needs the *same* register values at that fixed
// 256x ratio - Espressif's own coefficient table has one identical row
// per rate in that family - so unlike the original driver this doesn't
// need a lookup table, and switching rates between tracks needs no
// codec-side register changes at all (see es8311_init()'s comment).
//
// I2C address is fixed at 0x18 (CE pin tied low - labeled directly on the
// schematic), on the same bus as the RTC/SHTC3 (GPIO47 SDA / GPIO48 SCL -
// speaker.cpp owns calling Wire.begin() for it, since nothing else on this
// board uses I2C yet).

// Resets and configures the codec for 16-bit stereo I2S playback, unmuted
// at `volume` (0-100). Must run after Wire.begin() on the codec's bus.
// sampleRate only has to be *a* valid rate the ESP32-S3 side will actually
// clock (used solely to size the MCLK/BCLK ratio the codec is told to
// expect - see the top comment) - it does not need to match the first
// file's actual rate, since that ratio is the same for every rate in the
// family. Returns false on I2C error (codec not responding).
bool es8311_init(uint32_t sampleRate, int volume);

// 0 (silent) - 100 (max).
void es8311_set_volume(int volume);

void es8311_set_mute(bool mute);

// Enables/disables the ADC (mic) input path, independently of the DAC
// path es8311_init() already brought up - see es8311.cpp's
// es8311_set_mic_enabled() comment for the register sequence and where
// it's ported from (Espressif's own es8311 driver, which this whole file
// is otherwise not based on - that one only ever ran in playback mode).
void es8311_set_mic_enabled(bool enable);

// PGA gain, 0 (0dB) through 7 (42dB) in 6dB steps - matches the ES8311
// datasheet's REG16 field and Espressif's own es8311_mic_gain_t enum
// (ES8311_MIC_GAIN_0DB..ES8311_MIC_GAIN_42DB; that enum's MIN/MAX
// sentinels, -1 and 8, are never valid here).
void es8311_set_mic_gain(int gainCode);

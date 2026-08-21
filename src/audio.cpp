#include "audio.h"

#include <Arduino.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <driver/i2s.h>

#include "display.h"
#include "storage.h"

// -----------------------------------------------------------------------
// MP3 playback through the CYD's onboard speaker
//
// The speaker is wired to the ESP32's DAC2 pad (GPIO26) via a simple amp
// transistor. ESP8266Audio's AudioOutputI2S INTERNAL_DAC mode always
// enables *both* built-in DAC channels (GPIO25 and GPIO26) though, and
// GPIO25 is already spoken for - it's touch's XPT2046_CLK (see
// display.cpp). Right after bringing the output up we downgrade to the
// left channel only (GPIO26), which is the documented way (see
// i2s_set_dac_mode()'s header comment) to run just one DAC pin without
// touching the other.
// -----------------------------------------------------------------------

static AudioGeneratorMP3 *mp3 = nullptr;
static AudioFileSourceFS *file = nullptr;
static AudioOutputI2S *out = nullptr;
static bool holdingSdBus = false;
static bool paused = false;

static void release_bus_if_held() {
    if (holdingSdBus) {
        release_sd_bus();
        display_set_touch_enabled(true);
        holdingSdBus = false;
    }
}

void audio_stop() {
    if (mp3 && mp3->isRunning()) {
        mp3->stop();
    }
    delete mp3;
    mp3 = nullptr;
    delete file;
    file = nullptr;
    paused = false;
    release_bus_if_held();
}

bool audio_play(fs::FS &fs, const char *path) {
    Serial.println("[audio] audio_play: stop previous");
    audio_stop(); // clears any previous playback and releases whatever it held

    if (active_source_is_sd()) {
        Serial.println("[audio] audio_play: pausing touch, acquiring SD bus");
        display_set_touch_enabled(false);
        if (!acquire_sd_bus()) {
            Serial.println("[audio] audio_play: acquire_sd_bus failed");
            display_set_touch_enabled(true);
            return false;
        }
        holdingSdBus = true;
    }

    if (!out) {
        Serial.println("[audio] audio_play: creating AudioOutputI2S");
        out = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
        Serial.println("[audio] audio_play: out->begin()");
        out->begin();
        Serial.println("[audio] audio_play: i2s_set_dac_mode");
        i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN); // GPIO26 only
        out->SetOutputModeMono(true);
        Serial.println("[audio] audio_play: output ready");
    }

    Serial.println("[audio] audio_play: opening file source");
    file = new AudioFileSourceFS(fs, path);
    Serial.println("[audio] audio_play: mp3->begin");
    mp3 = new AudioGeneratorMP3();
    if (!mp3->begin(file, out)) {
        Serial.println("[audio] audio_play: mp3->begin failed");
        delete mp3;
        mp3 = nullptr;
        delete file;
        file = nullptr;
        release_bus_if_held();
        return false;
    }
    Serial.println("[audio] audio_play: mp3->begin succeeded");
    return true;
}

bool audio_is_playing() {
    return mp3 && mp3->isRunning();
}

void audio_pause() {
    if (mp3 && mp3->isRunning()) {
        paused = true;
    }
}

void audio_resume() {
    if (mp3 && mp3->isRunning()) {
        paused = false;
    }
}

bool audio_is_paused() {
    return paused;
}

void audio_loop() {
    static int debugCallsLeft = 5;
    if (mp3 && mp3->isRunning() && !paused) {
        bool trace = debugCallsLeft > 0;
        if (trace) {
            debugCallsLeft--;
            Serial.printf("[audio] audio_loop: calling mp3->loop() (#%d)\n", debugCallsLeft);
        }
        bool ok = mp3->loop();
        if (trace) {
            Serial.printf("[audio] audio_loop: mp3->loop() returned %d\n", ok);
        }
        if (!ok) {
            audio_stop(); // finished (or errored) - tears down + releases the bus/touch
        }
    }
}

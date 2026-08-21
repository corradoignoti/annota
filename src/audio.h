#pragma once

#include <FS.h>

// Starts playing the given file from fs (stops whatever's currently
// playing first). If fs is the SD card, touch input is paused for the
// whole playback - SD and touch share the ESP32's one spare SPI
// peripheral, and unlike the one-shot catalog scan, playback needs it for
// as long as the file plays (see storage.h's acquire_sd_bus()). Returns
// false if playback couldn't start.
bool audio_play(fs::FS &fs, const char *path);

// Stops playback if something is playing, releasing the SD bus/touch pause
// if it was holding either. Safe to call when already idle.
void audio_stop();

bool audio_is_playing();

// Pauses in place (decoder just stops being fed; the DAC goes quiet once
// whatever's already queued drains). audio_resume() picks up from the same
// spot. Both are no-ops if nothing is playing / not paused.
void audio_pause();
void audio_resume();
bool audio_is_paused();

// Pumps the decoder. Call every loop() iteration regardless of playback
// state - it's what notices a file finishing and releases the SD
// bus/touch pause afterwards.
void audio_loop();

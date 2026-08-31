#pragma once

// -----------------------------------------------------------------------
// Plays a root-level audio file through esp32-s3-epaper154's onboard
// speaker: an ES8311 I2C codec (es8311.h/.cpp) feeding an NS4150B amp over
// I2S, decoded with ESP8266Audio's AudioGeneratorMP3 - see speaker.cpp's
// top comment for the full pin/hardware rundown (from the board's own
// schematic) and why the codec/amp pins live where they do.
//
// esp32-cyd has no such hardware, and speaker.cpp itself is excluded from
// that env's build (see platformio.ini's board-split comment - it #includes
// ESP8266Audio/I2S headers that aren't in esp32-cyd's lib_deps, which would
// trip the same LDF gotcha display.cpp/display_epaper.cpp's split avoids).
// So BOARD_CYD gets inline no-op stubs right here instead, keeping the two
// callers (ui_epaper.cpp, main.cpp) free of a board #ifdef of their own.
// -----------------------------------------------------------------------

#if defined(BOARD_CYD)

inline bool speaker_begin() {
    return false;
}
inline void speaker_play(const char *filename) {
    (void)filename;
}
inline void speaker_stop() {}
inline void speaker_process() {}
inline bool speaker_is_playing() {
    return false;
}

#else

// Lazily powers up the codec/amp and brings up I2C + I2S - called once,
// internally, by the first speaker_play(). Returns false if the codec
// didn't respond (I2C error).
bool speaker_begin();

// Starts playing a root-level SD file (stops whatever's already playing
// first). Claims the SD card for the whole duration - see storage.h's
// sd_begin()'s comment - and releases it when playback stops or finishes.
// Must be pumped afterwards by repeated speaker_process() calls (from
// ui_epaper.cpp's ui_process_input(), once per loop() iteration) - this
// call only starts decoding, it doesn't block for the file's duration.
void speaker_play(const char *filename);

// Stops playback immediately (no-op if nothing is playing) and releases
// the SD card.
void speaker_stop();

// Decodes and writes out one more chunk of audio if something is playing;
// no-op otherwise. Cheap to call every loop() iteration regardless of
// playback state.
void speaker_process();

bool speaker_is_playing();

#endif

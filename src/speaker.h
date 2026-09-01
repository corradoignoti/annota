#pragma once

#include <cstddef>

// -----------------------------------------------------------------------
// Owns esp32-s3-epaper154's onboard audio hardware end to end: an ES8311
// I2C codec (es8311.h/.cpp) on a shared I2S bus, feeding an NS4150B amp for
// playback and reading the codec's own mic ADC for recording - see
// speaker.cpp's top comment for the full pin/hardware rundown (from the
// board's own schematic) and why the codec/amp/mic pins live where they
// do. Playback (speaker_*) and recording (mic_*) live in one file/module
// rather than two because they're the same physical peripherals (one I2C
// bus, one I2S controller, one PA_EN power rail) taking turns, never both
// at once - mic_start_recording() always stops playback first, the same
// way storage.cpp keeps one file for both boards where only a couple of
// functions actually differ.
//
// esp32-cyd has no such hardware, and speaker.cpp itself is excluded from
// that env's build (see platformio.ini's board-split comment - it #includes
// ESP8266Audio/I2S headers that aren't in esp32-cyd's lib_deps, which would
// trip the same LDF gotcha display.cpp/display_epaper.cpp's split avoids).
// So BOARD_CYD gets inline no-op stubs right here instead, keeping the
// callers (ui_epaper.cpp) free of a board #ifdef of their own.
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

inline bool mic_start_recording(char *filenameOut, size_t filenameOutLen) {
    (void)filenameOut;
    (void)filenameOutLen;
    return false;
}
inline void mic_stop_recording() {}
inline void mic_process() {}
inline bool mic_is_recording() {
    return false;
}
inline const char *mic_last_error() {
    return "no mic hardware on this board";
}

#else

// Lazily powers up the codec/amp and brings up I2C + I2S - called once,
// internally, by the first speaker_play() or mic_start_recording(). Returns
// false if the codec didn't respond (I2C error).
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

// Stops any playback in progress, then starts recording a new root-level
// PCM WAV from the onboard mic (a fresh "RECnnnn.wav" name from storage.h's
// next_recording_filename()), copying that name into filenameOut. Claims
// the SD card for the whole recording - released by mic_stop_recording().
// Must be pumped afterwards by repeated mic_process() calls (from
// ui_epaper.cpp's ui_process_input(), once per loop() iteration), same
// pattern as speaker_play()/speaker_process() - this call only sets up
// the codec and writes the WAV header, it doesn't block. Returns false
// (nothing started) on codec or SD failure.
bool mic_start_recording(char *filenameOut, size_t filenameOutLen);

// Flushes and closes the in-progress recording (no-op if none), releasing
// the SD card and muting the mic input again.
void mic_stop_recording();

// Reads and encodes one more chunk of mic audio if recording is in
// progress; no-op otherwise. Cheap to call every loop() iteration
// regardless of recording state.
void mic_process();

bool mic_is_recording();

// Reason the last mic_start_recording() call failed (empty string if it
// succeeded or none has been made yet) - ui_epaper.cpp shows this on
// screen on failure, since there's no serial monitor attached in normal
// use to see the Serial.println() speaker.cpp also logs it to.
const char *mic_last_error();

#endif

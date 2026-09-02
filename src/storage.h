#pragma once

#include <FS.h>

#include <cstddef>
#include <cstdint>

constexpr size_t MAX_MP3_FILES = 64;

// Extensions load_mp3_catalog() and the UI's audio/text toggle treat as
// audio - pass to load_file_catalog() (see has_ext() in storage.cpp for
// the '|'-separated format). .mp3 (playback: AudioGeneratorMP3) and .wav
// (playback: AudioGeneratorWAV; also what mic_start_recording() now
// writes - see speaker.cpp's top-of-recording-section comment for why
// PCM WAV instead of an MP3 encoder) - no AAC/m4a decoder anywhere in
// this codebase, and both AI providers' transcribe path only ever reads
// whatever's already on the card, so there's no path that can do
// anything useful with an .m4a file - keeping it listed (as this used
// to) just let it show up as a dead end.
#define AUDIO_EXTS ".mp3|.wav"

struct Mp3Entry {
    char filename[64];
    char created[20]; // "YYYY-MM-DD HH:MM" or "Unknown date"
    uint32_t size;     // bytes, for the on-device Details screen
};

extern Mp3Entry mp3Files[MAX_MP3_FILES];
extern size_t mp3FileCount;

// Scans the SD card's root for audio files (AUDIO_EXTS) into
// mp3Files/mp3FileCount. Returns false if no SD card is present - the
// caller should invite the user to insert one instead of showing a file
// list.
bool load_mp3_catalog();

// Scans the SD card's root for files matching `ext` (e.g. ".mp3", AUDIO_EXTS,
// ".txt"; case-insensitive, dot required, '|'-separated for more than one)
// into mp3Files/mp3FileCount. Generic form of load_mp3_catalog(), used by
// the UI's audio/text list toggle. Returns false if no SD card is present.
bool load_file_catalog(const char *ext);

// Mounts the SD card for a one-off operation outside the boot-time
// catalog scan (web_server.cpp's file manager). Callers bracket this with
// display_suspend_touch()/display_resume_touch() (no-ops on this board -
// see display.h) for symmetry with any future board that does share the
// card's peripheral with something else. Returns false if the card can't
// be opened.
bool sd_begin();

// Unmounts the card mounted by sd_begin().
void sd_end();

// The mounted filesystem object itself (SD_MMC - see storage.cpp's top
// comment). Callers that need direct fs::FS calls (web_server.cpp's file
// manager: list/open/remove) must go through this instead of naming
// `SD_MMC` directly. Only valid between sd_begin() and sd_end().
fs::FS &sd_fs();

struct SdInfo {
    uint64_t cardBytes;    // raw card capacity (SD.cardSize())
    uint64_t totalBytes;   // usable filesystem capacity (SD.totalBytes())
    uint64_t usedBytes;    // filesystem space in use (SD.usedBytes())
    size_t audioFileCount; // root-level audio files (AUDIO_EXTS)
    size_t textFileCount;  // root-level .txt files
};

// Claims the SD card via sd_begin() (same SPI-sharing rules apply - see
// sd_begin()'s comment) just long enough to read capacity/usage figures for
// the settings view, then releases it via sd_end(). Returns false if the
// card can't be opened.
bool get_sd_info(SdInfo &out);

// Claims the SD card via sd_begin() just long enough to read a root-level
// file's contents into `out` (NUL-terminated, truncated to outLen - 1 bytes
// if longer), then releases it via sd_end(). Returns false if the card or
// the file can't be opened. Caller is responsible for the same
// display_suspend_touch()/display_resume_touch() calls as every other
// after-boot SD access (no-ops on this board, kept for symmetry with
// web_server.cpp's SD handlers).
bool read_text_file_preview(const char *filename, char *out, size_t outLen);

// Claims the SD card via sd_begin(), deletes a root-level file, then
// releases it via sd_end(). Returns false if the card can't be opened or
// the file doesn't exist. Same caller responsibility as
// read_text_file_preview() above. Doesn't touch mp3Files/mp3FileCount -
// the caller re-scans (load_file_catalog()) to refresh the on-screen list.
bool delete_file(const char *filename);

// Finds an unused "RECnnnn.wav" name in the SD root (nnnn zero-padded,
// starting at 0001) for a new mic recording (speaker.cpp's
// mic_start_recording()), writing it (NUL-terminated) into `out`. Unlike
// this file's other helpers, doesn't bracket its own sd_begin()/sd_end() -
// the caller already holds the card open for the whole recording that
// follows. Returns false only if every slot up to 9999 is taken (never
// happens in practice).
bool next_recording_filename(char *out, size_t outLen);

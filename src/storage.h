#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t MAX_MP3_FILES = 64;

// Extensions load_mp3_catalog() and the UI's audio/text toggle treat as
// audio - pass to load_file_catalog() (see has_ext() in storage.cpp for
// the '|'-separated format).
#define AUDIO_EXTS ".mp3|.m4a"

struct Mp3Entry {
    char filename[64];
    char created[20]; // "YYYY-MM-DD HH:MM" or "Unknown date"
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

// Claims the shared SPI peripheral and mounts the SD card for a one-off
// operation outside the boot-time catalog scan (web_server.cpp's file
// manager). Touch owns that peripheral after display_init_input(), so call
// display_suspend_touch() first - and display_resume_touch() after sd_end()
// - or touch and SD will stomp on each other's SPI transactions. Returns
// false if the card can't be opened.
bool sd_begin();

// Unmounts the card and releases the SPI peripheral claimed by sd_begin().
void sd_end();

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

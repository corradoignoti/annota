#pragma once

#include <cstddef>

constexpr size_t MAX_MP3_FILES = 64;

struct Mp3Entry {
    char filename[64];
    char created[20]; // "YYYY-MM-DD HH:MM" or "Unknown date"
};

extern Mp3Entry mp3Files[MAX_MP3_FILES];
extern size_t mp3FileCount;

// Tries the SD card first, falls back to internal flash (LittleFS) if no
// card is present. Fills mp3Files/mp3FileCount and returns a short label
// for whichever source ended up in use.
const char *load_mp3_catalog();

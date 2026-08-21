#pragma once

#include <cstddef>

constexpr size_t MAX_MP3_FILES = 64;

struct Mp3Entry {
    char filename[64];
    char created[20]; // "YYYY-MM-DD HH:MM" or "Unknown date"
};

extern Mp3Entry mp3Files[MAX_MP3_FILES];
extern size_t mp3FileCount;

// Scans the SD card's root for .mp3 files into mp3Files/mp3FileCount.
// Returns false if no SD card is present - the caller should invite the
// user to insert one instead of showing a file list.
bool load_mp3_catalog();

#pragma once

#include <FS.h>
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

// The filesystem load_mp3_catalog() ended up using (SD or LittleFS) - all
// entries in mp3Files live at its root. Only valid after load_mp3_catalog().
fs::FS &active_fs();
bool active_source_is_sd();

// Removes mp3Files[index] from the active filesystem and, on success,
// compacts it out of mp3Files/mp3FileCount.
bool delete_mp3_file(size_t index);

// Re-claims the SD SPI bus for a one-off read after load_mp3_catalog()
// released it (see storage.cpp for why that's needed at all). No-op
// (returns true) when the active source isn't SD. Pair with
// release_sd_bus() once done - and disable touch input around the pair,
// since they contend for the same SPI peripheral (see display.h).
bool acquire_sd_bus();
void release_sd_bus();

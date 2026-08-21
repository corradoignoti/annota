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

// Claims the shared SPI peripheral and mounts the SD card for a one-off
// operation outside the boot-time catalog scan (web_server.cpp's file
// manager). Touch owns that peripheral after display_init_input(), so call
// display_suspend_touch() first - and display_resume_touch() after sd_end()
// - or touch and SD will stomp on each other's SPI transactions. Returns
// false if the card can't be opened.
bool sd_begin();

// Unmounts the card and releases the SPI peripheral claimed by sd_begin().
void sd_end();

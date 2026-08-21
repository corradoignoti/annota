#pragma once

// Builds the main screen: with sd_present, a title plus a scrollable list
// of rounded cards, one per entry in mp3Files/mp3FileCount (see
// storage.h); without it, a message inviting the user to insert an SD
// card. Call once, after display_init() and load_mp3_catalog().
void build_main_screen(bool sd_present);

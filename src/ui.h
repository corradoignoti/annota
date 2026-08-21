#pragma once

// Builds the main screen: title (naming the active storage source) plus a
// scrollable list of rounded cards, one per entry in mp3Files/mp3FileCount
// (see storage.h). Call once, after display_init() and load_mp3_catalog().
void build_main_screen(const char *source_label);

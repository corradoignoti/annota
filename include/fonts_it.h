#pragma once

// Custom Montserrat-Medium builds (src/fonts/lv_font_it_*.c) covering ASCII
// plus Latin-1 Supplement (0xC0-0xFF) - LVGL's own built-in
// lv_font_montserrat_* fonts only bake in ASCII, so accented letters (Italian
// e.g. e' a' o', or any other Latin-1 diacritic) render blank with them. Same
// FontAwesome icon glyph set and font metrics as the originals (regenerated
// with lv_font_conv from the same source TTF/woff LVGL itself ships, at
// scripts/built_in_font/ in the lvgl lib_dep - see each file's own Opts:
// header comment), so they're drop-in replacements for every
// &lv_font_montserrat_<size> call site this project actually uses.
#include <lvgl.h>

extern const lv_font_t lv_font_it_10;
extern const lv_font_t lv_font_it_12;
extern const lv_font_t lv_font_it_14;
extern const lv_font_t lv_font_it_20;
extern const lv_font_t lv_font_it_28;
extern const lv_font_t lv_font_it_40;

// Board-conditional semantic roles - ui_epaper.cpp uses these names, never
// a raw lv_font_it_<size> directly, so the same source picks the right
// font per board without any #ifdef inside that file. 154's 200x200 panel
// keeps the original small set; 397's much larger panel uses bigger ones -
// see CLAUDE.md's "LVGL configuration" section for how each set was
// chosen. FONT_HEADER doubles for the status bar's battery/SD icons too,
// not just header_label's WiFi text.
#if defined(BOARD_EPAPER_154)
#define FONT_HINT   lv_font_it_10
#define FONT_HEADER lv_font_it_12
#define FONT_BODY   lv_font_it_14
#define FONT_ICON   lv_font_it_28
#elif defined(BOARD_EPAPER_397)
#define FONT_HINT   lv_font_it_12
#define FONT_HEADER lv_font_it_20
#define FONT_BODY   lv_font_it_20
#define FONT_ICON   lv_font_it_40
#else
#error "No BOARD_EPAPER_* build flag defined - see display.h."
#endif

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
extern const lv_font_t lv_font_it_28;

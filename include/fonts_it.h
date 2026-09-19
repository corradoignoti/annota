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
extern const lv_font_t lv_font_it_15;
extern const lv_font_t lv_font_it_16;
extern const lv_font_t lv_font_it_17;
extern const lv_font_t lv_font_it_18;
extern const lv_font_t lv_font_it_20;
extern const lv_font_t lv_font_it_21;
extern const lv_font_t lv_font_it_28;
extern const lv_font_t lv_font_it_40;

// Board-conditional semantic roles - ui_epaper.cpp uses these names, never
// a raw lv_font_it_<size> directly, so the same source picks the right
// font per board without any #ifdef inside that file. 154's 200x200 panel
// keeps the original small set; 397's much larger panel uses bigger ones -
// see CLAUDE.md's "LVGL configuration" section for how each set was
// chosen. FONT_HEADER doubles for the status bar's battery/SD icons too,
// not just header_label's WiFi text. FONT_LIST is bumped above FONT_BODY,
// used only for kList's file rows (add_row()'s font param) - those are
// the primary content of the screen and were too small at FONT_BODY. It
// reuses FONT_ICON's generated size (the biggest one this project has on
// hand) rather than a size of its own. FONT_LIST_HEADER is kList's own
// "Audio/Text Files (N)" row (render_list_header()) - 15% above
// FONT_HINT, its own size since it needs to track that bump but land at
// a different point size.
#if defined(BOARD_EPAPER_154)
#define FONT_HINT        lv_font_it_15
#define FONT_HEADER      lv_font_it_12
#define FONT_BODY        lv_font_it_14
#define FONT_LIST        lv_font_it_28
#define FONT_LIST_HEADER lv_font_it_17
#define FONT_ICON        lv_font_it_28
#elif defined(BOARD_EPAPER_397)
#define FONT_HINT        lv_font_it_18
#define FONT_HEADER      lv_font_it_20
#define FONT_BODY        lv_font_it_20
#define FONT_LIST        lv_font_it_40
#define FONT_LIST_HEADER lv_font_it_21
#define FONT_ICON        lv_font_it_40
#else
#error "No BOARD_EPAPER_* build flag defined - see display.h."
#endif

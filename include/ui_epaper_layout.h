#pragma once

#include "fonts_it.h"

// kList's file-row/mode-header cards, the bottom hint bar, and menu-panel
// titles were grown past every other screen's plain ROW_H/FONT_BODY (see
// ui_epaper.cpp) during real-hardware readability passes on the 397
// board's much bigger panel. Isolated here, board-conditional, rather
// than inline in ui_epaper.cpp, so that bump can never leak onto the 154
// board even by accident - 154's branch below is wired back to the exact
// same values/fonts every other row on that board already uses, so its
// UI is pixel-identical to before this file existed.
#if defined(BOARD_EPAPER_154)
static const int16_t LIST_ROW_H = 20;    // == ui_epaper.cpp's ROW_H
static const int16_t LIST_HEADER_H = 20; // == ui_epaper.cpp's ROW_H
static const int16_t HINT_BAR_H = 30;    // == ui_epaper.cpp's original HINT_H
#define FONT_LIST        FONT_BODY
#define FONT_LIST_HEADER FONT_BODY
#define FONT_HINT_BAR    FONT_HINT
#define FONT_MENU_TITLE  FONT_BODY
#elif defined(BOARD_EPAPER_397)
static const int16_t LIST_ROW_H = 54;
static const int16_t LIST_HEADER_H = 32;
static const int16_t HINT_BAR_H = 44;
#define FONT_LIST        lv_font_it_40
#define FONT_LIST_HEADER lv_font_it_21
#define FONT_HINT_BAR    lv_font_it_18
#define FONT_MENU_TITLE  lv_font_it_18
#endif

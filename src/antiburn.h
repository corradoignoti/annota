#pragma once

// Idle-timeout backlight blanking: cuts power draw and how long a static
// screen stays lit once nobody's touching it. Pure policy on top of
// display.h's display_idle_ms()/display_set_backlight() - no state of its
// own, so unlike wifi_manager.cpp/transcribe.cpp there's no pending-request
// split to worry about. Call every loop() iteration, after
// lv_timer_handler() (for consistency with the rest of loop(), though
// nothing here actually touches LVGL).
//
// Board-gated: only does anything when platformio.ini's BOARD_CYD build
// flag is defined (see antiburn.cpp); a no-op stub otherwise, so callers
// don't need their own #ifdef.
void antiburn_process();

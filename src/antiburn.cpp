#include "antiburn.h"

// Backlight blanking assumes display.h's TFT_BL/backlight wiring, which is
// specific to the esp32-cyd board (platformio.ini defines BOARD_CYD=1 only
// in that env). A future non-CYD env just won't define it, and this whole
// feature compiles to the no-op stub below instead of touching a backlight
// pin that may not exist on that board.
#ifdef BOARD_CYD

#include "display.h"

// How long the screen sits untouched before the backlight blanks.
static const uint32_t IDLE_TIMEOUT_MS = 30000;

void antiburn_process() {
    // Idempotent either way (display_set_backlight() no-ops onto the same
    // pin state), so no need to track our own "already off/on" flag here -
    // display.cpp's touchpad_read_cb() is what actually notices a wake tap
    // and swallows it.
    display_set_backlight(display_idle_ms() < IDLE_TIMEOUT_MS);
}

#else

void antiburn_process() {}

#endif

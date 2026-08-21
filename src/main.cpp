#include <Arduino.h>
#include <lvgl.h>

#include "audio.h"
#include "display.h"
#include "storage.h"
#include "ui.h"

void setup() {
    Serial.begin(115200);

    display_init_panel();

    // Must run before display_init_input(): SD and touch share the one
    // spare SPI peripheral the display isn't already using full-time (see
    // storage.cpp).
    const char *source_label = load_mp3_catalog();

    display_init_input();
    build_main_screen(source_label);
}

void loop() {
    lv_timer_handler();
    audio_loop();
    delay(5);
}

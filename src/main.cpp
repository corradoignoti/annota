#include <Arduino.h>
#include <lvgl.h>

#include "display.h"
#include "storage.h"
#include "ui.h"

void setup() {
    Serial.begin(115200);

    display_init_panel();

    // Must run before display_init_input(): SD and touch share the one
    // spare SPI peripheral the display isn't already using full-time (see
    // storage.cpp).
    bool sd_present = load_mp3_catalog();

    display_init_input();
    build_main_screen(sd_present);
}

void loop() {
    lv_timer_handler();
    delay(5);
}

#include <Arduino.h>
#include <lvgl.h>

#include "display.h"
#include "storage.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

void setup() {
    Serial.begin(115200);

    display_init_panel();

    // Must run before display_init_input(): SD and touch share the one
    // spare SPI peripheral the display isn't already using full-time (see
    // storage.cpp).
    bool sd_present = load_mp3_catalog();

    display_init_input();
    build_main_screen(sd_present);

    // wifi_connect() paints its own status onto the screen it finds here
    // (ui_set_wifi_status() forces a repaint) before it can block on the
    // captive portal, so build_main_screen() must run first.
    if (wifi_connect()) {
        web_server_start();
    } else {
        Serial.println("Web file manager: not started (no WiFi)");
    }
}

void loop() {
    lv_timer_handler();
    web_server_handle();
    delay(5);
}

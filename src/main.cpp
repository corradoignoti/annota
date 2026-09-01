#include <Arduino.h>
#include <lvgl.h>

#include "display.h"
#include "storage.h"
#include "transcribe.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

void setup() {
    Serial.begin(115200);
    Serial.println("annota: boot");

    display_init_panel();

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
    // Button-driven nav - see ui.h's comment. Placed before the pumps
    // below since a button press here can queue work
    // (transcribe_request()) those pumps pick up in this same loop()
    // iteration.
    ui_process_input();
    // Must come after lv_timer_handler() has returned, never nested
    // inside it - see the comment on wifi_process_pending_reconnect().
    wifi_process_pending_reconnect();
    // Same constraint, same reason - see transcribe_process_pending()'s
    // comment.
    transcribe_process_pending();
    web_server_handle();
    delay(5);
}

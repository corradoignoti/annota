#include <Arduino.h>
#include <driver/gpio.h>
#include <lvgl.h>

#include "battery.h"
#include "display.h"
#include "reboot_combo.h"
#include "sleep.h"
#include "storage.h"
#include "transcribe.h"
#include "ui.h"
#include "usb_drive.h"
#include "web_server.h"
#include "wifi_manager.h"

// Battery power latch - see PWR_HOLD_PIN's comment in reboot_combo.h. Set
// first in setup(), before anything else, so nothing downstream (panel
// init, WiFi, SD) can lose power mid-init.
static void keepBatteryPowerOn() {
    pinMode(PWR_HOLD_PIN, OUTPUT);
    digitalWrite(PWR_HOLD_PIN, HIGH);
    // Release the pad hold reboot_now() may have latched across a software
    // reset - only after driving HIGH, so the level never glitches low.
    gpio_hold_dis((gpio_num_t)PWR_HOLD_PIN);
}

void setup() {
    keepBatteryPowerOn();
    // Right after the power latch, before anything that could hang
    // (panel busy-wait, SD, the first-boot captive portal) - see reboot_combo.h.
    reboot_combo_start();
    sleep_reset_activity(); // starts the idle-sleep clock from boot - see sleep.h

    Serial.begin(115200);
    Serial.println("annota: boot");

    display_init_panel();

    bool sd_present = load_mp3_catalog();

    display_init_input();
    build_main_screen(sd_present);
    ui_set_battery_percent(battery_read_percent());

    // wifi_start_boot_connect() paints its own status onto the screen it
    // finds here (ui_set_wifi_status() forces a repaint), so
    // build_main_screen() must run first. WiFi is off by default - a saved
    // network just stays off until something asks for it on demand
    // (wifi_ensure_connected(), transcribe.cpp) or explicitly
    // (wifi_request_reconnect(), the on-device Online toggle). Only the
    // no-saved-network first-time setup portal can leave WiFi connected by
    // the time this returns - web_server_start() is idempotent, so calling
    // it opportunistically from every place WiFi can become connected
    // (here, wifi_process_pending_reconnect(), transcribe_process_pending())
    // is safe.
    wifi_start_boot_connect();
    if (wifi_is_connected()) {
        web_server_start();
    }
}

void loop() {
    lv_timer_handler();
    // Button-driven nav - see ui.h's comment. Placed before the pumps
    // below since a button press here can queue work
    // (transcribe_request()) those pumps pick up in this same loop()
    // iteration.
    ui_process_input();
    // USB drive mode (usb_drive.h): the USB host owns the SD card, so none
    // of the pumps below (web file manager, transcription) may touch it -
    // WiFi was already taken offline on entry, this is belt-and-braces.
    // usb_drive_process() reboots once the session ends.
    if (usb_drive_active()) {
        usb_drive_process();
        delay(5);
        return;
    }
    // Must come after lv_timer_handler() has returned, never nested
    // inside it - see the comment on wifi_process_pending_reconnect().
    wifi_process_pending_reconnect();
    // Same constraint - see wifi_request_join_network()'s comment.
    wifi_process_pending_join();
    // Same constraint - see wifi_request_file_transfer()'s comment.
    wifi_process_pending_file_transfer();
    // Same constraint - see wifi_request_file_link()'s comment.
    wifi_process_pending_file_link();
    // Cheap no-op almost every call - see its own comment for the every-
    // 15-minutes check it actually does.
    wifi_process_periodic_check();
    // Same constraint, same reason - see transcribe_process_pending()'s
    // comment.
    transcribe_process_pending();
    web_server_handle();
    // Last, after everything above that can reset the idle clock this same
    // pass (button edges via ui_process_input(), served requests via
    // web_server_handle()) has had a chance to. Deep-sleeps and never
    // returns once idle for too long - see sleep.h.
    sleep_process_idle();

    // Battery percentage: polled on a timer, not every iteration - a full
    // e-paper repaint (~1-2s) per loop() pass just to catch a 1% ADC
    // wobble would burn the panel's limited refresh life for nothing.
    // ui_set_battery_percent() itself is a no-op repaint-wise if the
    // rounded percentage hasn't moved since the last call.
    static uint32_t lastBatteryCheckMs = 0;
    uint32_t nowMs = millis();
    if (nowMs - lastBatteryCheckMs >= 60000) {
        lastBatteryCheckMs = nowMs;
        ui_set_battery_percent(battery_read_percent());
    }

    delay(5);
}

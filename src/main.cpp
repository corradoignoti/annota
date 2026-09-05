#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "battery.h"
#include "display.h"
#include "sleep.h"
#include "storage.h"
#include "transcribe.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

// Battery power latch (Waveshare schematic): the physical power switch only
// pulses the regulator on - the MCU must itself hold this pin high or the
// board powers back off the moment the switch is released. Set first in
// setup(), before anything else, so nothing downstream (panel init, WiFi,
// SD) can lose power mid-init.
#define PWR_HOLD_PIN 17

static void keepBatteryPowerOn() {
    pinMode(PWR_HOLD_PIN, OUTPUT);
    digitalWrite(PWR_HOLD_PIN, HIGH);
}

void setup() {
    keepBatteryPowerOn();
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
    // build_main_screen() must run first. A saved network kicks off a
    // background reconnect and returns immediately - loop()'s
    // wifi_process_boot_connect() sees it through and starts the web file
    // manager once it lands. No saved network at all falls back to the
    // blocking setup portal (nothing else useful to do without it), same
    // as before.
    if (!wifi_start_boot_connect() && WiFi.status() == WL_CONNECTED) {
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
    // Must come after lv_timer_handler() has returned, never nested
    // inside it - see the comment on wifi_process_pending_reconnect().
    wifi_process_pending_reconnect();
    // Sees the background boot-time connect (if any) through to
    // completion - see wifi_start_boot_connect()'s comment. Same
    // reentrancy constraint as wifi_process_pending_reconnect() just
    // above, though this one never blocks.
    if (wifi_process_boot_connect() == WifiBootConnectResult::kConnected) {
        web_server_start();
    }
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

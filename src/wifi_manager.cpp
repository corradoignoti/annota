#include "wifi_manager.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <lvgl.h>
#include <time.h>

#include "ui.h"

// -----------------------------------------------------------------------
// WiFi connection manager (tzapu/WiFiManager captive portal).
// -----------------------------------------------------------------------

static const char *PORTAL_SSID = "Annota-Setup";

// Overall budget for one connection attempt (saved-network retry plus
// however long the "join Annota-Setup" portal stays open) before giving
// up and asking the user to hit Retry instead of spinning forever.
static const unsigned long WIFI_TIMEOUT_SECONDS = 300;

// UTC, no daylight offset - storage.cpp only needs a sane wall clock for
// file timestamps, not a local-time display, so no timezone UI exists yet.
static void sync_clock_via_ntp() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 10000)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
        Serial.printf("NTP: clock synced (%s UTC)\n", buf);
    } else {
        Serial.println("NTP: sync failed - keeping system clock as-is");
    }
}

// Forward-declared so it can be wired as the timeout dialog's Retry
// button callback below.
static void retry_button_event_cb(lv_event_t *e);

static bool try_connect() {
    WiFiManager wm;
    // Give up (instead of waiting forever in the portal) after
    // WIFI_TIMEOUT_SECONDS, so a bad/unreachable saved network doesn't
    // block the device offline indefinitely. The user can always ask for
    // another WIFI_TIMEOUT_SECONDS via the timeout dialog's Retry button.
    wm.setConfigPortalTimeout(WIFI_TIMEOUT_SECONDS);

    // autoConnect() blocks loop() the whole time it runs, so the on-screen
    // status only gets to update at these points: right before it tries
    // the saved network, once if it falls back to the portal, and once
    // it's done. ui_set_wifi_status()/ui_show_wifi_setup_dialog() force
    // their own repaint each time since our normal loop()'s
    // lv_timer_handler() isn't running.
    ui_set_wifi_status("Connecting to WiFi...");
    wm.setAPCallback([](WiFiManager *) { ui_show_wifi_setup_dialog(PORTAL_SSID); });

    bool connected = wm.autoConnect(PORTAL_SSID);
    ui_hide_wifi_setup_dialog();
    if (connected) {
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        ui_set_wifi_status(msg);
        Serial.println(msg);
        sync_clock_via_ntp();
    } else {
        ui_set_wifi_status(LV_SYMBOL_WARNING " WiFi not connected - continuing offline");
        Serial.println("WiFi: no connection after 5 minutes - continuing offline");
        ui_show_wifi_timeout_dialog(retry_button_event_cb);
    }
    return connected;
}

static void retry_button_event_cb(lv_event_t *e) {
    (void)e;
    ui_hide_wifi_setup_dialog();
    try_connect();
}

bool wifi_connect() {
    return try_connect();
}

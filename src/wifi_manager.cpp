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
static bool clockSynced = false;

// UTC, no daylight offset - storage.cpp only needs a sane wall clock for
// file timestamps, not a local-time display, so no timezone UI exists yet.
static void sync_clock_via_ntp() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeInfo;
    clockSynced = getLocalTime(&timeInfo, 10000);
    if (clockSynced) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
        Serial.printf("NTP: clock synced (%s UTC)\n", buf);
    } else {
        Serial.println("NTP: sync failed - keeping system clock as-is");
    }
}

bool wifi_clock_synced() {
    return clockSynced;
}

bool wifi_connect() {
    WiFiManager wm;
    // No config portal timeout: if there's no saved network (or it can't
    // be reached), stay in the portal and keep the dialog below up
    // instead of giving up and running offline.
    wm.setConfigPortalTimeout(0);

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
        // Only reachable if the portal is ever given a timeout again.
        ui_set_wifi_status("WiFi not configured - continuing offline");
        Serial.println("WiFi: no connection (portal timed out) - continuing offline");
    }
    return connected;
}

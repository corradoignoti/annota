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

// Budget for reconnecting to the network already saved in NVS before
// giving up and asking the user to hit Retry (in the timeout dialog or
// Settings) instead of retrying forever. Doesn't apply to first-time
// setup - see run_setup_portal().
static const unsigned long WIFI_RECONNECT_TIMEOUT_SECONDS = 30;

// UTC, no daylight offset - storage.cpp only needs a sane wall clock for
// file timestamps, not a local-time display, so no timezone UI exists yet.
// Called after every successful connect, including a reconnect - a
// reconnect's resync can fail on its own (DNS/SNTP isn't necessarily
// ready the instant WiFi.status() reports connected) without the clock
// itself having gone bad, so failure here only skips updating it; it
// never clears an already-true clockSynced back to false. configTime()
// starts the SNTP client asynchronously - it can go on to succeed well
// after this function's own bounded wait gives up, which
// wifi_clock_synced()'s own lazy recheck below is what actually catches.
static void sync_clock_via_ntp() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 10000)) {
        clockSynced = true;
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
        Serial.printf("NTP: clock synced (%s UTC)\n", buf);
    } else {
        Serial.println("NTP: sync still pending - will keep checking in the background");
    }
}

// Registered once (wifi_connect(), boot only) as a WiFi.onEvent()
// handler so the SNTP client gets (re)started on *any* got-IP event, not
// just the connects/reconnects try_connect() explicitly drives - the
// underlying esp_wifi/lwIP station will keep quietly auto-reconnecting
// to the saved network on its own after our own WIFI_RECONNECT_TIMEOUT_SECONDS
// give-up, and try_connect() never finds out when that later succeeds.
// Runs on the WiFi/system event task, not the LVGL task loop() owns, so
// it must stay LVGL-free - configTime() itself has no LVGL dependency.
static void on_wifi_got_ip(WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)event;
    (void)info;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

// Sticky once true (see sync_clock_via_ntp()'s comment); while still
// false, cheaply rechecks (getLocalTime() with a 0ms wait returns
// immediately either way - see esp32-hal-time.c) so a background SNTP
// completion we didn't wait around for gets picked up the next time
// anything asks, e.g. the Settings clock label's once-a-second tick.
bool wifi_clock_synced() {
    if (!clockSynced) {
        struct tm timeInfo;
        clockSynced = getLocalTime(&timeInfo, 0);
    }
    return clockSynced;
}

// Set by wifi_request_reconnect(), consumed once by
// wifi_process_pending_reconnect() from loop() - see the reentrancy note
// there for why a retry can't just run try_connect() directly from the
// UI callback that asked for it.
static volatile bool reconnectRequested = false;

// Forward-declared so it can be wired as the timeout dialog's Close
// button callback below.
static void close_button_event_cb(lv_event_t *e);

// First-time setup: no network saved yet, so there's no "reconnect" to
// attempt and no point giving up - the device has no other way online.
// Opens the "Annota-Setup" captive portal AP and blocks until the user
// joins it and picks a network from a phone/laptop; credentials entered
// there are saved to NVS for every boot after this one.
static bool run_setup_portal() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(0);

    // autoConnect() blocks loop() the whole time it runs, so the on-screen
    // dialog only gets to paint once, from this callback.
    // ui_show_wifi_setup_dialog() forces its own repaint since our normal
    // loop()'s lv_timer_handler() isn't running.
    wm.setAPCallback([](WiFiManager *) { ui_show_wifi_setup_dialog(PORTAL_SSID); });

    bool connected = wm.autoConnect(PORTAL_SSID);
    ui_hide_wifi_setup_dialog();
    return connected;
}

// Network already saved in NVS: try reconnecting to it directly (no
// portal, no AP - the user isn't being asked to reconfigure anything)
// for up to WIFI_RECONNECT_TIMEOUT_SECONDS, then give up so the device
// keeps working offline instead of hanging here indefinitely. Counts
// down on the status label each second so the wait isn't a silent
// freeze - ui_set_wifi_status() forces its own repaint since our normal
// loop()'s lv_timer_handler() isn't running here.
static bool reconnect_saved_network() {
    WiFi.mode(WIFI_STA);
    WiFi.begin();  // no args = reconnect with the credentials already in NVS

    unsigned long deadline = millis() + WIFI_RECONNECT_TIMEOUT_SECONDS * 1000UL;
    int lastShownSecs = -1;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        int remainingSecs = (deadline - millis() + 999) / 1000;  // round up
        if (remainingSecs != lastShownSecs) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Connecting to WiFi... %ds", remainingSecs);
            ui_set_wifi_status(msg);
            lastShownSecs = remainingSecs;
        }
        delay(100);
    }
    return WiFi.status() == WL_CONNECTED;
}

static bool try_connect() {
    WiFiManager wm;
    bool hasSavedNetwork = wm.getWiFiIsSaved();

    ui_set_wifi_status("Connecting to WiFi...");
    bool connected = hasSavedNetwork ? reconnect_saved_network() : run_setup_portal();

    if (connected) {
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        ui_set_wifi_status(msg);
        Serial.println(msg);
        sync_clock_via_ntp();
    } else {
        ui_set_wifi_status(LV_SYMBOL_WARNING " working offline");
        Serial.println("WiFi: no connection after 30 seconds - continuing offline");
        ui_show_wifi_timeout_dialog(close_button_event_cb);
    }
    ui_refresh_wifi_retry_button();
    return connected;
}

// The timeout dialog only dismisses itself on Close - reconnecting is a
// separate, explicit action via the Settings "Reconnect WiFi" button
// (ui.cpp, calls wifi_request_reconnect() directly), not something
// giving up on the dialog re-triggers. ui_hide_wifi_setup_dialog() is
// safe to call straight from here even though it's this button's own
// click event still being dispatched - it defers the actual delete (see
// its _async comment).
static void close_button_event_cb(lv_event_t *e) {
    (void)e;
    ui_hide_wifi_setup_dialog();
}

bool wifi_connect() {
    WiFi.onEvent(on_wifi_got_ip, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    return try_connect();
}

void wifi_request_reconnect() {
    reconnectRequested = true;
}

void wifi_process_pending_reconnect() {
    if (!reconnectRequested) return;
    reconnectRequested = false;
    ui_hide_wifi_setup_dialog();
    try_connect();
}

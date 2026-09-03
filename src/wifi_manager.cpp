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
// the web UI's Settings page) instead of retrying forever. Doesn't apply
// to first-time setup - see run_setup_portal().
static const unsigned long WIFI_RECONNECT_TIMEOUT_SECONDS = 10;

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

// Registered once (wifi_start_boot_connect(), boot only) as a WiFi.onEvent()
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
// anything asks, e.g. the web UI's clock status field, polled from
// /api/settings.
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

// State for the background boot-time connect - see wifi_start_boot_connect()/
// wifi_process_boot_connect() below. Unlike reconnectRequested's flag+block
// pair, this one is polled to completion across many loop() iterations
// instead of run to completion the first time loop() picks it up.
static bool bootConnectPending = false;
static unsigned long bootConnectDeadline = 0;
static int bootConnectLastShownSecs = -1;

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
// portal, no AP - the user isn't being asked to reconfigure anything yet)
// for up to WIFI_RECONNECT_TIMEOUT_SECONDS, then give up so this doesn't
// hang here indefinitely - try_connect() falls back to the setup portal
// once this returns false. Counts down on the status label each second so
// the wait isn't a silent freeze - ui_set_wifi_status() forces its own
// repaint since our normal loop()'s lv_timer_handler() isn't running here.
static bool reconnect_saved_network() {
    WiFi.mode(WIFI_STA);
    WiFi.begin();  // no args = reconnect with the credentials already in NVS

    unsigned long deadline = millis() + WIFI_RECONNECT_TIMEOUT_SECONDS * 1000UL;
    int lastShownSecs = -1;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        int remainingSecs = (deadline - millis() + 999) / 1000;  // round up
        if (remainingSecs != lastShownSecs) {
            char msg[48];
            snprintf(msg, sizeof(msg), "WiFi chk... %ds", remainingSecs);
            ui_set_wifi_status(msg);
            lastShownSecs = remainingSecs;
        }
        delay(100);
    }
    return WiFi.status() == WL_CONNECTED;
}

// No portal-fallback param anymore: the setup portal only ever appears
// when nothing is saved yet. A saved network that fails to answer never
// gets wiped and never drops the user into AP setup out from under
// them - it just leaves the device offline, said only via the header
// status line (ui_set_wifi_status() below) rather than a modal - nothing
// else on screen changes, so whatever the user was doing (browsing the
// file list, say) isn't interrupted, same whether this runs at boot or
// from the web UI's "Reconnect WiFi" button. Getting back to setup is
// only ever the explicit, irreversible "Delete WiFi Setup" action
// (wifi_forget_and_reboot()).
static bool try_connect() {
    // getWiFiIsSaved() below reads NVS through esp_wifi_get_config(), which
    // needs the wifi driver already initialized - on a fresh boot (driver
    // never touched yet) it fails and leaves its output struct as
    // uninitialized stack garbage, which getWiFiIsSaved() then reads as a
    // bogus non-empty "saved" SSID. That false positive skips the setup
    // portal entirely on an unconfigured device (goes to
    // reconnect_saved_network() instead, which just times out against
    // nothing real, then stops - no portal, no way online). WiFi.mode()
    // here guarantees the driver is initialized (idempotent - a no-op if
    // reconnect_saved_network() below is about to call it again anyway) so
    // the saved-network check reads real NVS state.
    WiFi.mode(WIFI_STA);

    WiFiManager wm;
    // getWiFiIsSaved() reads ESP-IDF's own station config out of NVS, not
    // anything wifi_manager.cpp itself wrote - it persists across reflashes
    // and even across different sketches ever run on this board. Doesn't
    // matter here whether it's stale: saved is saved, and only an explicit
    // "Delete WiFi Setup" clears it - a failed connect attempt never does.
    bool hasSavedNetwork = wm.getWiFiIsSaved();

    ui_set_wifi_status("Connecting to WiFi...");
    bool connected = hasSavedNetwork ? reconnect_saved_network() : false;

    if (!connected && !hasSavedNetwork) {
        // Nothing configured at all - the only way online is the setup
        // portal, same as a factory-fresh board.
        connected = run_setup_portal();
    }

    if (connected) {
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        ui_set_wifi_status(msg);
        Serial.println(msg);
        sync_clock_via_ntp();
    } else {
        ui_set_wifi_status(LV_SYMBOL_WARNING " working offline");
        Serial.println(hasSavedNetwork
                            ? "WiFi: saved network unreachable - continuing offline (Reconnect WiFi to retry)"
                            : "WiFi: setup portal exited without a connection - continuing offline");
    }
    ui_refresh_wifi_retry_button();
    return connected;
}

bool wifi_start_boot_connect() {
    WiFi.onEvent(on_wifi_got_ip, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    // See try_connect()'s comment: WiFi.mode() must run before
    // getWiFiIsSaved() so it reads real NVS state instead of uninitialized
    // driver stack garbage.
    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    if (!wm.getWiFiIsSaved()) {
        // Nothing to try in the background - the setup portal is the only
        // way online and needs the user's phone/laptop anyway, so this
        // path stays exactly as blocking as it always was.
        try_connect();
        return false;
    }

    ui_set_wifi_status("Connecting to WiFi...");
    WiFi.begin();  // no args = reconnect with the credentials already in NVS, doesn't block
    bootConnectPending = true;
    bootConnectDeadline = millis() + WIFI_RECONNECT_TIMEOUT_SECONDS * 1000UL;
    bootConnectLastShownSecs = -1;
    return true;
}

WifiBootConnectResult wifi_process_boot_connect() {
    if (!bootConnectPending) return WifiBootConnectResult::kIdle;

    if (WiFi.status() == WL_CONNECTED) {
        bootConnectPending = false;
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        ui_set_wifi_status(msg);
        Serial.println(msg);
        sync_clock_via_ntp();
        ui_refresh_wifi_retry_button();
        return WifiBootConnectResult::kConnected;
    }

    if (millis() < bootConnectDeadline) {
        int remainingSecs = (bootConnectDeadline - millis() + 999) / 1000;  // round up
        if (remainingSecs != bootConnectLastShownSecs) {
            char msg[48];
            snprintf(msg, sizeof(msg), "WiFi chk... %ds", remainingSecs);
            ui_set_wifi_status(msg);
            bootConnectLastShownSecs = remainingSecs;
        }
        return WifiBootConnectResult::kPending;
    }

    bootConnectPending = false;
    ui_set_wifi_status(LV_SYMBOL_WARNING " working offline");
    Serial.println("WiFi: saved network unreachable - continuing offline (Reconnect WiFi to retry)");
    ui_refresh_wifi_retry_button();
    return WifiBootConnectResult::kFailed;
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

// Used by transcribe.cpp right before a transcription attempt: if already
// online, returns true with no side effects; otherwise makes one blocking
// reconnect attempt to the saved network (same 30s budget and status
// feedback as try_connect()'s own reconnect path) so a device that's
// offline only because it booted with the router unreachable doesn't
// force the user to go find "Reconnect WiFi" first. Never opens the
// setup portal - transcribing doesn't imply the user wants to
// reconfigure the network, and there may be nothing saved to fall back
// from anyway. Safe to call from loop() top level (transcribe_process_pending()'s
// call site) for the same reentrancy reason wifi_process_pending_reconnect() is.
bool wifi_ensure_connected() {
    if (WiFi.status() == WL_CONNECTED) return true;

    // See try_connect()'s comment: getWiFiIsSaved() needs the wifi driver
    // initialized to read real NVS state instead of stack garbage.
    WiFi.mode(WIFI_STA);

    WiFiManager wm;
    if (!wm.getWiFiIsSaved()) {
        // Nothing to even try - leave the header saying so rather than
        // whatever it happened to say before (stale "Connecting..." from
        // an earlier attempt, say), same wording as every other failure
        // path below.
        ui_set_wifi_status(LV_SYMBOL_WARNING " working offline");
        return false;
    }

    ui_set_wifi_status("Connecting to WiFi...");
    bool connected = reconnect_saved_network();
    if (connected) {
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        ui_set_wifi_status(msg);
        Serial.println(msg);
        sync_clock_via_ntp();
    } else {
        ui_set_wifi_status(LV_SYMBOL_WARNING " working offline");
        Serial.println("WiFi: reconnect for transcription failed - still offline");
    }
    ui_refresh_wifi_retry_button();
    return connected;
}

// Unlike wifi_request_reconnect(), this doesn't need to defer through
// loop() - it never returns, so there's no repaint afterwards that could
// silently no-op from running nested inside lv_timer_handler().
void wifi_forget_and_reboot() {
    WiFiManager wm;
    wm.resetSettings();
    Serial.println("WiFi: saved network erased by user - rebooting into setup portal");
    delay(200);
    ESP.restart();
}

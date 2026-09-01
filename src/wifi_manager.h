#pragma once

// Connects to WiFi. Two cases:
//  - No network saved in NVS yet: opens a captive portal AP
//    ("Annota-Setup", no password) and shows an on-screen dialog inviting
//    the user to join it and pick a network from a phone/laptop;
//    credentials entered there are saved for every boot after this one.
//    Blocks indefinitely - there's no other way online yet, so this only
//    returns once it succeeds.
//  - A network is already saved: reconnects to it directly, no portal, for
//    up to 30 seconds (WIFI_RECONNECT_TIMEOUT_SECONDS in wifi_manager.cpp).
//    If that times out, the saved credentials are wiped (WiFiManager's
//    "saved" NVS entry can be stale - left over from a different sketch
//    ever flashed to this board, not necessarily something the user
//    configured) and this falls back to the same setup portal as the
//    no-network-saved case above, so there's always a way back to
//    configuring an AP instead of getting stuck offline with no recourse.
//    Only if that portal itself is exited without connecting does this
//    give up, show a warning dialog with a Close button, and let the
//    device run offline - reconnecting again is then a separate, explicit
//    action via the web UI's "Reconnect WiFi" button (see
//    wifi_request_reconnect()).
// Also registers a WiFi.onEvent() handler (once) that (re)starts the
// SNTP client on every got-IP event, including ones try_connect() never
// sees - e.g. the underlying esp_wifi/lwIP station quietly auto-
// reconnecting to the saved network on its own well after this module
// gave up waiting (see WIFI_RECONNECT_TIMEOUT_SECONDS). Without that,
// booting offline and having WiFi come back on its own later would never
// kick off an NTP sync at all, and wifi_clock_synced() would stay false
// forever.
// Call once, after build_main_screen() (the dialog is drawn onto
// whatever's already on screen) and Serial.begin().
bool wifi_connect();

// True once the system clock has been set from an NTP server, after any
// connect, reconnect, or the background auto-reconnect wifi_connect()
// listens for (see its comment); false before the first success (no
// WiFi yet, or the NTP request hasn't completed) - the system clock may
// still be running from whatever it was at power-on in that case.
// Sticky once true, and lazily self-correcting while still false: each
// call that finds it still false cheaply rechecks whether the SNTP
// client (started elsewhere, asynchronously) has actually finished by
// now, so a sync that completes in the background after whatever
// triggered it gave up waiting still gets picked up next time anything
// asks - e.g. the web UI's clock status field, polled from /api/settings.
bool wifi_clock_synced();

// Asks for the same connect attempt wifi_connect() makes to run again -
// wired to the web UI's "Reconnect WiFi" button (web_server.cpp's
// handle_settings_reconnect()), so this only flags the request; it does
// not block. Call wifi_process_pending_reconnect() to actually run it.
// Since this is only reachable once the device has booted past
// wifi_connect(), a network is always already saved at this point, so the
// run always takes the reconnect path, never the setup portal - unlike
// the boot-time connect, a manual click here never wipes saved
// credentials on failure; it just fails the same way it always did (see
// wifi_connect()'s comment) so the user isn't dropped into AP setup mode
// by a button that looks like a simple retry.
void wifi_request_reconnect();

// Runs the reconnect attempt requested by wifi_request_reconnect(), if
// any - a no-op otherwise. Call once per loop() iteration, after
// lv_timer_handler() has returned (never from inside an LVGL event or
// timer callback): this blocks until it connects or times out, the same
// way wifi_connect() does, and needs lv_timer_handler() to not already
// be running so its own status/dialog repaints actually take effect.
void wifi_process_pending_reconnect();

// Erases the WiFi network saved in NVS (WiFiManager's resetSettings()) and
// immediately reboots (ESP.restart()) so the next boot has nothing saved
// and falls straight into wifi_connect()'s first-time setup portal - same
// recovery path as a factory-fresh board. Never returns. Irreversible -
// callers (web_server.cpp's "Delete WiFi Setup" button) must confirm with
// the user first; this function itself does no confirmation.
void wifi_forget_and_reboot();

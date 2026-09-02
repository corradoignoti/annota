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
//    If that times out, the device just stays offline - the saved
//    credentials are never wiped and the setup portal never reappears on
//    its own. A warning dialog with a Close button says so and lets the
//    device run offline; the user can still record, delete, and preview
//    files while offline (see main.cpp/ui_epaper.cpp - none of that needs
//    a network). Reconnecting again is a separate, explicit action, either
//    the web UI's "Reconnect WiFi" button (see wifi_request_reconnect())
//    or the automatic retry transcribe.cpp makes before a transcription
//    (see wifi_ensure_connected()). The only way back to the setup portal
//    is the explicit, irreversible "Delete WiFi Setup" button
//    (wifi_forget_and_reboot()).
//  This same logic runs unchanged on every boot, including a deep-sleep
//  wakeup - waking is a full MCU reset (see sleep.h), so there's no
//  separate wake-time code path: a configured network gets one reconnect
//  attempt, succeeds or leaves the device offline, same as any other boot.
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

// If already connected, returns true immediately. Otherwise, if a network
// is saved, makes one blocking reconnect attempt to it (same budget and
// status feedback as the reconnect path above) and returns whether that
// succeeded; never opens the setup portal. Called by
// transcribe_process_pending() before a transcription attempt so a device
// that's offline only because it booted with the router unreachable
// doesn't force the user to hunt down "Reconnect WiFi" first. Same
// reentrancy constraint as wifi_process_pending_reconnect(): call from
// loop() top level, never nested inside lv_timer_handler().
bool wifi_ensure_connected();

// Erases the WiFi network saved in NVS (WiFiManager's resetSettings()) and
// immediately reboots (ESP.restart()) so the next boot has nothing saved
// and falls straight into wifi_connect()'s first-time setup portal - same
// recovery path as a factory-fresh board. Never returns. Irreversible -
// callers (web_server.cpp's "Delete WiFi Setup" button) must confirm with
// the user first; this function itself does no confirmation.
void wifi_forget_and_reboot();

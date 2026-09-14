#pragma once

// Starts connecting to WiFi at boot (including a deep-sleep wakeup, which
// is a full MCU reset - see sleep.h - so there's no separate wake-time
// path). Two cases:
//  - No network saved in NVS yet: there's nothing to try in the
//    background - the only way online is the captive portal AP
//    ("Annota-Setup", no password), and that needs the user's
//    phone/laptop anyway. Opens it and shows an on-screen dialog, blocking
//    indefinitely until the user configures a network from there;
//    credentials entered are saved for every boot after this one. Returns
//    false in this case: by the time it returns, the portal has already
//    resolved (connected or given up), there's nothing left to poll.
//  - A network is already saved: kicks off a reconnect (WiFi.begin() with
//    the credentials already in NVS) and returns immediately, true,
//    without waiting for it to complete - so setup() can build the UI and
//    start the SD scan without the device sitting frozen on a blank
//    screen for however long the router takes to answer. Call
//    wifi_process_boot_connect() every loop() iteration afterwards to see
//    it through: up to WIFI_RECONNECT_TIMEOUT_SECONDS (wifi_manager.cpp)
//    before giving up. If that times out, the device just stays offline -
//    the saved credentials are never wiped and the setup portal never
//    reappears on its own. Nothing modal interrupts the screen for this -
//    only the header status line says so (ui_set_wifi_status()) - and the
//    device keeps running offline; the user can still record, delete, and
//    preview files while offline (see main.cpp/ui_epaper.cpp - none of
//    that needs a network). Reconnecting again afterwards is a
//    separate, explicit action, either the web UI's "Reconnect WiFi"
//    button (see wifi_request_reconnect()) or the automatic retry
//    transcribe.cpp makes before a transcription (see
//    wifi_ensure_connected()). The only way back to the setup portal is
//    the explicit, irreversible "Delete WiFi Setup" button
//    (wifi_forget_and_reboot()).
// Also registers a WiFi.onEvent() handler (once) that (re)starts the
// SNTP client on every got-IP event, including ones this module's own
// connect attempts never see - e.g. the underlying esp_wifi/lwIP station
// quietly auto-reconnecting to the saved network on its own well after
// WIFI_RECONNECT_TIMEOUT_SECONDS gave up. Without that, booting offline
// and having WiFi come back on its own later would never kick off an NTP
// sync at all, and wifi_clock_synced() would stay false forever.
// Call once, after build_main_screen() (status/dialogs are drawn onto
// whatever's already on screen) and Serial.begin().
bool wifi_start_boot_connect();

// What wifi_process_boot_connect() found on a given call.
enum class WifiBootConnectResult {
    kIdle,       // nothing pending - either no boot connect was started,
                 // or it already finished on an earlier call
    kPending,    // still waiting, within budget - call again next loop()
    kConnected,  // just succeeded this call (fires once) - main.cpp starts
                 // the web file manager off this
    kFailed,     // just gave up this call (fires once) - device continues
                 // offline, said only via the header status line, nothing
                 // modal
};

// Polls the background connect wifi_start_boot_connect() started; a no-op
// (kIdle) if nothing is pending, e.g. the no-saved-network/portal case,
// which already resolved synchronously inside wifi_start_boot_connect()
// itself. On kConnected/kFailed, finalizes the same way the old blocking
// connect did: status label, NTP sync on success, offline status on
// failure, ui_refresh_wifi_retry_button() either way - deliberately no
// modal dialog on failure, so whatever the user's looking at (the file
// list, say) isn't interrupted just because the router didn't answer.
// Never blocks. Call once per loop() iteration, after lv_timer_handler()
// has returned, never nested inside it - ui_set_wifi_status() forces its
// own repaint, which would silently no-op if lv_timer_handler() were
// already running higher up the call stack.
WifiBootConnectResult wifi_process_boot_connect();

// True once the system clock has been set from an NTP server, after any
// connect, reconnect, or the background auto-reconnect wifi_start_boot_connect()
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

// Asks for the same connect attempt wifi_start_boot_connect() makes to run
// again - wired to the web UI's "Reconnect WiFi" button (web_server.cpp's
// handle_settings_reconnect()), so this only flags the request; it does
// not block. Call wifi_process_pending_reconnect() to actually run it.
// Since this is only reachable once the device has booted past the boot
// connect, a network is always already saved at this point, so the run
// always takes the reconnect path, never the setup portal - unlike the
// boot-time connect, a manual click here never wipes saved credentials on
// failure; it just fails the same way it always did (see
// wifi_start_boot_connect()'s comment) so the user isn't dropped into AP
// setup mode by a button that looks like a simple retry.
void wifi_request_reconnect();

// Runs the reconnect attempt requested by wifi_request_reconnect(), if
// any - a no-op otherwise. Call once per loop() iteration, after
// lv_timer_handler() has returned (never from inside an LVGL event or
// timer callback): this blocks until it connects or times out - unlike
// wifi_start_boot_connect()'s background poll, a manual reconnect click is
// already an explicit wait the user asked for - and needs
// lv_timer_handler() to not already be running so its own status repaints
// actually take effect.
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
// and falls straight into wifi_start_boot_connect()'s first-time setup
// portal - same recovery path as a factory-fresh board. Never returns. Irreversible -
// callers (web_server.cpp's "Delete WiFi Setup" button) must confirm with
// the user first; this function itself does no confirmation.
void wifi_forget_and_reboot();

// Plain WiFi.status() == WL_CONNECTED check, wrapped here so callers
// (ui_epaper.cpp's on-device menu) don't need their own <WiFi.h> include
// just to ask.
bool wifi_is_connected();

// Explicit, reversible opposite of a reconnect: disconnects and powers off
// the WiFi radio (WiFi.disconnect(true)/esp_wifi_deinit()) so the device
// actually saves power while offline, not just idles an associated radio -
// the network saved in NVS is left untouched, unlike
// wifi_forget_and_reboot(), so wifi_request_reconnect() (the matching
// on-device "Online" action, ui_epaper.cpp) can bring it back without
// redoing setup. Powering all the way off means that reconnect has to
// reinitialize the WiFi driver from scratch first - see try_connect()'s
// settle-delay comment for the one thing that reinit needs that a normal
// live-driver reconnect doesn't. Also cancels a still-pending background
// boot connect (wifi_start_boot_connect()), if any, so it doesn't
// overwrite this call's own status line moments later. Updates the header
// status line the same way every other disconnect path does. Safe to call
// from loop() top level (ui_process_input(), same as
// wifi_request_reconnect()) - it never blocks.
void wifi_go_offline();

// Call once per loop() iteration (top level, same as every other
// wifi_process_*() here) - cheap no-op almost every call, and every 15
// minutes checks whether the saved AP is actually still reachable. If the
// radio's on but not connected, calls wifi_go_offline() - same as the
// on-device "Offline" menu item, silently (no dialog): gives up on an AP
// that's gone rather than leaving the radio burning power retrying
// forever. No-op if already offline (radio off) or still connected.
// Never blocks.
void wifi_process_periodic_check();

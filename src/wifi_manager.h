#pragma once

#include <stddef.h>

// Prepares WiFi at boot (including a deep-sleep wakeup, which is a full
// MCU reset - see sleep.h - so there's no separate wake-time path). WiFi
// is off by default - it only turns on on-demand (see
// wifi_ensure_connected(), used by transcribe.cpp before a transcription)
// or when the user explicitly asks (wifi_request_reconnect(), the
// on-device Online toggle / web UI's "Reconnect WiFi"). Two cases:
//  - No network saved in NVS yet: there's nothing to try in the
//    background - the only way online is the captive portal AP
//    ("Annota-Setup", no password), and that needs the user's
//    phone/laptop anyway. Opens it and shows an on-screen dialog, blocking
//    indefinitely until the user configures a network from there;
//    credentials entered are saved for every boot after this one. By the
//    time this returns, the portal has already resolved (connected or
//    given up).
//  - A network is already saved: briefly initializes the WiFi driver just
//    to confirm that (see try_connect()'s comment on why
//    getWiFiIsSaved() needs it), then immediately powers the radio back
//    off via wifi_go_offline() - no connect attempt is made here. The
//    header status line stays blank; nothing modal interrupts the screen.
//    The user can still record, delete, and preview files with WiFi off
//    (see main.cpp/ui_epaper.cpp - none of that needs a network). Going
//    online is always a separate, explicit-or-on-demand action: the web
//    UI's "Reconnect WiFi" button / on-device Online toggle
//    (wifi_request_reconnect()), or transcribe.cpp's automatic on-demand
//    connect before a transcription (wifi_ensure_connected()). The only
//    way back to the setup portal is the explicit, irreversible "Delete
//    WiFi Setup" button (wifi_forget_and_reboot()).
// Also registers a WiFi.onEvent() handler (once) that (re)starts the
// SNTP client on every got-IP event, including ones this module's own
// connect attempts never see - e.g. the underlying esp_wifi/lwIP station
// quietly auto-reconnecting to the saved network on its own well after
// WIFI_RECONNECT_TIMEOUT_SECONDS gave up. Without that, a later
// WiFi-on window that outlives an explicit wait would never kick off an
// NTP sync at all, and wifi_clock_synced() would stay false forever.
// Call once, after build_main_screen() (status/dialogs are drawn onto
// whatever's already on screen) and Serial.begin().
void wifi_start_boot_connect();

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

// Same request/process split as wifi_request_reconnect()/
// wifi_process_pending_reconnect() above, but for opening the
// "Annota-Setup" captive-portal AP on demand (not just at first boot) so
// a different network can be joined without wiping the one already
// saved - WiFiManager's portal overwrites saved credentials on submit,
// no explicit erase needed first. wifi_process_pending_setup_portal()
// does the actual blocking wm.autoConnect() call and paints its own
// dialog via ui_show_wifi_setup_dialog()/ui_hide_wifi_setup_dialog() -
// same reentrancy constraint as wifi_process_pending_reconnect(): call
// from loop() top level, never nested inside lv_timer_handler().
void wifi_request_setup_portal();
void wifi_process_pending_setup_portal();

// Puts the radio into standalone soft-AP mode ("Annota-AP", no password)
// and starts the web file manager on it, so a phone/laptop can reach it
// directly with no router/internet involved at all - distinct from the
// captive portal above, which is a join-a-network flow that happens to
// use its own AP along the way. Non-blocking (WiFi.softAP() returns
// immediately), so unlike the two request/process pairs above this is
// safe to call directly from an LVGL callback. Writes the AP's IP
// (dotted-quad text, e.g. "192.168.4.1") into ip_out (ip_out_size bytes)
// so the caller can display it - same out-buffer idiom as
// mic_start_recording()'s filename param. wifi_go_offline()'s
// WiFi.disconnect(true) already tears this back down (mode-agnostic
// esp_wifi_stop() under wifioff=true), so the existing on-device
// Offline/Online toggle doubles as the way out of this mode too.
void wifi_start_standalone_ap(char *ip_out, size_t ip_out_size);

// If already connected, returns true immediately with no side effects -
// callers can use this to tell whether they're the ones turning WiFi on
// (and therefore responsible for turning it back off after, see
// wifi_go_offline()) or whether it was already on for some other reason
// (e.g. a manual Online session) that they shouldn't disturb. Otherwise,
// if a network is saved, makes one blocking reconnect attempt to it (same
// budget and status feedback as the reconnect path above) and returns
// whether that succeeded; never opens the setup portal. On failure, powers
// the radio back off (wifi_go_offline()) rather than leaving it
// initialized-but-unassociated, since a failure here only ever happens on
// a radio this call itself just touched. Called by
// transcribe_process_pending() before a transcription attempt - WiFi is
// off by default (see wifi_start_boot_connect()), so this is what actually
// turns it on for that. Same reentrancy constraint as
// wifi_process_pending_reconnect(): call from loop() top level, never
// nested inside lv_timer_handler().
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
// actually saves power while off, not just idles an associated radio - the
// network saved in NVS is left untouched, unlike wifi_forget_and_reboot(),
// so wifi_request_reconnect() (the matching on-device "Online" action,
// ui_epaper.cpp) can bring it back without redoing setup. Powering all the
// way off means that reconnect has to reinitialize the WiFi driver from
// scratch first - see try_connect()'s settle-delay comment for the one
// thing that reinit needs that a normal live-driver reconnect doesn't.
// Three callers: the on-device Offline menu item, wifi_start_boot_connect()'s
// stay-off-at-boot path, and transcribe_process_pending()'s post-transcription
// auto-off - so the header status line this clears goes blank rather than
// any "disconnected by you" framing; it's routinely just WiFi returning to
// its normal off-by-default state, not a failure. Safe to call from loop()
// top level (ui_process_input(), same as wifi_request_reconnect()) - it
// never blocks.
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

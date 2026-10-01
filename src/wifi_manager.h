#pragma once

#include <stddef.h>

// Limits for the saved-network list below - small on purpose (NVS/heap
// footprint on an ESP32-S3), generous enough for home + office + a phone
// hotspot with a little headroom.
static const size_t WIFI_SSID_MAX_LEN = 32;
static const size_t WIFI_PASSWORD_MAX_LEN = 64;
static const int WIFI_MAX_SAVED_NETWORKS = 5;

// Fast-path budget for the preferred/default network (see
// wifi_request_join_network() below) - short, since it's one specific
// network rather than a full WiFiMulti sweep; a full sweep still follows if
// this one fails.
static const unsigned long WIFI_PREFERRED_TIMEOUT_SECONDS = 5;

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

// Request/process split for the on-device Home screen's "File transfer"
// row (ui_epaper.cpp's kHome case): makes one on-demand connect attempt
// (wifi_ensure_connected() - never opens the setup portal, unlike
// try_connect()) and, on success, starts the web file manager and shows
// its IP (ui.h's ui_show_wifi_joined_screen()); on failure (nothing
// saved, or the attempt failed) sends the user to the WiFi management
// screen instead (ui.h's ui_show_wifi_manage_screen()) so they can join
// or create an AP. Same request/process split and reentrancy constraint
// as wifi_request_reconnect()/wifi_process_pending_reconnect() above -
// call wifi_process_pending_file_transfer() from loop() top level, after
// lv_timer_handler() has returned.
void wifi_request_file_transfer();
void wifi_process_pending_file_transfer();

// Erases the WiFi network saved in NVS (WiFiManager's resetSettings()) and
// the multi-AP list below, then immediately reboots (ESP.restart()) so the
// next boot has nothing saved and falls straight into
// wifi_start_boot_connect()'s first-time setup portal - same recovery path
// as a factory-fresh board. Never returns. Irreversible - callers
// (web_server.cpp's "Delete WiFi Setup" button) must confirm with the user
// first; this function itself does no confirmation.
void wifi_forget_and_reboot();

// Multi-AP list: web_server.cpp's Settings page lets the user save several
// networks (home/office/hotspot) instead of just the one the captive
// portal writes into ESP-IDF's single NVS slot. Persisted separately
// (Preferences, namespace "annota" - see wifi_manager.cpp), consulted by
// try_connect()/wifi_ensure_connected() via WiFiMulti so the device joins
// whichever saved network is actually in range. Passwords are write-only
// from here on out - same "never echo a saved secret back" rule as
// transcribe.h's AI API key - so there's no getter for one, only
// wifi_update_network_password() to replace it.

// Number of networks currently saved (0..WIFI_MAX_SAVED_NETWORKS).
int wifi_saved_network_count();

// Writes the SSID at index (0-based, < wifi_saved_network_count()) into out
// (outSize bytes, NUL-terminated); returns false if index is out of range.
// Never returns the password - see the note above.
bool wifi_get_saved_network_ssid(int index, char *out, size_t outSize);

// Adds a new saved network. password may be empty (open network). Fails
// (returns false, nothing changed) if ssid/password exceed
// WIFI_SSID_MAX_LEN/WIFI_PASSWORD_MAX_LEN, ssid is already saved (use
// wifi_update_network_password() instead), or the list is already at
// WIFI_MAX_SAVED_NETWORKS.
bool wifi_add_network(const char *ssid, const char *password);

// Replaces the password of an already-saved ssid. Fails if ssid isn't
// found or password exceeds WIFI_PASSWORD_MAX_LEN. The ssid itself is
// immutable - remove and re-add to rename an entry.
bool wifi_update_network_password(const char *ssid, const char *password);

// Removes a saved network by ssid. Fails (returns false) if it isn't
// found. No effect on whatever the radio is currently connected to.
bool wifi_remove_network(const char *ssid);

// On-device "Join an access point" (ui_epaper.cpp's kWifiManage menu):
// scans for which of the saved networks above are actually in range right
// now, ranked by signal strength, and lets the user pick one to connect to
// immediately - distinct from wifi_add_network() (typing in a brand-new
// ssid/password, done from the web UI), this only ever picks among
// networks already saved.
enum class WifiScanStatus { kRunning, kDone, kFailed };

// Starts an async scan (WiFi.scanNetworks(true) - returns immediately, the
// scan itself runs in the background) after the same driver-settle delay
// wifi_start_standalone_ap() uses. Non-blocking - safe to call directly
// from an LVGL callback, same reasoning as that function. Invalidates any
// previously computed wifi_scan_in_range_count()/..._ssid() results.
void wifi_start_scan();

// Cheap, non-blocking poll of the scan started by wifi_start_scan(). Call
// every loop() iteration while waiting, same idiom as speaker_process()/
// mic_process() (see ui_process_input()).
WifiScanStatus wifi_scan_status();

// Valid once wifi_scan_status() reports kDone (or kFailed - reads as 0
// results either way): how many of the saved networks were actually
// detected in this scan, sorted strongest-first. A saved network the scan
// didn't detect isn't counted - there's nothing to rank it by. Computed
// once per completed scan and cached; wifi_start_scan() invalidates the
// cache for the next one.
int wifi_scan_in_range_count();

// Writes the ssid at sorted rank (0 = strongest signal) into out. Returns
// false if rank is out of range.
bool wifi_scan_get_in_range_ssid(int rank, char *out, size_t outSize);

// Request/process split, same pattern and reentrancy constraint as
// wifi_request_reconnect()/wifi_process_pending_reconnect(): connecting
// blocks for up to WIFI_RECONNECT_TIMEOUT_SECONDS, so
// wifi_process_pending_join() must run from loop() top level, after
// lv_timer_handler() has returned. ssid must already be a saved network -
// its password is looked up internally and never exposed outside this
// module. On a successful connect, ssid also becomes the new preferred/
// default network: every later try_connect()/wifi_ensure_connected() call
// tries it directly first (WIFI_PREFERRED_TIMEOUT_SECONDS budget) before
// falling back to sweeping the whole saved list, same as today.
void wifi_request_join_network(const char *ssid);
void wifi_process_pending_join();

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

#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiMulti.h>
#include <lvgl.h>
#include <time.h>

#include "display.h"
#include "ui.h"
#include "web_server.h"

// -----------------------------------------------------------------------
// WiFi connection manager (tzapu/WiFiManager captive portal).
// -----------------------------------------------------------------------

static const char *PORTAL_SSID = "Annota-Setup";
static const char *STANDALONE_AP_SSID = "Annota-AP";
static bool clockSynced = false;

// Budget for reconnecting to the network already saved in NVS before
// giving up and asking the user to hit Retry (in the timeout dialog or
// the web UI's Settings page) instead of retrying forever. Doesn't apply
// to first-time setup - see run_setup_portal().
static const unsigned long WIFI_RECONNECT_TIMEOUT_SECONDS = 10;

// How often wifi_process_periodic_check() looks in on the connection - see
// its own comment.
static const unsigned long WIFI_HEALTH_CHECK_INTERVAL_MS = 15UL * 60UL * 1000UL;

// -----------------------------------------------------------------------
// Multi-AP saved-network list (web_server.cpp's Settings page). Separate
// from - and layered on top of - WiFiManager's own single ESP-IDF NVS slot
// (which the captive portal still writes to; see
// fold_current_esp_idf_network_into_list() below for how that becomes part
// of this list too). Persisted as indexed Preferences keys under the same
// "annota" namespace transcribe_openai.cpp/sleep.cpp already use, one key
// per fact rather than a serialized blob, to match that existing
// convention and keep this file free of an ArduinoJson dependency it
// doesn't otherwise need.
// -----------------------------------------------------------------------

struct SavedNetwork {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char pass[WIFI_PASSWORD_MAX_LEN + 1];
};

// Reads every saved network into out (must hold WIFI_MAX_SAVED_NETWORKS
// entries) and returns how many were actually saved.
static int load_saved_networks(SavedNetwork out[WIFI_MAX_SAVED_NETWORKS]) {
    Preferences prefs;
    prefs.begin("annota", true);
    int count = prefs.getUChar("netCount", 0);
    if (count > WIFI_MAX_SAVED_NETWORKS) count = WIFI_MAX_SAVED_NETWORKS;  // defensive, shouldn't happen
    for (int i = 0; i < count; i++) {
        char ssidKey[10], passKey[10];
        snprintf(ssidKey, sizeof(ssidKey), "net%dssid", i);
        snprintf(passKey, sizeof(passKey), "net%dpass", i);
        prefs.getString(ssidKey, "").toCharArray(out[i].ssid, sizeof(out[i].ssid));
        prefs.getString(passKey, "").toCharArray(out[i].pass, sizeof(out[i].pass));
    }
    prefs.end();
    return count;
}

// Overwrites the whole saved list with list[0..count). Removes any slot
// keys beyond count so a shrinking list doesn't leave stale strings behind
// in NVS.
static void save_saved_networks(const SavedNetwork *list, int count) {
    Preferences prefs;
    prefs.begin("annota", false);
    prefs.putUChar("netCount", (uint8_t)count);
    for (int i = 0; i < WIFI_MAX_SAVED_NETWORKS; i++) {
        char ssidKey[10], passKey[10];
        snprintf(ssidKey, sizeof(ssidKey), "net%dssid", i);
        snprintf(passKey, sizeof(passKey), "net%dpass", i);
        if (i < count) {
            prefs.putString(ssidKey, list[i].ssid);
            prefs.putString(passKey, list[i].pass);
        } else {
            prefs.remove(ssidKey);
            prefs.remove(passKey);
        }
    }
    prefs.end();
}

int wifi_saved_network_count() {
    Preferences prefs;
    prefs.begin("annota", true);
    int count = prefs.getUChar("netCount", 0);
    prefs.end();
    return count > WIFI_MAX_SAVED_NETWORKS ? WIFI_MAX_SAVED_NETWORKS : count;
}

bool wifi_get_saved_network_ssid(int index, char *out, size_t outSize) {
    SavedNetwork list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_saved_networks(list);
    if (index < 0 || index >= count) return false;
    strncpy(out, list[index].ssid, outSize - 1);
    out[outSize - 1] = '\0';
    return true;
}

bool wifi_add_network(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > WIFI_SSID_MAX_LEN) return false;
    if (password && strlen(password) > WIFI_PASSWORD_MAX_LEN) return false;

    SavedNetwork list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_saved_networks(list);
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) return false;  // already saved - use update instead
    }
    if (count >= WIFI_MAX_SAVED_NETWORKS) return false;

    strncpy(list[count].ssid, ssid, sizeof(list[count].ssid) - 1);
    list[count].ssid[sizeof(list[count].ssid) - 1] = '\0';
    strncpy(list[count].pass, password ? password : "", sizeof(list[count].pass) - 1);
    list[count].pass[sizeof(list[count].pass) - 1] = '\0';
    save_saved_networks(list, count + 1);
    return true;
}

bool wifi_update_network_password(const char *ssid, const char *password) {
    if (!ssid || !password || strlen(password) > WIFI_PASSWORD_MAX_LEN) return false;

    SavedNetwork list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_saved_networks(list);
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            strncpy(list[i].pass, password, sizeof(list[i].pass) - 1);
            list[i].pass[sizeof(list[i].pass) - 1] = '\0';
            save_saved_networks(list, count);
            return true;
        }
    }
    return false;  // not found
}

bool wifi_remove_network(const char *ssid) {
    if (!ssid) return false;

    SavedNetwork list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_saved_networks(list);
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            for (int j = i; j < count - 1; j++) list[j] = list[j + 1];
            save_saved_networks(list, count - 1);
            return true;
        }
    }
    return false;  // not found
}

// Used only by fold_current_esp_idf_network_into_list() below: unlike
// wifi_add_network(), never fails on a duplicate ssid (updates the
// password instead) and, if the list is already full, evicts the oldest
// entry (index 0) to make room - a captive-portal join must never be
// silently dropped just because the list happened to be full, unlike the
// web UI's "Add network" form, which should surface that as an error
// instead of quietly evicting something the user didn't ask to remove.
static void wifi_upsert_network_internal(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return;
    char safeSsid[WIFI_SSID_MAX_LEN + 1];
    strncpy(safeSsid, ssid, sizeof(safeSsid) - 1);
    safeSsid[sizeof(safeSsid) - 1] = '\0';
    char safePass[WIFI_PASSWORD_MAX_LEN + 1];
    strncpy(safePass, password ? password : "", sizeof(safePass) - 1);
    safePass[sizeof(safePass) - 1] = '\0';

    SavedNetwork list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_saved_networks(list);
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, safeSsid) == 0) {
            strncpy(list[i].pass, safePass, sizeof(list[i].pass) - 1);
            list[i].pass[sizeof(list[i].pass) - 1] = '\0';
            save_saved_networks(list, count);
            return;
        }
    }
    if (count >= WIFI_MAX_SAVED_NETWORKS) {
        for (int j = 0; j < count - 1; j++) list[j] = list[j + 1];  // evict oldest
        count--;
    }
    strncpy(list[count].ssid, safeSsid, sizeof(list[count].ssid) - 1);
    list[count].ssid[sizeof(list[count].ssid) - 1] = '\0';
    strncpy(list[count].pass, safePass, sizeof(list[count].pass) - 1);
    list[count].pass[sizeof(list[count].pass) - 1] = '\0';
    save_saved_networks(list, count + 1);
}

static void wifi_clear_all_saved_networks() {
    save_saved_networks(nullptr, 0);
}

// The network the user last picked from the on-device scan-and-join list
// (wifi_request_join_network() below) - tried directly first, before the
// full saved-list sweep, on every later reconnect. Same one-key-per-fact
// Preferences convention as the rest of this section; a plain String
// rather than an index, since indices shift as the saved list is edited
// from the web UI but a ssid string stays a stable reference.
static bool load_preferred_ssid(char *out, size_t outSize) {
    Preferences prefs;
    prefs.begin("annota", true);
    String ssid = prefs.getString("prefSsid", "");
    prefs.end();
    if (ssid.length() == 0) return false;
    ssid.toCharArray(out, outSize);
    return true;
}

static void save_preferred_ssid(const char *ssid) {
    Preferences prefs;
    prefs.begin("annota", false);
    prefs.putString("prefSsid", ssid);
    prefs.end();
}

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

// Folds whatever WiFiManager's captive portal just wrote into ESP-IDF's
// single NVS slot into the new multi-AP list too, so a first-boot portal
// join shows up in the web UI's Settings list without any extra user
// action. Uses
// wifi_upsert_network_internal() rather than wifi_add_network() - a portal
// join must never be silently dropped just because the ssid happens to
// already be saved (updates the password instead) or the list happens to
// be full (evicts the oldest entry instead).
static void fold_current_esp_idf_network_into_list() {
    WiFiManager wm;
    String ssid = wm.getWiFiSSID(true);
    if (ssid.length() == 0) return;
    wifi_upsert_network_internal(ssid.c_str(), wm.getWiFiPass(true).c_str());
}

// First-time setup: no network saved yet, so there's no "reconnect" to
// attempt and no point giving up on its own - the device has no other way
// online. Opens the "Annota-Setup" captive portal AP and waits until either
// the user joins it and picks a network from a phone/laptop (credentials
// entered there are saved to NVS for every boot after this one, and folded
// into the multi-AP list right after - see
// fold_current_esp_idf_network_into_list() above - so it's there for
// wifi_ensure_connected()/reconnect_saved_networks() on every later boot,
// not just this one via the single ESP-IDF slot), or a long Select press
// cancels out of it entirely to work offline instead (see the loop below).
// Non-blocking config portal (setConfigPortalBlocking(false)) rather than
// WiFiManager's own internal wait loop, specifically so this function can
// run its own loop instead and watch for that cancel - the AP callback
// still fires synchronously either way (WiFiManager.cpp fires it before
// checking the blocking flag), so ui_show_wifi_setup_dialog() still forces
// its one paint exactly as before.
static bool run_setup_portal() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(0);
    wm.setConfigPortalBlocking(false);
    wm.setAPCallback([](WiFiManager *) { ui_show_wifi_setup_dialog(PORTAL_SSID); });

    wm.autoConnect(PORTAL_SSID);  // starts the portal (or connects) and returns right away

    bool cancelled = false;
    while (wm.getConfigPortalActive() && WiFi.status() != WL_CONNECTED) {
        wm.process();  // services the portal's HTTP requests - autoConnect()'s own blocking loop would otherwise have done this
        if (display_button_poll(DisplayButton::kSelect) == DisplayButtonEvent::kLong) {
            wm.stopConfigPortal();
            cancelled = true;
            break;
        }
        delay(10);
    }

    bool connected = !cancelled && WiFi.status() == WL_CONNECTED;
    ui_hide_wifi_setup_dialog();
    if (connected) {
        fold_current_esp_idf_network_into_list();
    } else if (cancelled) {
        // "Close the dialog and turn WiFi off" - stopConfigPortal() above
        // only tears down the AP/webserver, not the radio itself.
        Serial.println("WiFi: setup portal cancelled by user - working offline");
        wifi_go_offline();
    }
    return connected;
}

// Tries every network in the saved list (see the multi-AP section above),
// for up to WIFI_RECONNECT_TIMEOUT_SECONDS total, then gives up so this
// doesn't hang here indefinitely - try_connect() falls back to the setup
// portal if nothing at all is saved. WiFiMulti::run() is one blocking
// scan-then-connect call (the scan alone routinely takes 2-4s) that can't
// usefully be sliced into per-second increments the way the old
// single-network loop counted down - re-running run() every second would
// just repeat the whole scan each time without ever finishing a connect -
// so this shows one static status message instead of a countdown.
static bool reconnect_saved_networks() {
    SavedNetwork list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_saved_networks(list);
    if (count == 0) return false;

    WiFi.mode(WIFI_STA);

    // Fast path: whichever network the user last picked from the on-device
    // scan-and-join list (wifi_request_join_network()) gets tried directly
    // first, short budget - only falls through to the full sweep below if
    // it's since gone away or was never set.
    char preferred[WIFI_SSID_MAX_LEN + 1];
    if (load_preferred_ssid(preferred, sizeof(preferred))) {
        for (int i = 0; i < count; i++) {
            if (strcmp(list[i].ssid, preferred) != 0) continue;
            ui_set_wifi_status("Connecting to WiFi...");
            WiFi.begin(list[i].ssid, list[i].pass[0] ? list[i].pass : nullptr);
            unsigned long deadline = millis() + WIFI_PREFERRED_TIMEOUT_SECONDS * 1000UL;
            while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
            if (WiFi.status() == WL_CONNECTED) return true;
            break;
        }
    }

    WiFiMulti wifiMulti;
    for (int i = 0; i < count; i++) {
        wifiMulti.addAP(list[i].ssid, list[i].pass[0] ? list[i].pass : nullptr);
    }
    ui_set_wifi_status("Connecting to WiFi...");
    wifiMulti.run(WIFI_RECONNECT_TIMEOUT_SECONDS * 1000UL);
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
    // WiFi.mode() guarantees the driver is initialized before anything here
    // touches NVS-backed state - a fresh, never-touched driver otherwise
    // reads as stack garbage (see wifi_ensure_connected()'s identical
    // comment). The settle delay is a known arduino-esp32 workaround for
    // the driver's re-init/reload race after wifi_go_offline()'s full
    // esp_wifi_deinit(); harmless (100ms once) on an ordinary boot too.
    WiFi.mode(WIFI_STA);
    delay(100);

    // wifi_saved_network_count() checks the multi-AP list (see the section
    // above); doesn't matter here whether any entry is stale - only an
    // explicit "Delete WiFi Setup" clears it, a failed connect attempt
    // never does.
    bool hasSavedNetwork = wifi_saved_network_count() > 0;

    ui_set_wifi_status("Connecting to WiFi...");
    bool connected = hasSavedNetwork ? reconnect_saved_networks() : false;

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

void wifi_start_boot_connect() {
    WiFi.onEvent(on_wifi_got_ip, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    // See try_connect()'s comment: WiFi.mode() must run before
    // getWiFiIsSaved() so it reads real NVS state instead of uninitialized
    // driver stack garbage.
    WiFi.mode(WIFI_STA);

    // One-time migration for a device already deployed before the multi-AP
    // list existed: it has a network in WiFiManager's single ESP-IDF slot
    // but nothing yet in the new list, which would otherwise look
    // identical to a factory-fresh board and drop it into the setup portal
    // for no reason. Folding it in is enough - every check below already
    // reads the new list, not the old slot.
    WiFiManager wm;
    if (wifi_saved_network_count() == 0 && wm.getWiFiIsSaved()) {
        fold_current_esp_idf_network_into_list();
    }

    if (wifi_saved_network_count() == 0) {
        // Nothing to try in the background - the setup portal is the only
        // way online and needs the user's phone/laptop anyway, so this
        // path stays exactly as blocking as it always was.
        try_connect();
        return;
    }

    // WiFi is off by default - a saved network just means *some* network
    // is available on demand (wifi_ensure_connected(), the manual Online
    // toggle), not that we connect right now. Power the radio straight
    // back off rather than leaving the STA driver idling unassociated.
    Serial.println("WiFi: off by default - network saved, will connect on demand");
    wifi_go_offline();
}

void wifi_request_reconnect() {
    reconnectRequested = true;
}

void wifi_start_standalone_ap(char *ip_out, size_t ip_out_size) {
    // See try_connect()'s comment: switching mode on a radio that was
    // fully powered off (wifi_go_offline()'s esp_wifi_deinit(), or WiFi
    // off by default at boot) needs a settle delay, or the AP comes up
    // visible but rejects every association attempt - same re-init/
    // reload race, just hit here via WIFI_AP instead of WIFI_STA.
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP(STANDALONE_AP_SSID);  // no password, same convention as PORTAL_SSID
    strncpy(ip_out, WiFi.softAPIP().toString().c_str(), ip_out_size - 1);
    ip_out[ip_out_size - 1] = '\0';
    char msg[64];
    snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " AP: %s", STANDALONE_AP_SSID);
    ui_set_wifi_status(msg);
    web_server_start();
    Serial.printf("WiFi: standalone AP '%s' started, IP %s\n", STANDALONE_AP_SSID, ip_out);
}

void wifi_process_pending_reconnect() {
    if (!reconnectRequested) return;
    reconnectRequested = false;
    ui_hide_wifi_setup_dialog();
    if (try_connect()) web_server_start();
}

// -----------------------------------------------------------------------
// On-device scan-and-join (ui_epaper.cpp's kWifiManage "Join an access
// point"): which of the saved networks above are actually in range right
// now, ranked by signal strength. See wifi_manager.h's declarations for
// the public contract.
// -----------------------------------------------------------------------

struct ScanMatch {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    int32_t rssi;
};
static ScanMatch scanMatches[WIFI_MAX_SAVED_NETWORKS];
static int scanMatchCount = -1;  // -1 = not yet computed for the current completed scan

void wifi_start_scan() {
    WiFi.mode(WIFI_STA);
    delay(100);  // settle - same reasoning as wifi_start_standalone_ap()'s
    WiFi.scanDelete();
    scanMatchCount = -1;  // invalidate any previous scan's cached matches
    WiFi.scanNetworks(true /* async */);
}

WifiScanStatus wifi_scan_status() {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return WifiScanStatus::kRunning;
    if (n == WIFI_SCAN_FAILED) return WifiScanStatus::kFailed;
    return WifiScanStatus::kDone;
}

// Cross-references the completed scan against the saved list, keeping only
// matches, sorted strongest-first. Frees the scan's own results
// (WiFi.scanDelete()) once copied out - nothing else needs them raw.
static void ensure_scan_matches_computed() {
    if (scanMatchCount >= 0) return;  // already computed for this scan

    int16_t found = WiFi.scanComplete();
    scanMatchCount = 0;
    if (found <= 0) return;  // still running, failed, or genuinely zero networks nearby

    SavedNetwork saved[WIFI_MAX_SAVED_NETWORKS];
    int savedCount = load_saved_networks(saved);
    for (int i = 0; i < savedCount; i++) {
        int32_t bestRssi = INT32_MIN;
        bool matched = false;
        for (int j = 0; j < found; j++) {
            if (WiFi.SSID(j) != saved[i].ssid) continue;
            matched = true;
            if (WiFi.RSSI(j) > bestRssi) bestRssi = WiFi.RSSI(j);
        }
        if (!matched) continue;
        strncpy(scanMatches[scanMatchCount].ssid, saved[i].ssid, sizeof(scanMatches[scanMatchCount].ssid) - 1);
        scanMatches[scanMatchCount].ssid[sizeof(scanMatches[scanMatchCount].ssid) - 1] = '\0';
        scanMatches[scanMatchCount].rssi = bestRssi;
        scanMatchCount++;
    }
    WiFi.scanDelete();

    // Insertion sort by rssi descending - at most WIFI_MAX_SAVED_NETWORKS
    // entries, not worth anything fancier.
    for (int i = 1; i < scanMatchCount; i++) {
        ScanMatch key = scanMatches[i];
        int j = i - 1;
        while (j >= 0 && scanMatches[j].rssi < key.rssi) {
            scanMatches[j + 1] = scanMatches[j];
            j--;
        }
        scanMatches[j + 1] = key;
    }
}

int wifi_scan_in_range_count() {
    ensure_scan_matches_computed();
    return scanMatchCount;
}

bool wifi_scan_get_in_range_ssid(int rank, char *out, size_t outSize) {
    ensure_scan_matches_computed();
    if (rank < 0 || rank >= scanMatchCount) return false;
    strncpy(out, scanMatches[rank].ssid, outSize - 1);
    out[outSize - 1] = '\0';
    return true;
}

// Set by wifi_request_join_network(), consumed once by
// wifi_process_pending_join() from loop() - same reentrancy reasoning as
// reconnectRequested above.
static volatile bool joinRequested = false;
static char pendingJoinSsid[WIFI_SSID_MAX_LEN + 1];

void wifi_request_join_network(const char *ssid) {
    strncpy(pendingJoinSsid, ssid, sizeof(pendingJoinSsid) - 1);
    pendingJoinSsid[sizeof(pendingJoinSsid) - 1] = '\0';
    joinRequested = true;
}

void wifi_process_pending_join() {
    if (!joinRequested) return;
    joinRequested = false;

    SavedNetwork list[WIFI_MAX_SAVED_NETWORKS];
    int count = load_saved_networks(list);
    const char *pass = nullptr;
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, pendingJoinSsid) == 0) {
            pass = list[i].pass[0] ? list[i].pass : nullptr;
            found = true;
            break;
        }
    }
    if (!found) return;  // removed from the saved list between selection and now

    WiFi.mode(WIFI_STA);
    ui_set_wifi_status("Connecting to WiFi...");
    WiFi.begin(pendingJoinSsid, pass);
    unsigned long deadline = millis() + WIFI_RECONNECT_TIMEOUT_SECONDS * 1000UL;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);

    if (WiFi.status() == WL_CONNECTED) {
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        ui_set_wifi_status(msg);
        Serial.println(msg);
        sync_clock_via_ntp();
        save_preferred_ssid(pendingJoinSsid);
        web_server_start();
    } else {
        ui_set_wifi_status(LV_SYMBOL_WARNING " working offline");
        Serial.printf("WiFi: join \"%s\" failed - continuing offline\n", pendingJoinSsid);
    }
    ui_refresh_wifi_retry_button();
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

    // See try_connect()'s comment: the wifi driver needs to be initialized
    // to read real NVS state instead of stack garbage, and (if called
    // after wifi_go_offline() fully powered the radio off) the settle
    // delay to avoid its re-init/reload race.
    WiFi.mode(WIFI_STA);
    delay(100);

    if (wifi_saved_network_count() == 0) {
        // Nothing to even try - power back off (this call is the only
        // thing that just touched the radio) rather than leave it
        // initialized-but-unassociated.
        Serial.println("WiFi: no saved network - can't connect for transcription");
        wifi_go_offline();
        return false;
    }

    ui_set_wifi_status("Connecting to WiFi...");
    bool connected = reconnect_saved_networks();
    if (connected) {
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        ui_set_wifi_status(msg);
        Serial.println(msg);
        sync_clock_via_ntp();
        ui_refresh_wifi_retry_button();
    } else {
        Serial.println("WiFi: reconnect for transcription failed - still offline");
        wifi_go_offline();
    }
    return connected;
}

// Unlike wifi_request_reconnect(), this doesn't need to defer through
// loop() - it never returns, so there's no repaint afterwards that could
// silently no-op from running nested inside lv_timer_handler().
void wifi_forget_and_reboot() {
    WiFiManager wm;
    wm.resetSettings();
    wifi_clear_all_saved_networks();
    Preferences prefs;
    prefs.begin("annota", false);
    prefs.remove("prefSsid");
    prefs.end();
    Serial.println("WiFi: saved network erased by user - rebooting into setup portal");
    delay(200);
    ESP.restart();
}

bool wifi_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifi_go_offline() {
    WiFi.disconnect(true);  // true = power off the radio too (battery life); leaves NVS credentials alone
    ui_set_wifi_status("");
    ui_refresh_wifi_retry_button();
    Serial.println("WiFi: radio off");
}

// Only fires this often (see WIFI_HEALTH_CHECK_INTERVAL_MS) - a plain
// status check every loop() iteration would be free, but there's nothing
// to react to faster than the AP itself would ever flap.
static unsigned long lastHealthCheckMs = 0;

void wifi_process_periodic_check() {
    unsigned long now = millis();
    if (now - lastHealthCheckMs < WIFI_HEALTH_CHECK_INTERVAL_MS) return;
    lastHealthCheckMs = now;

    if (WiFi.getMode() == WIFI_MODE_NULL) return;  // radio already off - already offline, nothing to check
    if (WiFi.status() == WL_CONNECTED) return;      // still fine

    // Radio's on but not associated - the underlying esp_wifi/lwIP
    // auto-reconnect (see wifi_start_boot_connect()'s WiFi.onEvent()
    // comment) has been quietly failing behind the header status line for
    // a while. Give up gracefully instead of leaving it burning power
    // retrying against an AP that's gone - same call the on-device
    // "Offline" menu item makes (wifi_go_offline(): radio off, saved
    // network untouched, no dialog), so this is silent exactly the same
    // way that is.
    Serial.println("WiFi: periodic check found the AP unreachable - going offline to save power");
    wifi_go_offline();
}

#include "sleep.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include "ui.h"
#include "web_server.h"

// Select/PWR button display_epaper.cpp drives (its own PWR_BUTTON_PIN,
// private to that file) - duplicated here rather than exposed through
// display.h since nothing else needs it outside this wakeup mask. Only
// this one button wakes the device (see ui_show_sleep_screen()'s "Hold
// Select to wake" - BOOT/Next is left out of the mask so that message
// stays true instead of being a second, undocumented way to wake it).
static const int PWR_BUTTON_PIN = 18;

// Default before the user ever touches the Settings page slider, and the
// clamp range for whatever they set it to - generous ceiling (3 hours) so
// a heavy user recording/browsing on and off all day doesn't fight the
// timeout, floor of 1 minute so the slider can't be dragged down to
// something that deep-sleeps the device out from under normal use.
static const uint16_t IDLE_TIMEOUT_MIN_DEFAULT = 30;
static const uint16_t IDLE_TIMEOUT_MIN_MIN = 1;
static const uint16_t IDLE_TIMEOUT_MIN_MAX = 180;

// Same NVS namespace as transcribe_openai.cpp's API key storage.
static const char *NVS_KEY = "idleMin";

static uint32_t lastActivityMs = 0;

// 0 = not loaded from NVS yet - never a valid stored value once loaded,
// since sleep_set_idle_timeout_minutes() clamps to
// [IDLE_TIMEOUT_MIN_MIN, IDLE_TIMEOUT_MIN_MAX], both >= 1. Doubles as the
// lazy-load sentinel for sleep_get_idle_timeout_minutes().
static uint16_t idleTimeoutMinutes = 0;

static void ensure_idle_timeout_loaded() {
    if (idleTimeoutMinutes != 0) return;
    Preferences prefs;
    prefs.begin("annota", true);
    uint16_t minutes = prefs.getUShort(NVS_KEY, IDLE_TIMEOUT_MIN_DEFAULT);
    prefs.end();
    if (minutes < IDLE_TIMEOUT_MIN_MIN || minutes > IDLE_TIMEOUT_MIN_MAX) minutes = IDLE_TIMEOUT_MIN_DEFAULT;
    idleTimeoutMinutes = minutes;
}

uint16_t sleep_get_idle_timeout_minutes() {
    ensure_idle_timeout_loaded();
    return idleTimeoutMinutes;
}

void sleep_set_idle_timeout_minutes(uint16_t minutes) {
    if (minutes < IDLE_TIMEOUT_MIN_MIN) minutes = IDLE_TIMEOUT_MIN_MIN;
    if (minutes > IDLE_TIMEOUT_MIN_MAX) minutes = IDLE_TIMEOUT_MIN_MAX;
    idleTimeoutMinutes = minutes;
    Preferences prefs;
    prefs.begin("annota", false);
    prefs.putUShort(NVS_KEY, minutes);
    prefs.end();
}

void sleep_reset_activity() {
    lastActivityMs = millis();
}

static void enter_deep_sleep() {
    ui_show_sleep_screen(); // paints, so do it before tearing down WiFi
    delay(50);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // PWR_HOLD_PIN (GPIO17, latched HIGH by main.cpp's
    // keepBatteryPowerOn()) is deliberately left alone here - deep sleep
    // still needs the regulator latched on for ext1 wakeup to fire at all;
    // only pulling the physical power switch again cuts it for good.
    uint64_t wakeMask = (1ULL << PWR_BUTTON_PIN);
    esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

void sleep_process_idle() {
    if (ui_is_sleep_blocked()) return; // recording/playing/transcribing - see ui.h
    if (web_transcribe_in_progress()) return; // browser-side transcription - see web_server.h
    uint32_t timeoutMs = (uint32_t)sleep_get_idle_timeout_minutes() * 60000UL;
    if (millis() - lastActivityMs > timeoutMs) enter_deep_sleep();
}

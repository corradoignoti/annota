#include "sleep.h"

#include <Arduino.h>
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

// Matches pala_note's ULTRA_SLEEP_MS - same board family, same battery
// budget reasoning (see sleep.h).
static const uint32_t IDLE_TIMEOUT_MS = 120000UL;

static uint32_t lastActivityMs = 0;

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
    if (millis() - lastActivityMs > IDLE_TIMEOUT_MS) enter_deep_sleep();
}

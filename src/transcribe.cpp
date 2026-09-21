#include "transcribe.h"

#include <cstring>

#include "display.h"
#include "sleep.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

// -----------------------------------------------------------------------
// Provider-agnostic half of transcribe.h: the deferred-dispatch pair
// (mirrors wifi_manager.cpp's reconnect pair, see transcribe_request()'s
// comment) that calls into whichever provider's ai_transcribe_file() is
// compiled in. ai_provider_name()/ai_provider_*_api_key()/
// ai_transcribe_file() itself live in transcribe_<provider>.cpp instead -
// see transcribe.h's top comment for how the provider is selected.
// -----------------------------------------------------------------------

// Extend this line when adding a provider (transcribe_<provider>.cpp,
// guarded by its own #ifdef AI_PROVIDER_<PROVIDER>) so a platformio.ini
// missing (or misspelling) its build flag fails at compile time here
// instead of as a confusing "undefined reference to ai_transcribe_file()"
// link error.
#if !defined(AI_PROVIDER_OPENAI) && !defined(AI_PROVIDER_GEMINI)
#error "No AI_PROVIDER_* build flag defined - add one (e.g. -D AI_PROVIDER_OPENAI=1) to platformio.ini's build_flags to select a transcription provider."
#endif

static volatile bool transcribeRequested = false;
static char transcribeTargetFilename[64];

void transcribe_request(const char *filename) {
    strncpy(transcribeTargetFilename, filename, sizeof(transcribeTargetFilename) - 1);
    transcribeTargetFilename[sizeof(transcribeTargetFilename) - 1] = '\0';
    transcribeRequested = true;
}

void transcribe_process_pending() {
    if (!transcribeRequested) return;
    transcribeRequested = false;

    ui_show_transcribe_progress(transcribeTargetFilename);

    // WiFi is off by default (see wifi_manager.h) - transcribing is the
    // one place that needs the device online. Only turn it on (and take
    // responsibility for turning it back off below) if it wasn't already
    // on for some other reason, e.g. a manual Online session browsing the
    // web file manager - that session shouldn't get cut out from under the
    // user just because an on-device transcription also ran.
    bool wifiWasOnAlready = wifi_is_connected();
    bool weTurnedWifiOn = false;
    if (!wifiWasOnAlready) {
        if (!wifi_ensure_connected()) {
            // wifi_ensure_connected() already powered the radio back off
            // on failure - nothing left to clean up here.
            sleep_reset_activity();
            ui_show_transcribe_result(false, "No WiFi connection.");
            return;
        }
        weTurnedWifiOn = true;
        // Idempotent - may be the first WiFi connection since boot, since
        // boot no longer auto-connects.
        web_server_start();
    }

    display_suspend_touch();
    char err[96];
    bool ok = ai_transcribe_file(transcribeTargetFilename, err, sizeof(err));
    display_resume_touch();

    // Turn back off before the result screen paints, so the header
    // doesn't flash a stale "connected" status - header_label persists
    // across every screen, including kTranscribeResult.
    if (weTurnedWifiOn) wifi_go_offline();

    // A big/slow upload can easily run past sleep.h's idle timeout on its
    // own - without this, sleep_process_idle() (running right after this
    // same loop() pass, once ui_is_sleep_blocked() no longer sees
    // kTranscribeProgress) would deep-sleep the instant the result screen
    // below appears, before the user ever gets to read it.
    sleep_reset_activity();
    ui_show_transcribe_result(ok, ok ? "Transcription saved." : err);
}

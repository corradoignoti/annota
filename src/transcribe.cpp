#include "transcribe.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdarg>
#include <cstring>
#include <ctime>

#include "display.h"
#include "i18n.h"
#include "sleep.h"
#include "storage.h"
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

// -----------------------------------------------------------------------
// Progress + diagnostics hooks (see transcribe.h). State only lives for
// the duration of one transcribe_process_pending() call.
// -----------------------------------------------------------------------

// Caps the diagnostics log so a pathological run (many retries, a huge
// error body) can't eat the heap - lines past this are dropped.
static const size_t LOG_MAX = 4096;
static String logText;
static bool logTruncated = false;

static TranscribePhase currentPhase = TranscribePhase::kConnecting;
static int lastReportedStep = -1;    // last 10% step repainted, -1 = none yet
static int lastReportedAttempt = 0;

static const char *phase_label(TranscribePhase phase) {
    switch (phase) {
        case TranscribePhase::kConnecting: return tr(Str::PHASE_CONNECTING);
        case TranscribePhase::kUploading: return tr(Str::PHASE_UPLOADING);
        case TranscribePhase::kWaiting: return tr(Str::PHASE_WAITING);
        case TranscribePhase::kSummarizing: return tr(Str::PHASE_SUMMARIZING);
        case TranscribePhase::kSaving: return tr(Str::PHASE_SAVING);
    }
    return "";
}

void transcribe_log(const char *fmt, ...) {
    // Small lines format on the stack; longer ones (e.g. a provider's
    // error body) spill to the heap rather than growing this frame - this
    // can run deep inside HTTPClient's/mbedTLS's own call stack.
    char stackLine[160];
    char *line = stackLine;
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(stackLine, sizeof(stackLine), fmt, args);
    va_end(args);
    if (len < 0) return;
    if ((size_t)len >= sizeof(stackLine)) {
        line = (char *)malloc(len + 1);
        if (!line) {
            line = stackLine; // keep the truncated copy
        } else {
            va_start(args, fmt);
            vsnprintf(line, len + 1, fmt, args);
            va_end(args);
        }
    }

    Serial.printf("transcribe: %s\n", line);
    if (!logTruncated) {
        if (logText.length() + strlen(line) + 1 > LOG_MAX) {
            logText += "...log truncated\n";
            logTruncated = true;
        } else {
            logText += line;
            logText += '\n';
        }
    }
    if (line != stackLine) free(line);
}

void transcribe_report_phase(TranscribePhase phase) {
    currentPhase = phase;
    transcribe_log("Phase: %s", phase_label(phase));
    ui_update_transcribe_progress(phase_label(phase), -1, "");
}

void transcribe_report_upload(size_t sent, size_t total, int attempt, int maxAttempts) {
    int percent = total > 0 ? (int)((uint64_t)sent * 100 / total) : 0;
    if (percent > 100) percent = 100;
    int step = percent / 10;
    if (currentPhase == TranscribePhase::kUploading && attempt == lastReportedAttempt && step == lastReportedStep) {
        return;
    }
    currentPhase = TranscribePhase::kUploading;
    lastReportedAttempt = attempt;
    lastReportedStep = step;

    char detail[64];
    int n = snprintf(detail, sizeof(detail), "%d%%  %.1f/%.1f MB", percent, sent / 1048576.0, total / 1048576.0);
    if (attempt > 1 && n > 0 && (size_t)n < sizeof(detail)) {
        snprintf(detail + n, sizeof(detail) - n, tr(Str::TRANSCRIBE_RETRY), attempt, maxAttempts);
    }
    ui_update_transcribe_progress(phase_label(TranscribePhase::kUploading), percent, detail);
}

// "song.mp3" -> "song_error.txt" (no leading '/', storage.h's helpers add
// it). Modeled on transcribe_<provider>.cpp's txt_sibling_path().
static void error_log_name(const char *filename, char *out, size_t outLen) {
    static const char *SUFFIX = "_error.txt";
    const char *dot = strrchr(filename, '.');
    size_t baseLen = dot ? (size_t)(dot - filename) : strlen(filename);
    size_t maxBase = outLen - strlen(SUFFIX) - 1;
    if (baseLen > maxBase) baseLen = maxBase;
    memcpy(out, filename, baseLen);
    strcpy(out + baseLen, SUFFIX);
}

static void log_header(const char *filename) {
    logText = "";
    logTruncated = false;
    currentPhase = TranscribePhase::kConnecting;
    lastReportedStep = -1;
    lastReportedAttempt = 0;

    transcribe_log("Annota transcription log");
    transcribe_log("File: %s", filename);
    transcribe_log("Provider: %s", ai_provider_name());
    if (wifi_clock_synced()) {
        time_t now = time(nullptr);
        struct tm tmInfo;
        localtime_r(&now, &tmInfo);
        char when[24];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmInfo);
        transcribe_log("Time: %s (uptime %lu s)", when, (unsigned long)(millis() / 1000));
    } else {
        transcribe_log("Time: clock not synced (uptime %lu s)", (unsigned long)(millis() / 1000));
    }
    transcribe_log("Free heap: %u bytes", (unsigned)ESP.getFreeHeap());
}

// On failure, saves the log next to the audio file and points the result
// message at it; on success, removes any stale log from an earlier failed
// attempt at the same file.
static void finish_log(bool ok, const char *filename, char *message, size_t messageLen) {
    char logName[80];
    error_log_name(filename, logName, sizeof(logName));
    if (ok) {
        delete_file(logName); // usually absent - failure is expected and harmless
    } else {
        transcribe_log("Failed during: %s", phase_label(currentPhase));
        transcribe_log("Result: %s", message);
        if (write_text_file(logName, logText.c_str())) {
            size_t len = strlen(message);
            snprintf(message + len, messageLen - len, tr(Str::TRANSCRIBE_DETAILS_SUFFIX), logName);
        }
    }
    logText = ""; // free the buffer until the next run
}

// A transcription parked because no saved network was reachable - see
// transcribe.h's transcribe_is_waiting_for_wifi(). resumedAfterJoin marks
// the re-queued run so it still powers WiFi off afterwards, even though
// WiFi is already connected by the time it starts.
static bool waitingForWifi = false;
static bool resumedAfterJoin = false;

bool transcribe_is_waiting_for_wifi() { return waitingForWifi; }

void transcribe_resume_after_join(bool joined) {
    if (!waitingForWifi) return;
    waitingForWifi = false;
    if (joined) {
        resumedAfterJoin = true;
        transcribeRequested = true; // transcribeTargetFilename still holds it
        return;
    }
    wifi_go_offline();
    sleep_reset_activity();
    ui_show_transcribe_result(false, tr(Str::ERR_WIFI_JOIN_FAILED));
}

void transcribe_cancel_wifi_wait() {
    if (!waitingForWifi) return;
    waitingForWifi = false;
    wifi_go_offline();
}

void transcribe_process_pending() {
    if (!transcribeRequested) return;
    transcribeRequested = false;
    bool resumed = resumedAfterJoin;
    resumedAfterJoin = false;

    ui_show_transcribe_progress(transcribeTargetFilename);
    log_header(transcribeTargetFilename);

    // WiFi is off by default (see wifi_manager.h) - transcribing is the
    // one place that needs the device online. Only turn it on (and take
    // responsibility for turning it back off below) if it wasn't already
    // on for some other reason, e.g. a manual Online session browsing the
    // web file manager - that session shouldn't get cut out from under the
    // user just because an on-device transcription also ran.
    bool wifiWasOnAlready = wifi_is_connected() && !resumed;
    bool weTurnedWifiOn = resumed;
    char message[192];
    if (!wifi_is_connected()) {
        transcribe_report_phase(TranscribePhase::kConnecting);
        if (!wifi_ensure_connected()) {
            // No saved network in reach - not a transcription failure, so
            // no error log: park the request and let the user pick an
            // access point from the scan list instead.
            // wifi_ensure_connected() already powered the radio back off;
            // the scan below turns it on again, and resume/cancel above
            // turns it back off.
            Serial.println("transcribe: no saved network reachable, asking user to pick one");
            waitingForWifi = true;
            sleep_reset_activity();
            ui_show_wifi_join_for_transcribe();
            return;
        }
        weTurnedWifiOn = true;
        // Idempotent - may be the first WiFi connection since boot, since
        // boot no longer auto-connects.
        web_server_start();
    }
    transcribe_log("WiFi: connected (%s), RSSI %d dBm", wifiWasOnAlready ? "already on" : "turned on for this transcription",
                   (int)WiFi.RSSI());

    display_suspend_touch();
    char err[128];
    bool ok = ai_transcribe_file(transcribeTargetFilename, err, sizeof(err));
    display_resume_touch();

    // Turn back off before the result screen paints, so the header
    // doesn't flash a stale "connected" status - header_label persists
    // across every screen, including kTranscribeResult.
    if (weTurnedWifiOn) wifi_go_offline();

    strncpy(message, ok ? tr(Str::TRANSCRIBE_SAVED) : err, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    finish_log(ok, transcribeTargetFilename, message, sizeof(message));

    // A big/slow upload can easily run past sleep.h's idle timeout on its
    // own - without this, sleep_process_idle() (running right after this
    // same loop() pass, once ui_is_sleep_blocked() no longer sees
    // kTranscribeProgress) would deep-sleep the instant the result screen
    // below appears, before the user ever gets to read it.
    sleep_reset_activity();
    ui_show_transcribe_result(ok, message);
}

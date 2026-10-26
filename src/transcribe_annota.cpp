// Annota speech-to-text provider - only compiled in when platformio.ini's
// build_flags define AI_PROVIDER_ANNOTA (see transcribe.h's top comment
// for the provider-selection scheme). Compiles to an empty translation
// unit otherwise, so it's safe for this file to always be in src/
// regardless of which provider is actually selected.
#ifdef AI_PROVIDER_ANNOTA

#include "transcribe.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "annota_ca.h"
#include "i18n.h"
#include "storage.h"
#include "transcribe_upload.h"

// -----------------------------------------------------------------------
// Annota speech-to-text API (self-hosted faster-whisper, docs at
// API_BASE + "/docs"). Asynchronous, unlike OpenAI: POST /jobs uploads
// the file and returns a queued job, GET /jobs/{id} is polled until its
// status is "done" (text) or "failed" (error). Returns the transcript
// only - no title/abstract header, so storage.cpp's title falls back to
// the transcript's first line. No `language` field is sent: the server's
// default applies.
// -----------------------------------------------------------------------

static const char *API_HOST = "annota-api.fizban.net";
static const char *API_BASE = "https://annota-api.fizban.net";
// Arbitrary, just has to not appear inside the request body it wraps.
static const char *BOUNDARY = "----AnnotaBoundary7MA4YWxkTrZu0gW";
// Jobs are processed one at a time server-side, so the wait includes
// whatever is queued ahead of this one.
static const unsigned long POLL_INTERVAL_MS = 3000;
static const unsigned long JOB_TIMEOUT_MS = 15UL * 60UL * 1000UL;
// Consecutive poll requests that never got an HTTP answer (TLS/socket
// errors) before giving up - a single dropped poll is not worth failing a
// transcription the server is still working on.
static const int MAX_POLL_ERRORS = 5;

// NVS key namespaced by provider (not just "apiKey") so switching which
// AI_PROVIDER_* is compiled in doesn't silently feed a stale key saved
// for a different provider into this one, or vice versa.
static const char *NVS_KEY = "annotaKey";

static Preferences prefs;

const char *ai_provider_name() { return "Annota"; }

bool ai_provider_has_api_key() {
    prefs.begin("annota", true);
    String key = prefs.getString(NVS_KEY, "");
    prefs.end();
    return key.length() > 0;
}

void ai_provider_get_api_key(char *out, size_t outLen) {
    prefs.begin("annota", true);
    String key = prefs.getString(NVS_KEY, "");
    prefs.end();
    key.toCharArray(out, outLen);
}

void ai_provider_set_api_key(const char *key) {
    prefs.begin("annota", false);
    prefs.putString(NVS_KEY, key);
    prefs.end();
}

// The API's own error message ({"detail": "..."}) if `response` carries
// one, otherwise "HTTP <code>" (plus HTTPClient's reason for its own
// negative connection-layer codes - see transcribe_openai.cpp's matching
// branch).
static String error_message(int code, const String &response) {
    JsonDocument doc;
    if (deserializeJson(doc, response) == DeserializationError::Ok && doc["detail"].is<const char *>()) {
        return doc["detail"].as<const char *>();
    }
    if (code == HTTPC_ERROR_SEND_PAYLOAD_FAILED) return tr(Str::ERR_UPLOAD_INTERRUPTED);
    if (code < 0) return "HTTP " + String(code) + " (" + HTTPClient::errorToString(code) + ")";
    return "HTTP " + String(code);
}

// One small authenticated request (GET/DELETE /jobs/{id}) - returns the
// HTTP code and fills `response`.
static int job_request(const char *method, const char *apiKey, const String &jobId, String &response) {
    WiFiClientSecure client;
    client.setCACert(ANNOTA_ROOT_CA);
    HTTPClient http;
    // Both timeouts for the same reason as the upload's setConnectTimeout()
    // comment in ai_transcribe_file().
    http.setTimeout(30000);
    http.setConnectTimeout(30000);
    if (!http.begin(client, String(API_BASE) + "/jobs/" + jobId)) {
        response = String();
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }
    // "Connection: close", so the server ends the connection after the
    // reply instead of keeping it alive for a next request that never comes.
    http.setReuse(false);
    http.addHeader("X-API-Key", apiKey);
    int code = http.sendRequest(method);
    // Never read a 204's body: it has no Content-Length, which HTTPClient's
    // getString() takes as "read until the connection closes" - on a
    // kept-alive connection that blocked DELETE's reply indefinitely.
    response = code > 0 && code != HTTP_CODE_NO_CONTENT ? http.getString() : String();
    if (code < 0) transcribe_log("%s job: HTTP %d %s%s", method, code, HTTPClient::errorToString(code).c_str(),
                                 tls_error_suffix(client).c_str());
    http.end();
    return code;
}

// Uploads `src` as a new job (POST /jobs) and copies its id into jobId.
// Same retry loop as transcribe_openai.cpp's upload - see its comments
// there for why a whole-file retry on HTTPC_ERROR_SEND_PAYLOAD_FAILED is
// what actually recovers.
static bool create_job(const char *filename, File &src, const char *apiKey, String &jobId, char *errOut,
                       size_t errOutLen) {
    String preamble;
    preamble += "--";
    preamble += BOUNDARY;
    preamble += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    preamble += filename;
    preamble += "\"\r\nContent-Type: ";
    preamble += upload_mime_type_for(filename);
    preamble += "\r\n\r\n";

    String trailer;
    trailer += "\r\n--";
    trailer += BOUNDARY;
    trailer += "--\r\n";

    size_t contentLength = preamble.length() + src.size() + trailer.length();

    static const int kMaxAttempts = 6;
    static const int kBackoffMs[kMaxAttempts] = {0, 300, 900, 2000, 4000, 8000};
    int code = 0;
    String response;
    // Modem sleep off for the upload only - see transcribe_openai.cpp.
    bool wasSleepEnabled = WiFi.getSleep();
    WiFi.setSleep(false);
    for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
        if (attempt > 0) {
            if (WiFi.status() != WL_CONNECTED) {
                code = HTTPC_ERROR_CONNECTION_LOST;
                transcribe_log("WiFi lost before attempt %d/%d, giving up", attempt + 1, kMaxAttempts);
                break;
            }
            src.seek(0);
            delay(kBackoffMs[attempt]);
        }

        UploadClient client;
        // Verify the server's cert against annota_ca.h, so a MITM can't
        // read the API key out of the X-API-Key header.
        client.setCACert(ANNOTA_ROOT_CA);

        HTTPClient http;
        http.setTimeout(60000);
        // HTTPClient's separate connect timeout also becomes the mbedTLS
        // socket timeout for the whole upload - see transcribe_openai.cpp.
        http.setConnectTimeout(60000);
        if (!http.begin(client, String(API_BASE) + "/jobs")) {
            code = HTTPC_ERROR_CONNECTION_REFUSED;
            transcribe_log("Attempt %d/%d: http.begin() failed", attempt + 1, kMaxAttempts);
            continue;
        }
        http.addHeader("X-API-Key", apiKey);
        http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);

        MultipartStream body(preamble, src, trailer, attempt + 1, kMaxAttempts);
        transcribe_log("Attempt %d/%d: connecting, free heap %u bytes, RSSI %d dBm", attempt + 1, kMaxAttempts,
                       (unsigned)ESP.getFreeHeap(), (int)WiFi.RSSI());
        transcribe_report_upload(0, contentLength, attempt + 1, kMaxAttempts);
        unsigned long startMs = millis();
        code = http.sendRequest("POST", &body, contentLength);
        response = http.getString();
        String tlsError = code < 0 ? tls_error_suffix(client) : String();
        http.end();

        transcribe_log("Attempt %d/%d: HTTP %d%s%s%s, sent %u/%u bytes, %lu ms, RSSI %d dBm", attempt + 1,
                       kMaxAttempts, code, code < 0 ? " " : "", code < 0 ? HTTPClient::errorToString(code).c_str() : "",
                       tlsError.c_str(), (unsigned)body.served(), (unsigned)contentLength, millis() - startMs,
                       (int)WiFi.RSSI());
        if (code != HTTPC_ERROR_SEND_PAYLOAD_FAILED) break;
    }
    WiFi.setSleep(wasSleepEnabled);

    if (code != 202 && code != 200) {
        if (response.length() > 0) {
            transcribe_log("Response body (first 1 KB):");
            transcribe_log("%.1024s", response.c_str());
        }
        error_message(code, response).toCharArray(errOut, errOutLen);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok || !doc["id"].is<const char *>()) {
        snprintf(errOut, errOutLen, tr(Str::ERR_UNEXPECTED_RESPONSE), ai_provider_name());
        transcribe_log("Error: %s. Body (first 1 KB):", errOut);
        transcribe_log("%.1024s", response.c_str());
        return false;
    }
    jobId = doc["id"].as<const char *>();
    transcribe_log("Job %s created", jobId.c_str());
    return true;
}

// Polls GET /jobs/{jobId} until the job is done (text into `transcript`)
// or failed, the poll keeps erroring, or JOB_TIMEOUT_MS passes.
static bool wait_for_job(const char *apiKey, const String &jobId, String &transcript, char *errOut,
                         size_t errOutLen) {
    transcribe_report_phase(TranscribePhase::kWaiting);
    unsigned long startMs = millis();
    String lastStatus;
    int lastQueuePos = -1;
    int pollErrors = 0;
    while (true) {
        if (millis() - startMs > JOB_TIMEOUT_MS) {
            set_err(errOut, errOutLen, tr(Str::ERR_JOB_TIMEOUT));
            transcribe_log("Error: %s (last status: %s)", errOut, lastStatus.c_str());
            return false;
        }
        if (WiFi.status() != WL_CONNECTED) {
            set_err(errOut, errOutLen, tr(Str::ERR_WIFI_NOT_CONNECTED));
            transcribe_log("Error: %s while waiting for job", errOut);
            return false;
        }
        delay(POLL_INTERVAL_MS);

        String response;
        int code = job_request("GET", apiKey, jobId, response);
        if (code < 0) {
            if (++pollErrors >= MAX_POLL_ERRORS) {
                error_message(code, response).toCharArray(errOut, errOutLen);
                return false;
            }
            continue;
        }
        pollErrors = 0;
        if (code != 200) {
            transcribe_log("Poll: HTTP %d. Body (first 1 KB):", code);
            transcribe_log("%.1024s", response.c_str());
            error_message(code, response).toCharArray(errOut, errOutLen);
            return false;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response) != DeserializationError::Ok || !doc["status"].is<const char *>()) {
            snprintf(errOut, errOutLen, tr(Str::ERR_UNEXPECTED_RESPONSE), ai_provider_name());
            transcribe_log("Error: %s. Body (first 1 KB):", errOut);
            transcribe_log("%.1024s", response.c_str());
            return false;
        }
        String status = doc["status"].as<const char *>();
        int queuePos = doc["queue_position"] | -1;
        unsigned long elapsedS = (millis() - startMs) / 1000;
        if (status == "queued" || status == "processing") {
            transcribe_report_job(status == "processing" ? TranscribeJobState::kProcessing
                                                         : TranscribeJobState::kQueued,
                                  queuePos, elapsedS);
        }
        if (status != lastStatus || queuePos != lastQueuePos) {
            transcribe_log("Job %s after %lu s, queue position %d", status.c_str(), elapsedS, queuePos);
            lastStatus = status;
            lastQueuePos = queuePos;
        }
        if (status == "done") {
            if (!doc["text"].is<const char *>()) {
                snprintf(errOut, errOutLen, tr(Str::ERR_UNEXPECTED_RESPONSE), ai_provider_name());
                transcribe_log("Error: %s (done without text)", errOut);
                return false;
            }
            transcript = doc["text"].as<const char *>();
            transcript.trim();
            if (doc["audio_duration_seconds"].is<float>()) {
                transcribe_log("Audio duration: %.1f s", doc["audio_duration_seconds"].as<float>());
            }
            return true;
        }
        if (status == "failed") {
            const char *jobError = doc["error"].as<const char *>(); // nullptr if absent/null
            if (jobError) {
                set_err(errOut, errOutLen, jobError);
            } else {
                snprintf(errOut, errOutLen, tr(Str::ERR_UNEXPECTED_RESPONSE), ai_provider_name());
            }
            transcribe_log("Job failed: %s", errOut);
            return false;
        }
    }
}

bool ai_transcribe_file(const char *filename, char *errOut, size_t errOutLen) {
    if (WiFi.status() != WL_CONNECTED) {
        set_err(errOut, errOutLen, tr(Str::ERR_WIFI_NOT_CONNECTED));
        transcribe_log("Error: %s", errOut);
        return false;
    }

    char apiKey[AI_API_KEY_MAX];
    ai_provider_get_api_key(apiKey, sizeof(apiKey));
    if (apiKey[0] == '\0') {
        snprintf(errOut, errOutLen, tr(Str::ERR_NO_API_KEY), ai_provider_name());
        transcribe_log("Error: %s", errOut);
        return false;
    }

    if (!sd_begin()) {
        set_err(errOut, errOutLen, tr(Str::ERR_SD_UNAVAILABLE));
        transcribe_log("Error: %s", errOut);
        return false;
    }

    char srcPath[80];
    snprintf(srcPath, sizeof(srcPath), "/%s", filename);
    File src = sd_fs().open(srcPath, FILE_READ);
    if (!src) {
        sd_end();
        set_err(errOut, errOutLen, tr(Str::ERR_OPEN_FILE));
        transcribe_log("Error: %s (%s)", errOut, srcPath);
        return false;
    }
    transcribe_log("Server: %s, file size: %u bytes", API_BASE, (unsigned)src.size());
    // A failed or wrong DNS answer otherwise only shows up as the upload's
    // "HTTP -1 connection refused, TLS: Generic error" after a long TCP
    // connect timeout - log what the device's resolver actually returned.
    IPAddress serverIp;
    if (WiFi.hostByName(API_HOST, serverIp)) {
        transcribe_log("DNS: %s -> %s (DNS server %s)", API_HOST, serverIp.toString().c_str(),
                       WiFi.dnsIP().toString().c_str());
    } else {
        transcribe_log("DNS: lookup of %s failed (DNS server %s)", API_HOST, WiFi.dnsIP().toString().c_str());
    }

    String jobId;
    bool created = create_job(filename, src, apiKey, jobId, errOut, errOutLen);
    src.close();
    if (!created) {
        sd_end();
        return false;
    }

    String transcript;
    bool done = wait_for_job(apiKey, jobId, transcript, errOut, errOutLen);
    // Best effort: the server deletes jobs after its retention time anyway,
    // this just doesn't leave the audio there any longer than needed. A
    // job still processing (timeout) answers 409 and is left to retention.
    String ignored;
    int delCode = job_request("DELETE", apiKey, jobId, ignored);
    transcribe_log("Delete job: HTTP %d", delCode);
    if (!done) {
        sd_end();
        return false;
    }

    transcribe_report_phase(TranscribePhase::kSaving);
    char dstPath[80];
    txt_sibling_path(filename, dstPath, sizeof(dstPath));
    File dst = sd_fs().open(dstPath, FILE_WRITE);
    if (!dst) {
        sd_end();
        set_err(errOut, errOutLen, tr(Str::ERR_WRITE_TRANSCRIPT));
        transcribe_log("Error: %s (%s)", errOut, dstPath);
        return false;
    }
    dst.print(transcript);
    dst.close();
    sd_end();
    return true;
}

#endif // AI_PROVIDER_ANNOTA

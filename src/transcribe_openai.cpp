// OpenAI transcription provider - only compiled in when platformio.ini's
// build_flags define AI_PROVIDER_OPENAI (see transcribe.h's top comment
// for the provider-selection scheme). Compiles to an empty translation
// unit otherwise, so it's safe for this file to always be in src/
// regardless of which provider is actually selected.
#ifdef AI_PROVIDER_OPENAI

#include "transcribe.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "i18n.h"
#include "openai_ca.h"
#include "storage.h"
#include "transcribe_upload.h"

// -----------------------------------------------------------------------
// OpenAI Whisper transcription (POST /v1/audio/transcriptions).
// -----------------------------------------------------------------------

static const char *TRANSCRIBE_URL = "https://api.openai.com/v1/audio/transcriptions";
// Arbitrary, just has to not appear inside the request body it wraps.
static const char *BOUNDARY = "----AnnotaBoundary7MA4YWxkTrZu0gW";
// Oldest, cheapest, most broadly available OpenAI transcription model -
// good default until there's a Settings UI for picking a different one.
static const char *MODEL = "whisper-1";

// Second request, after a successful transcription: a chat completion that
// turns the transcript into a title and a short abstract, written on top
// of the .txt file. Cheap model with JSON mode - keep in sync with
// web_server.cpp's browser-side summarize().
static const char *SUMMARY_URL = "https://api.openai.com/v1/chat/completions";
static const char *SUMMARY_MODEL = "gpt-4o-mini";
static const char *SUMMARY_PROMPT =
    "Given a transcript, reply with a JSON object {\"title\": string, \"abstract\": string}. "
    "Title: short, at most 10 words. Abstract: 2 to 4 sentences summarizing the content. "
    "Write both in the same language as the transcript.";

// NVS key namespaced by provider (not just "apiKey") so switching which
// AI_PROVIDER_* is compiled in doesn't silently feed a stale key saved
// for a different provider into this one, or vice versa.
static const char *NVS_KEY = "openaiKey";

static Preferences prefs;

const char *ai_provider_name() { return "OpenAI"; }

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

// Asks SUMMARY_MODEL for a title and abstract of `transcript`. Any failure
// is only logged - the caller falls back to saving the plain transcript,
// since the transcription itself (the slow, expensive part) already
// succeeded.
static bool summarize_transcript(const char *apiKey, const String &transcript, String &title, String &abstract) {
    String body;
    {
        JsonDocument req;
        req["model"] = SUMMARY_MODEL;
        req["response_format"]["type"] = "json_object";
        JsonArray messages = req["messages"].to<JsonArray>();
        JsonObject sys = messages.add<JsonObject>();
        sys["role"] = "system";
        sys["content"] = SUMMARY_PROMPT;
        JsonObject user = messages.add<JsonObject>();
        user["role"] = "user";
        user["content"] = transcript;
        serializeJson(req, body);
    }

    static const int kMaxAttempts = 2;
    int code = 0;
    String response;
    for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
        if (attempt > 0) {
            if (WiFi.status() != WL_CONNECTED) break;
            delay(1000);
        }
        WiFiClientSecure client;
        client.setCACert(OPENAI_ROOT_CA); // same as the transcription request
        HTTPClient http;
        // Both timeouts for the same reason as ai_transcribe_file()'s
        // setConnectTimeout() comment.
        http.setTimeout(60000);
        http.setConnectTimeout(60000);
        if (!http.begin(client, SUMMARY_URL)) {
            code = HTTPC_ERROR_CONNECTION_REFUSED;
            transcribe_log("Summary attempt %d/%d: http.begin() failed", attempt + 1, kMaxAttempts);
            continue;
        }
        http.addHeader("Authorization", String("Bearer ") + apiKey);
        http.addHeader("Content-Type", "application/json");
        unsigned long startMs = millis();
        code = http.POST(body);
        response = http.getString();
        String tlsError = code < 0 ? tls_error_suffix(client) : String();
        http.end();
        transcribe_log("Summary attempt %d/%d: HTTP %d%s%s%s, %lu ms", attempt + 1, kMaxAttempts, code,
                       code < 0 ? " " : "", code < 0 ? HTTPClient::errorToString(code).c_str() : "",
                       tlsError.c_str(), millis() - startMs);
        if (code >= 0) break;
    }
    body = String(); // free before parsing the response

    if (code != 200) {
        if (response.length() > 0) {
            transcribe_log("Summary response body (first 1 KB):");
            transcribe_log("%.1024s", response.c_str());
        }
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok ||
        !doc["choices"][0]["message"]["content"].is<const char *>()) {
        transcribe_log("Summary: unexpected response. Body (first 1 KB):");
        transcribe_log("%.1024s", response.c_str());
        return false;
    }
    // The model's answer is itself a JSON string (response_format json_object).
    JsonDocument content;
    if (deserializeJson(content, doc["choices"][0]["message"]["content"].as<const char *>()) !=
            DeserializationError::Ok ||
        !content["title"].is<const char *>() || !content["abstract"].is<const char *>()) {
        transcribe_log("Summary: model reply is not the expected JSON");
        return false;
    }
    title = content["title"].as<const char *>();
    abstract = content["abstract"].as<const char *>();
    title.trim();
    abstract.trim();
    return title.length() > 0 && abstract.length() > 0;
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
    transcribe_log("Model: %s, file size: %u bytes", MODEL, (unsigned)src.size());

    String preamble;
    preamble += "--";
    preamble += BOUNDARY;
    preamble += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    preamble += MODEL;
    preamble += "\r\n--";
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

    // Streaming a whole audio file over one TLS write is prone to a
    // transient HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3). Traced into
    // arduino-esp32's own HTTPClient::sendRequest(Stream*, size)
    // (HTTPClient.cpp): it writes the stream in HTTP_TCP_TX_BUFFER_SIZE
    // (1460-byte) chunks, and any one chunk that comes back short from
    // _client->write() gets exactly one 1ms-delay retry - a second short
    // write on that same chunk fails the *entire* upload right there,
    // with no reconnect. A 4.8MB file is ~3300 of those chunks in a
    // single attempt, versus a few hundred for a short clip - the same
    // per-chunk risk, spread over far more chunks, made the once-good-
    // enough single retry inadequate once recordings got this long.
    // Retrying the whole file from the top, with backoff, is what
    // actually recovers from this - a flaky AP or one bad chunk among
    // thousands is usually gone by the next attempt.
    static const int kMaxAttempts = 6;
    static const int kBackoffMs[kMaxAttempts] = {0, 300, 900, 2000, 4000, 8000};
    int code = 0;
    String response;
    // WiFi modem sleep (arduino-esp32's default) parks the radio between AP
    // beacons, so ACKs for the upload trickle in once per beacon interval -
    // with lwIP's small send buffer (CONFIG_LWIP_TCP_SND_BUF_DEFAULT, ~5.7KB)
    // that capped a multi-MB upload at ~15 KB/s, long enough for the
    // connection to drop partway through. Keep the radio awake for the
    // upload only, then restore whatever was set before.
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
        // Verify api.openai.com's cert against the root CAs in openai_ca.h,
        // so a MITM can't read the API key out of the Authorization header.
        // A verification failure shows up in the attempt log through
        // tls_error_suffix().
        client.setCACert(OPENAI_ROOT_CA);

        HTTPClient http;
        http.setTimeout(60000);
        // HTTPClient::connect() passes its OWN separate _connectTimeout
        // (5000ms default, HTTPCLIENT_DEFAULT_TCP_TIMEOUT) to the client's
        // connect(host, port, timeout) - which WiFiClientSecure latches in
        // as the mbedTLS socket timeout for the rest of this connection's
        // life (ssl_client.cpp's send_ssl_data() gives up on a write stalled
        // longer than that). setTimeout() above never touches it. Left at
        // its 5s default, any single >5s stall on the socket during the
        // upload (peer backpressure, weak RSSI) kills the write with
        // HTTPC_ERROR_SEND_PAYLOAD_FAILED. Match it to the same 60s budget.
        http.setConnectTimeout(60000);
        if (!http.begin(client, TRANSCRIBE_URL)) {
            code = HTTPC_ERROR_CONNECTION_REFUSED;
            transcribe_log("Attempt %d/%d: http.begin() failed", attempt + 1, kMaxAttempts);
            continue;
        }
        http.addHeader("Authorization", String("Bearer ") + apiKey);
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
    src.close();

    if (code != 200) {
        if (response.length() > 0) {
            transcribe_log("Response body (first 1 KB):");
            transcribe_log("%.1024s", response.c_str());
        }
        JsonDocument doc;
        String message;
        if (deserializeJson(doc, response) == DeserializationError::Ok && doc["error"]["message"].is<const char *>()) {
            message = doc["error"]["message"].as<const char *>();
        } else if (code == HTTPC_ERROR_SEND_PAYLOAD_FAILED) {
            message = tr(Str::ERR_UPLOAD_INTERRUPTED);
        } else if (code < 0) {
            // Other negative codes are HTTPClient's own connection-layer
            // errors (never reached the server, so no JSON body to parse
            // to blame instead) - the request never got past
            // client.connect() inside sendRequest(). errorToString() names
            // which stage failed (DNS, socket connect, read timeout, ...).
            // The last attempt's TLS error, if any, is in the log (see
            // tls_error_suffix()).
            message = "HTTP " + String(code) + " (" + HTTPClient::errorToString(code) + ")";
        } else {
            message = "HTTP " + String(code);
        }
        sd_end();
        message.toCharArray(errOut, errOutLen);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok || !doc["text"].is<const char *>()) {
        sd_end();
        snprintf(errOut, errOutLen, tr(Str::ERR_UNEXPECTED_RESPONSE), ai_provider_name());
        transcribe_log("Error: %s. Body (first 1 KB):", errOut);
        transcribe_log("%.1024s", response.c_str());
        return false;
    }

    // Free the Whisper response before opening a second TLS session.
    String transcript = doc["text"].as<const char *>();
    doc.clear();
    response = String();

    transcribe_report_phase(TranscribePhase::kSummarizing);
    String title, abstract;
    bool summarized = summarize_transcript(apiKey, transcript, title, abstract);
    if (!summarized) transcribe_log("Summary skipped, saving plain transcript");

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
    if (summarized) {
        dst.print(title);
        dst.print("\n\n");
        dst.print(abstract);
        dst.print("\n\n");
    }
    dst.print(transcript);
    dst.close();
    sd_end();
    return true;
}

#endif // AI_PROVIDER_OPENAI

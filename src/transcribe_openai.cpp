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
// See scripts/patch_wolfssl.py's top comment: only one translation unit in
// this project may define wolfSSL_Arduino_Serial_Print() to avoid a
// multiple-definition link error, and ESP32-EasyWolfSSL's own
// WolfSSLClient.cpp (a downloaded lib_deps package this project can't
// patch) is it.
#define ANNOTA_WOLFSSL_SKIP_SERIAL_PRINT_DEFINITION
#include <WolfSSLClient.h> // wolfSSL-backed drop-in for WiFiClientSecure (see platformio.ini)

#include "storage.h"

// -----------------------------------------------------------------------
// OpenAI Whisper transcription (POST /v1/audio/transcriptions).
// -----------------------------------------------------------------------

static const char *TRANSCRIBE_URL = "https://api.openai.com/v1/audio/transcriptions";
// Arbitrary, just has to not appear inside the request body it wraps.
static const char *BOUNDARY = "----AnnotaBoundary7MA4YWxkTrZu0gW";
// Oldest, cheapest, most broadly available OpenAI transcription model -
// good default until there's a Settings UI for picking a different one.
static const char *MODEL = "whisper-1";

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

// Streams a multipart/form-data body - a literal preamble, then an SD
// file's bytes, then a literal trailer - to HTTPClient::sendRequest()
// without ever buffering the whole file in RAM: the ESP32 doesn't have
// enough of it for anything but the smallest clips.
class MultipartStream : public Stream {
   public:
    MultipartStream(const String &preamble, File &file, const String &trailer)
        : preamble_(preamble), file_(file), trailer_(trailer) {}

    int available() override {
        long remaining = (long)(preamble_.length() - preamblePos_) + (long)file_.available() +
                          (long)(trailer_.length() - trailerPos_);
        return remaining > 0 ? (int)remaining : 0;
    }

    int read() override {
        char b;
        return readBytes(&b, 1) == 1 ? (int)(uint8_t)b : -1;
    }

    // Not needed by HTTPClient::sendRequest()'s write loop.
    int peek() override { return -1; }

    size_t readBytes(char *buffer, size_t length) override {
        size_t total = 0;
        while (total < length && preamblePos_ < preamble_.length()) {
            buffer[total++] = preamble_[preamblePos_++];
        }
        while (total < length && file_.available()) {
            int n = file_.read((uint8_t *)buffer + total, length - total);
            if (n <= 0) break;
            total += n;
        }
        while (total < length && trailerPos_ < trailer_.length()) {
            buffer[total++] = trailer_[trailerPos_++];
        }
        return total;
    }

    void flush() override {}
    size_t write(uint8_t) override { return 0; } // request body is upload-only

   private:
    String preamble_;
    File &file_;
    String trailer_;
    size_t preamblePos_ = 0;
    size_t trailerPos_ = 0;
};

// Swaps `filename`'s extension for ".txt" and adds the leading '/'
// SD.open() needs (e.g. "song.mp3" -> "/song.txt").
static void txt_sibling_path(const char *filename, char *out, size_t outLen) {
    const char *dot = strrchr(filename, '.');
    size_t baseLen = dot ? (size_t)(dot - filename) : strlen(filename);
    if (baseLen > outLen - 6) baseLen = outLen - 6; // '/' + up to baseLen + ".txt" + '\0'
    out[0] = '/';
    memcpy(out + 1, filename, baseLen);
    strcpy(out + 1 + baseLen, ".txt");
}

static void set_err(char *errOut, size_t errOutLen, const char *msg) {
    strncpy(errOut, msg, errOutLen - 1);
    errOut[errOutLen - 1] = '\0';
}

bool ai_transcribe_file(const char *filename, char *errOut, size_t errOutLen) {
    if (WiFi.status() != WL_CONNECTED) {
        set_err(errOut, errOutLen, "WiFi not connected");
        return false;
    }

    char apiKey[AI_API_KEY_MAX];
    ai_provider_get_api_key(apiKey, sizeof(apiKey));
    if (apiKey[0] == '\0') {
        set_err(errOut, errOutLen, "No OpenAI API key set (see Settings)");
        return false;
    }

    if (!sd_begin()) {
        set_err(errOut, errOutLen, "SD card not available");
        return false;
    }

    char srcPath[80];
    snprintf(srcPath, sizeof(srcPath), "/%s", filename);
    File src = sd_fs().open(srcPath, FILE_READ);
    if (!src) {
        sd_end();
        set_err(errOut, errOutLen, "Could not open file");
        return false;
    }

    String preamble;
    preamble += "--";
    preamble += BOUNDARY;
    preamble += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    preamble += MODEL;
    preamble += "\r\n--";
    preamble += BOUNDARY;
    preamble += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    preamble += filename;
    preamble += "\"\r\nContent-Type: audio/mpeg\r\n\r\n";

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
    for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
        if (attempt > 0) {
            if (WiFi.status() != WL_CONNECTED) {
                code = HTTPC_ERROR_CONNECTION_LOST;
                break;
            }
            src.seek(0);
            delay(kBackoffMs[attempt]);
        }

        WiFiClientSecure client;
        // No certificate pinning / root-CA bundle exists in this project
        // yet - accept whatever cert the server presents. Traffic is
        // still TLS-encrypted in transit; this just means no protection
        // against a MITM presenting a fake cert.
        client.setInsecure();

        HTTPClient http;
        http.setTimeout(60000);
        // HTTPClient::connect() passes its OWN separate _connectTimeout
        // (5000ms default, HTTPCLIENT_DEFAULT_TCP_TIMEOUT) to the client's
        // connect(host, port, timeout) - which WolfSSLClient::connect()
        // (WolfSSLClient.cpp) latches into both its underlying WiFiClient's
        // own setTimeout() and _timeout_ms, the no-progress budget its recv
        // callback uses for the rest of this connection's life. setTimeout()
        // above never touches it. Left at its 5s default, any single >5s
        // stall on the socket during the upload (peer backpressure, weak
        // RSSI) kills the write with HTTPC_ERROR_SEND_PAYLOAD_FAILED -
        // reliably, not just under flaky conditions. Match it to the same
        // 60s budget.
        http.setConnectTimeout(60000);
        if (!http.begin(client, TRANSCRIBE_URL)) {
            code = HTTPC_ERROR_CONNECTION_REFUSED;
            continue;
        }
        http.addHeader("Authorization", String("Bearer ") + apiKey);
        http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);

        MultipartStream body(preamble, src, trailer);
        Serial.printf("ai_transcribe_file: connecting (attempt %d/%d), free heap %u bytes, RSSI %d dBm\n", attempt + 1,
                       kMaxAttempts, (unsigned)ESP.getFreeHeap(), (int)WiFi.RSSI());
        code = http.sendRequest("POST", &body, contentLength);
        response = http.getString();
        http.end();

        if (code != HTTPC_ERROR_SEND_PAYLOAD_FAILED) break;
        Serial.printf("ai_transcribe_file: send payload failed (attempt %d/%d), retrying\n", attempt + 1, kMaxAttempts);
    }
    src.close();

    if (code != 200) {
        JsonDocument doc;
        String message;
        if (deserializeJson(doc, response) == DeserializationError::Ok && doc["error"]["message"].is<const char *>()) {
            message = doc["error"]["message"].as<const char *>();
        } else if (code == HTTPC_ERROR_SEND_PAYLOAD_FAILED) {
            message = "Upload interrupted, connection dropped (HTTP -3 send payload failed)";
        } else if (code < 0) {
            // Other negative codes are HTTPClient's own connection-layer
            // errors (never reached the server, so no JSON body to parse
            // to blame instead) - the request never got past
            // client.connect() inside sendRequest(). errorToString() names
            // which stage failed (DNS, socket connect, read timeout, ...).
            // Unlike mbedTLS's WiFiClientSecure, WolfSSLClient exposes no
            // lastError()-equivalent to add a TLS-specific reason on top
            // when the failed stage was the handshake itself -
            // WolfSSLClient::setDebug(true) would surface wolfSSL's own
            // error string to Serial instead, but only that, not here.
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
        set_err(errOut, errOutLen, "Unexpected response from OpenAI");
        return false;
    }

    char dstPath[80];
    txt_sibling_path(filename, dstPath, sizeof(dstPath));
    File dst = sd_fs().open(dstPath, FILE_WRITE);
    if (!dst) {
        sd_end();
        set_err(errOut, errOutLen, "Could not write transcript file");
        return false;
    }
    dst.print(doc["text"].as<const char *>());
    dst.close();
    sd_end();
    return true;
}

#endif // AI_PROVIDER_OPENAI

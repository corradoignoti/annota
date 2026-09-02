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
    // transient HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3): arduino-esp32's
    // NetworkClientSecure::write() (send_ssl_data() in ssl_client.cpp)
    // never updates sslclient's last_error on a write failure or
    // send-timeout - only NetworkClientSecure::connect() does - so
    // client.lastError() after a -3 reports the (successful) TLS
    // handshake result, not the write failure, and is not worth
    // surfacing here. Retries from the top of the file, with backoff,
    // clear most one-off drops (a stalled SD read starving the socket
    // long enough for the peer to give up, a flaky AP, ...); a single
    // retry proved not enough in practice, so this allows a few more
    // with increasing pauses between them before giving up as
    // non-transient.
    static const int kMaxAttempts = 4;
    static const int kBackoffMs[kMaxAttempts] = {0, 300, 900, 2000};
    int code = 0;
    String response;
    char tlsErr[100] = "";
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
        // (5000ms default, HTTPCLIENT_DEFAULT_TCP_TIMEOUT) to the
        // client's connect(host, port, timeout) - which is what
        // NetworkClientSecure/ssl_client.cpp latches into
        // sslclient->socket_timeout, the no-progress watchdog
        // send_ssl_data() uses for every write for the rest of this
        // connection's life. setTimeout() above never touches it. Left
        // at its 5s default, any single >5s stall on the socket during
        // the upload (peer backpressure, weak RSSI) kills the write with
        // HTTPC_ERROR_SEND_PAYLOAD_FAILED - reliably, not just under
        // flaky conditions. Match it to the same 60s budget.
        http.setConnectTimeout(60000);
        if (!http.begin(client, TRANSCRIBE_URL)) {
            code = HTTPC_ERROR_CONNECTION_REFUSED;
            continue;
        }
        http.addHeader("Authorization", String("Bearer ") + apiKey);
        http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);

        MultipartStream body(preamble, src, trailer);
        Serial.printf("ai_transcribe_file: connecting (attempt %d/%d), free heap %u bytes\n", attempt + 1,
                       kMaxAttempts, (unsigned)ESP.getFreeHeap());
        code = http.sendRequest("POST", &body, contentLength);
        response = http.getString();
        // Grab this before client goes out of scope below - meaningful
        // for a genuine handshake failure (code < 0, not -3; see the
        // comment on the send-payload-failed branch below for why -3
        // doesn't get anything useful out of it).
        client.lastError(tlsErr, sizeof(tlsErr));
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
            // Connection dropped mid-upload (see the retry loop's comment
            // above for why client.lastError() isn't worth printing here -
            // it never reflects a write failure, only the earlier,
            // successful handshake).
            message = "Upload interrupted, connection dropped (HTTP -3 send payload failed)";
        } else if (code < 0) {
            // Other negative codes are HTTPClient's own connection-layer
            // errors (never reached the server, so no JSON body to parse
            // to blame instead) - the request never got past
            // client.connect() inside sendRequest(). errorToString()
            // names which stage failed (DNS, socket connect, read
            // timeout, ...); when it's the TLS handshake itself,
            // mbedtls_strerror() (via client.lastError()) gives the
            // actual mbedTLS reason - often a heap allocation failure if
            // the free heap logged just above is only tens of KB (this
            // board has PSRAM, but mbedTLS's own buffers still have to
            // compete with whatever else hasn't been pushed off internal
            // RAM).
            message = "HTTP " + String(code) + " (" + HTTPClient::errorToString(code) + "; TLS: " + tlsErr + ")";
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

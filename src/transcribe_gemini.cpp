// Google Gemini transcription provider - only compiled in when
// platformio.ini's build_flags define AI_PROVIDER_GEMINI (see
// transcribe.h's top comment for the provider-selection scheme). Compiles
// to an empty translation unit otherwise, so it's safe for this file to
// always be in src/ regardless of which provider is actually selected.
#ifdef AI_PROVIDER_GEMINI

#include "transcribe.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "storage.h"

// -----------------------------------------------------------------------
// Gemini transcription via generateContent, with the audio file sent
// inline as base64 (no separate upload/files API - keeps this the same
// one-request shape as transcribe_openai.cpp).
// -----------------------------------------------------------------------

// Cheapest/fastest generally-available Gemini model with audio input
// support - good default until there's a Settings UI for picking a
// different one.
static const char *MODEL = "gemini-3.7-flash";
static const char *API_HOST = "https://generativelanguage.googleapis.com/v1beta/models/";
static const char *PROMPT =
    "Transcribe this audio recording verbatim. Respond with only the transcript text, no commentary.";

// NVS key namespaced by provider (not just "apiKey") so switching which
// AI_PROVIDER_* is compiled in doesn't silently feed a stale key saved
// for a different provider into this one, or vice versa.
static const char *NVS_KEY = "geminiKey";

static Preferences prefs;

const char *ai_provider_name() { return "Gemini"; }

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

static const char BASE64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Exact base64-encoded length of `rawLen` bytes, padding included.
static size_t base64_encoded_length(size_t rawLen) { return ((rawLen + 2) / 3) * 4; }

// Encodes 1-3 raw bytes into exactly 4 base64 chars (with '=' padding if
// n < 3), the usual base64 group transform.
static void base64_encode_group(const uint8_t *raw, int n, char *out) {
    uint32_t v = ((uint32_t)raw[0]) << 16;
    if (n > 1) v |= ((uint32_t)raw[1]) << 8;
    if (n > 2) v |= raw[2];
    out[0] = BASE64_CHARS[(v >> 18) & 0x3F];
    out[1] = BASE64_CHARS[(v >> 12) & 0x3F];
    out[2] = n > 1 ? BASE64_CHARS[(v >> 6) & 0x3F] : '=';
    out[3] = n > 2 ? BASE64_CHARS[v & 0x3F] : '=';
}

// Streams a JSON body - a literal preamble, then an SD file's bytes
// base64-encoded on the fly, then a literal trailer - to
// HTTPClient::sendRequest() without ever buffering the whole file (raw or
// encoded) in RAM: the ESP32 doesn't have enough of it for anything but
// the smallest clips. Mirrors transcribe_openai.cpp's MultipartStream,
// just with a base64 encode step folded into the file-reading part.
class Base64JsonStream : public Stream {
   public:
    Base64JsonStream(const String &preamble, File &file, const String &trailer)
        : preamble_(preamble), file_(file), trailer_(trailer), encodedLen_(base64_encoded_length(file.size())) {}

    int available() override {
        long remaining = (long)(preamble_.length() - preamblePos_) + (long)(encodedLen_ - encodedPos_) +
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
        while (total < length && encodedPos_ < encodedLen_) {
            if (groupPos_ >= groupLen_) {
                uint8_t raw[3];
                int n = file_.read(raw, 3);
                if (n <= 0) break; // shouldn't happen before encodedPos_ reaches encodedLen_
                base64_encode_group(raw, n, group_);
                groupPos_ = 0;
                groupLen_ = 4;
            }
            while (total < length && groupPos_ < groupLen_) {
                buffer[total++] = group_[groupPos_++];
                encodedPos_++;
            }
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
    size_t encodedLen_;
    size_t encodedPos_ = 0;
    char group_[4];
    int groupPos_ = 0;
    int groupLen_ = 0;
};

// Gemini validates inline_data's mime_type against the actual audio
// format, so - unlike transcribe_openai.cpp, which always sends
// "audio/mpeg" regardless of extension - pick it from the filename.
static const char *mime_type_for(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (dot && strcasecmp(dot, ".m4a") == 0) return "audio/mp4";
    return "audio/mpeg";
}

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
        set_err(errOut, errOutLen, "No Gemini API key set (see Settings)");
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
    preamble += "{\"contents\":[{\"parts\":[{\"text\":\"";
    preamble += PROMPT;
    preamble += "\"},{\"inline_data\":{\"mime_type\":\"";
    preamble += mime_type_for(filename);
    preamble += "\",\"data\":\"";

    String trailer = "\"}}]}]}";

    Base64JsonStream body(preamble, src, trailer);
    size_t contentLength = preamble.length() + base64_encoded_length(src.size()) + trailer.length();

    WiFiClientSecure client;
    // No certificate pinning / root-CA bundle exists in this project yet -
    // accept whatever cert the server presents. Traffic is still
    // TLS-encrypted in transit; this just means no protection against a
    // MITM presenting a fake cert.
    client.setInsecure();

    String url = String(API_HOST) + MODEL + ":generateContent?key=" + apiKey;

    HTTPClient http;
    http.setTimeout(60000);
    if (!http.begin(client, url)) {
        src.close();
        sd_end();
        set_err(errOut, errOutLen, "Could not reach generativelanguage.googleapis.com");
        return false;
    }
    http.addHeader("Content-Type", "application/json");

    Serial.printf("ai_transcribe_file: connecting, free heap %u bytes\n", (unsigned)ESP.getFreeHeap());
    int code = http.sendRequest("POST", &body, contentLength);
    String response = http.getString();
    http.end();
    src.close();

    if (code != 200) {
        JsonDocument doc;
        String message;
        if (deserializeJson(doc, response) == DeserializationError::Ok && doc["error"]["message"].is<const char *>()) {
            message = doc["error"]["message"].as<const char *>();
        } else if (code < 0) {
            // Negative codes are HTTPClient's own connection-layer errors -
            // see transcribe_openai.cpp's identical branch for what each
            // piece means and why a heap-starved TLS handshake is a
            // plausible culprit.
            char tlsErr[100];
            client.lastError(tlsErr, sizeof(tlsErr));
            message = "HTTP " + String(code) + " (" + HTTPClient::errorToString(code) + "; TLS: " + tlsErr + ")";
        } else {
            message = "HTTP " + String(code);
        }
        sd_end();
        message.toCharArray(errOut, errOutLen);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok ||
        !doc["candidates"][0]["content"]["parts"][0]["text"].is<const char *>()) {
        sd_end();
        set_err(errOut, errOutLen, "Unexpected response from Gemini");
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
    dst.print(doc["candidates"][0]["content"]["parts"][0]["text"].as<const char *>());
    dst.close();
    sd_end();
    return true;
}

#endif // AI_PROVIDER_GEMINI

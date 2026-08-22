#include "transcribe.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "display.h"
#include "storage.h"
#include "ui.h"

// -----------------------------------------------------------------------
// OpenAI Whisper transcription. See transcribe.h for the
// request/process_pending split (mirrors wifi_manager.cpp's reconnect
// pair) and its reentrant-lv_timer_handler() rationale.
// -----------------------------------------------------------------------

static const char *TRANSCRIBE_URL = "https://api.openai.com/v1/audio/transcriptions";
// Arbitrary, just has to not appear inside the request body it wraps.
static const char *BOUNDARY = "----AnnotaBoundary7MA4YWxkTrZu0gW";
// Oldest, cheapest, most broadly available OpenAI transcription model -
// good default until there's a Settings UI for picking a different one.
static const char *MODEL = "whisper-1";

static Preferences prefs;

bool openai_has_api_key() {
    prefs.begin("annota", true);
    String key = prefs.getString("oaiKey", "");
    prefs.end();
    return key.length() > 0;
}

void openai_get_api_key(char *out, size_t outLen) {
    prefs.begin("annota", true);
    String key = prefs.getString("oaiKey", "");
    prefs.end();
    key.toCharArray(out, outLen);
}

void openai_set_api_key(const char *key) {
    prefs.begin("annota", false);
    prefs.putString("oaiKey", key);
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

bool transcribe_file(const char *filename, char *errOut, size_t errOutLen) {
    if (WiFi.status() != WL_CONNECTED) {
        set_err(errOut, errOutLen, "WiFi not connected");
        return false;
    }

    char apiKey[OPENAI_API_KEY_MAX];
    openai_get_api_key(apiKey, sizeof(apiKey));
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
    File src = SD.open(srcPath, FILE_READ);
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

    WiFiClientSecure client;
    // No certificate pinning / root-CA bundle exists in this project yet -
    // accept whatever cert the server presents. Traffic is still
    // TLS-encrypted in transit; this just means no protection against a
    // MITM presenting a fake cert.
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(60000);
    if (!http.begin(client, TRANSCRIBE_URL)) {
        src.close();
        sd_end();
        set_err(errOut, errOutLen, "Could not reach api.openai.com");
        return false;
    }
    http.addHeader("Authorization", String("Bearer ") + apiKey);
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);

    MultipartStream body(preamble, src, trailer);
    int code = http.sendRequest("POST", &body, contentLength);
    String response = http.getString();
    http.end();
    src.close();

    if (code != 200) {
        JsonDocument doc;
        String message;
        if (deserializeJson(doc, response) == DeserializationError::Ok && doc["error"]["message"].is<const char *>()) {
            message = doc["error"]["message"].as<const char *>();
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
    File dst = SD.open(dstPath, FILE_WRITE);
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

    display_suspend_touch();
    char err[96];
    bool ok = transcribe_file(transcribeTargetFilename, err, sizeof(err));
    display_resume_touch();

    ui_show_transcribe_result(ok, ok ? "Transcription saved." : err);
}

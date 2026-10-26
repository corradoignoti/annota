#pragma once

// Upload helpers shared by the providers that stream a multipart/form-data
// request body off the SD card (transcribe_openai.cpp, transcribe_annota.cpp).
// Provider-agnostic, so not wrapped in any AI_PROVIDER_* #ifdef - whatever
// the compiled-in provider doesn't use is dropped at link time.

#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "transcribe.h"

// Streams a multipart/form-data body - a literal preamble, then an SD
// file's bytes, then a literal trailer - to HTTPClient::sendRequest()
// without ever buffering the whole file in RAM: the ESP32 doesn't have
// enough of it for anything but the smallest clips. Also reports upload
// progress through transcribe.h's hooks as HTTPClient pulls bytes out of
// it, switching to the "waiting" phase once the last byte has gone.
class MultipartStream : public Stream {
   public:
    MultipartStream(const String &preamble, File &file, const String &trailer, int attempt, int maxAttempts)
        : preamble_(preamble),
          file_(file),
          trailer_(trailer),
          total_(preamble.length() + file.size() + trailer.length()),
          attempt_(attempt),
          maxAttempts_(maxAttempts) {}

    size_t served() const { return served_; }

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
        served_ += total;
        if (total > 0) transcribe_report_upload(served_, total_, attempt_, maxAttempts_);
        if (served_ >= total_ && !doneReported_) {
            doneReported_ = true;
            transcribe_report_phase(TranscribePhase::kWaiting);
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
    size_t total_;
    size_t served_ = 0;
    int attempt_;
    int maxAttempts_;
    bool doneReported_ = false;
};

// WiFiClientSecure tuned for the multi-MB upload, fixing two ways
// HTTPClient::sendRequest(Stream*, size) throttles or kills it:
//
// - Record size. HTTPClient writes the body in HTTP_TCP_TX_BUFFER_SIZE
//   (1460-byte) chunks, and each write() becomes its own TLS record: 1460 +
//   TLS overhead, just over lwIP's 1436-byte MSS, so every record went out
//   as one full segment plus a tiny runt, several thousand times per file.
//   api.openai.com resets a request whose body is still arriving after
//   ~100-120 s, so upload throughput decides how long a recording can be
//   transcribed at all. Writes are coalesced here into kRecordBytes (TLS's
//   max record size) before being handed to the TLS layer.
// - Stalls. HTTPClient gives a short write exactly one retry, 1ms later,
//   before returning HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3). write_all()
//   keeps retrying a short or zero write until the socket closes or
//   nothing has moved for kStallMs.
//
// A write() shorter than HTTP_TCP_TX_BUFFER_SIZE (the headers, or the
// body's last chunk) is sent right away, so a failure there still surfaces
// as -3 from sendRequest() and gets the upload loop's retry. Anything still
// buffered is sent before the first read of the response.
class UploadClient : public WiFiClientSecure {
   public:
    ~UploadClient() { free(buf_); }

    size_t write(const uint8_t *data, size_t size) override {
        if (failed_) return 0;
        if (!buf_) buf_ = (uint8_t *)malloc(kRecordBytes);
        if (!buf_) return write_all(data, size);
        size_t done = 0;
        while (done < size) {
            size_t n = min(size - done, kRecordBytes - len_);
            memcpy(buf_ + len_, data + done, n);
            len_ += n;
            done += n;
            if (len_ == kRecordBytes && !send_pending()) return 0;
        }
        if (size < HTTP_TCP_TX_BUFFER_SIZE && !send_pending()) return 0;
        return size;
    }
    size_t write(uint8_t data) override { return write(&data, 1); }

    int available() override {
        send_pending();
        return WiFiClientSecure::available();
    }
    int read() override {
        send_pending();
        return WiFiClientSecure::read();
    }
    int read(uint8_t *data, size_t size) override {
        send_pending();
        return WiFiClientSecure::read(data, size);
    }
    void stop() override {
        len_ = 0;
        WiFiClientSecure::stop();
    }

   private:
    static const size_t kRecordBytes = 16384;
    static const unsigned long kStallMs = 60000;

    bool send_pending() {
        if (len_ == 0 || failed_) return !failed_;
        size_t sent = write_all(buf_, len_);
        failed_ = sent != len_;
        len_ = 0;
        return !failed_;
    }

    size_t write_all(const uint8_t *data, size_t size) {
        size_t done = 0;
        unsigned long lastProgressMs = millis();
        while (done < size) {
            size_t n = WiFiClientSecure::write(data + done, size - done);
            if (n > 0) {
                done += n;
                lastProgressMs = millis();
                continue;
            }
            if (!connected() || millis() - lastProgressMs > kStallMs) break;
            delay(10);
        }
        return done;
    }

    uint8_t *buf_ = nullptr;
    size_t len_ = 0;
    bool failed_ = false;
};

// ", TLS: <mbedTLS reason>" for the attempt log when the connection failed
// inside the TLS layer (e.g. the handshake), empty otherwise.
String tls_error_suffix(WiFiClientSecure &client);

// Multipart part Content-Type for the uploaded file, from its extension.
const char *upload_mime_type_for(const char *filename);

// Swaps `filename`'s extension for ".txt" and adds the leading '/'
// SD.open() needs (e.g. "song.mp3" -> "/song.txt").
void txt_sibling_path(const char *filename, char *out, size_t outLen);

// Copies `msg` into errOut, truncated and always nul-terminated.
void set_err(char *errOut, size_t errOutLen, const char *msg);

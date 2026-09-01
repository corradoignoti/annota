#include "speaker.h"

#include <Arduino.h>
#include <cstring>

#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutput.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>

#include "es8311.h"
#include "storage.h"

// -----------------------------------------------------------------------
// Hardware, read straight off Waveshare's own schematic for this board
// (ESP32-S3-Touch-ePaper-1.54-Schematic.pdf, "Codec"/"PA&SPEAKER&MIC"
// blocks) - nothing about this pinout is documented anywhere else
// reachable (see the git history for how this was tracked down: even
// Waveshare's own audio example names a board type string,
// "S3_ePaper_1_54", that isn't in its own vendored pin-table component).
//
//   I2S_MCLK  -> GPIO14      I2S_LRCK (WS)   -> GPIO38
//   I2S_SCLK  -> GPIO15      I2S_DSDIN (out) -> GPIO45
//   I2S_ASDOUT (mic in)      -> GPIO16 (see mic_start_recording() below)
//   PA_EN     -> GPIO42 (codec+amp analog rail switch - matches Waveshare's
//                own user_config.h Audio_PWR_PIN)
//   PA_CTRL   -> GPIO46 (NS4150B amp shutdown/enable)
//   Codec I2C -> shared RTC/SHTC3 bus: SDA=GPIO47, SCL=GPIO48, addr 0x18
//
// This pinout was reverse-engineered from the schematic before Waveshare's
// own ESP-IDF audio example (waveshareteam/ESP32-S3-ePaper-1.54G,
// Example/ESP-IDF_5.5.1/07_Audio_Test/components/codec_board/board_cfg.txt,
// board "S3_ePaper_1_54") surfaced - it confirms every pin here except WS,
// which that file gives as GPIO38, not GPIO21. GPIO21 was the actual
// silent-output bug: with WS never toggling, the codec has no valid LRCK
// and never latches DAC samples no matter what else is configured
// correctly. That same example (audio_bsp.c, via esp_codec_dev) is also
// the source for the mic path below: it opens the codec's ADC and DAC
// together (esp_codec_dev_open() on both an `in`/`out` handle) over one
// shared full-duplex I2S pair, at 16kHz mono for the mic side - the same
// plan i2s_configure()/mic_start_recording() follow here, just against the
// raw i2s_std/es8311 driver instead of esp_codec_dev's C++-unfriendly API.
// -----------------------------------------------------------------------

namespace {

constexpr gpio_num_t I2S_MCLK_PIN = GPIO_NUM_14;
constexpr gpio_num_t I2S_BCLK_PIN = GPIO_NUM_15;
constexpr gpio_num_t I2S_WS_PIN = GPIO_NUM_38;
constexpr gpio_num_t I2S_DOUT_PIN = GPIO_NUM_45;
constexpr gpio_num_t I2S_DIN_PIN = GPIO_NUM_16; // mic ADC data in (I2S_ASDOUT)
constexpr int PA_EN_PIN = 42;
constexpr int PA_CTRL_PIN = 46;
constexpr int I2C_SDA_PIN = 47;
constexpr int I2C_SCL_PIN = 48;

constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
constexpr int DEFAULT_VOLUME = 85;

// Recording is voice-memo/transcription-oriented (not music), so mono at a
// speech-friendly rate, written straight to uncompressed 16-bit PCM WAV
// (write_wav_header()/patch_wav_header() below) - no encoder, no encoder
// working set to allocate, at the cost of a bigger file than a compressed
// format would give (fine for a short voice memo headed straight to
// transcribe.cpp's AI provider).
//
// MUST stay well under DEFAULT_SAMPLE_RATE, not just "a speech-typical
// rate" - this was briefly bumped to DEFAULT_SAMPLE_RATE (44100) to chase
// a playback-tail glitch (see below) and that broke recording outright,
// real voice replaced by noise throughout. Mono 16-bit at 44100 is 88,200
// bytes/sec that mic_process() has to get onto the SD card in real time,
// on top of every other loop() iteration's work (LVGL, web server, ...) -
// over 2.75x what 16kHz needs, and enough to overrun sd_begin()'s 1-bit
// SDMMC mode (storage.cpp, deliberately not 4-bit) and corrupt far more of
// the stream than the tail-glitch below ever did. 16kHz is comfortably
// sustainable and is one of es8311.h's fixed 256x-MCLK family members, so
// no codec clock-plan changes are needed switching between this and
// playback's 44.1kHz in principle - though on real hardware, playing back
// a 16kHz recording currently loops its last second or two several times
// over; not yet root-caused (a read-ahead buffer on the playback source,
// ruling out SD-read starvation, made no difference; bumping this constant
// to dodge a codec clock-relock theory is what caused the regression
// above, and got reverted). Fix that on the *playback* side only, without
// touching this rate. ES8311_MIC_GAIN_24DB (see es8311.h) is a reasonable
// starting gain for a board-mounted mic at conversational distance - not
// tuned against real hardware yet, may need adjusting once it is.
constexpr uint32_t MIC_SAMPLE_RATE = 16000;
constexpr int MIC_GAIN_CODE = 4; // ES8311_MIC_GAIN_24DB

// TX DMA ring depth - see i2s_configure()'s chanCfg for why (a little
// deeper than the esp-idf default), and prime_tx_silence() below for why
// this needs its own name instead of staying inline magic numbers there.
constexpr int TX_DMA_DESC_NUM = 8;
constexpr int TX_DMA_FRAME_NUM = 480;
constexpr int TX_DMA_RING_FRAMES = TX_DMA_DESC_NUM * TX_DMA_FRAME_NUM;

i2s_chan_handle_t txChan = nullptr;
i2s_chan_handle_t rxChan = nullptr;
uint32_t i2sConfiguredRate = 0;
bool rxEnabled = false; // mic_start_recording()/mic_stop_recording() only
bool hwReady = false;

// Sets up (or reconfigures) the I2S TX+RX channel pair for `rate`. Slot
// width is forced to 32 bits despite 16-bit data - the ES8311's fixed
// clock plan (see es8311.h) runs BCLK at 64x the sample rate (2 slots x
// 32 bits), not the 32x a plain 16-bit slot would give; the driver pads
// (TX) / strips (RX) the 16-bit samples we write/read against that wider
// slot on its own - same as the existing TX-only code already relied on,
// now true for rxChan's i2s_channel_read() too.
bool i2s_configure(uint32_t rate) {
    if (rate == i2sConfiguredRate) return true;

    i2s_std_slot_config_t slotCfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    slotCfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    i2s_std_clk_config_t clkCfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clkCfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (txChan == nullptr) {
        i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        // A little deeper than the default 6x240 frames, so a slow
        // web_server.cpp request or transcribe_process_pending() call
        // elsewhere in loop() doesn't starve playback into an audible
        // glitch before its next speaker_process() turn (and, now, so a
        // slow loop() iteration doesn't overflow the RX side into dropped
        // mic samples before its next mic_process() turn either).
        chanCfg.dma_desc_num = TX_DMA_DESC_NUM;
        chanCfg.dma_frame_num = TX_DMA_FRAME_NUM;
        // Request both directions from the same I2S controller in one
        // call - the only way to get them sharing one BCLK/WS master
        // clock plan off one physical bus, matching the schematic (this
        // file's top comment): DOUT feeds the codec's DAC, DIN reads its
        // ADC. rxChan is left disabled below - mic_start_recording() is
        // the only thing that ever enables it, so the DMA doesn't spend
        // however long the device is only ever playing (never recording)
        // silently filling up with unread mic samples.
        if (i2s_new_channel(&chanCfg, &txChan, &rxChan) != ESP_OK) return false;

        i2s_std_gpio_config_t gpioCfg = {};
        gpioCfg.mclk = I2S_MCLK_PIN;
        gpioCfg.bclk = I2S_BCLK_PIN;
        gpioCfg.ws = I2S_WS_PIN;
        gpioCfg.dout = I2S_DOUT_PIN;
        gpioCfg.din = I2S_DIN_PIN;

        i2s_std_config_t stdCfg = {};
        stdCfg.clk_cfg = clkCfg;
        stdCfg.slot_cfg = slotCfg;
        stdCfg.gpio_cfg = gpioCfg;
        if (i2s_channel_init_std_mode(txChan, &stdCfg) != ESP_OK) return false;
        if (i2s_channel_init_std_mode(rxChan, &stdCfg) != ESP_OK) return false;
        if (i2s_channel_enable(txChan) != ESP_OK) return false;
        // rxChan intentionally left disabled - see comment above.
    } else {
        if (i2s_channel_disable(txChan) != ESP_OK) return false;
        if (rxEnabled && i2s_channel_disable(rxChan) != ESP_OK) return false;
        if (i2s_channel_reconfig_std_clock(txChan, &clkCfg) != ESP_OK) return false;
        if (i2s_channel_reconfig_std_clock(rxChan, &clkCfg) != ESP_OK) return false;
        if (i2s_channel_enable(txChan) != ESP_OK) return false;
        if (rxEnabled && i2s_channel_enable(rxChan) != ESP_OK) return false;
    }

    i2sConfiguredRate = rate;
    return true;
}

// Buffers decoded samples and, once BUF_FRAMES have piled up, writes them
// to the I2S DMA - non-blockingly (try_flush(), timeout_ms=0). Once the
// DMA's own buffer (dma_desc_num*dma_frame_num frames - see
// i2s_configure()) is full, which is the normal steady state once
// playback catches up (we produce samples far faster than 44.1kHz real
// time whenever we're not blocked), that write returns having written
// nothing - ConsumeSample() reports the *current* sample as not consumed
// (false) instead of blocking here to wait for room. AudioGeneratorMP3/
// WAV's decode loop (both have the same
// `do { ... } while (running && output->ConsumeSample(...))`) treats a
// false return as "can't take more right now" and returns out of
// loop() - back to speaker_process(), and from there back to loop() - so
// something else (ui_epaper.cpp's button polling, notably a Stop press)
// gets a turn too. A previous version of this class always returned true
// from ConsumeSample() and paced itself with a blocking portMAX_DELAY
// write instead: since ESP8266Audio's decode loop only ever yields when
// ConsumeSample() says it must, that meant one gen->loop() call decoded
// and played the *entire* file before returning, and Stop had no chance
// to be noticed until the track ended on its own.
class Es8311Output : public AudioOutput {
public:
    bool SetRate(int hz) override {
        AudioOutput::SetRate(hz);
        return i2s_configure((uint32_t)hz);
    }

    bool begin() override {
        return hwReady;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        if (fill == BUF_FRAMES && !try_flush()) return false;
        MakeSampleStereo16(sample);
        buf[fill * 2] = Amplify(sample[LEFTCHANNEL]);
        buf[fill * 2 + 1] = Amplify(sample[RIGHTCHANNEL]);
        fill++;
        return true;
    }

    // Track end (natural EOF or speaker_stop()'s gen->stop()) - block
    // until every already-decoded sample actually reaches the DMA, so the
    // tail of the file doesn't get silently dropped. Fine to block here,
    // unlike ConsumeSample() above: this runs once per track, not once
    // per BUF_FRAMES samples.
    //
    // Mute right here, not just in cleanup() after gen->loop() returns -
    // AudioGeneratorWAV's own stop() (which calls this) goes on to close
    // its SD file *after* this returns, and that close (real card I/O) can
    // take real time - time this file's own now-real-data-exhausted DMA
    // ring spends re-clocking its last content on a still-unmuted DAC if
    // muting waits for cleanup(). Muting here, right after every real
    // sample has been handed to the DMA, closes that window; cleanup()'s
    // own mute stays as a harmless no-op/insurance for the manual-stop and
    // begin()-failed paths.
    void flush() override {
        drain_blocking();
        es8311_set_mute(true);
    }

    bool stop() override {
        drain_blocking();
        es8311_set_mute(true);
        return true;
    }

private:
    // Non-blocking: writes as much of buf as the DMA will accept right
    // now, shifts any unwritten leftover to the front, and reports
    // whether it fully drained.
    bool try_flush() {
        if (fill == 0 || txChan == nullptr) {
            fill = 0;
            return true;
        }
        size_t toWrite = (size_t)fill * sizeof(int16_t) * 2;
        size_t written = 0;
        i2s_channel_write(txChan, buf, toWrite, &written, 0);
        size_t framesWritten = written / (sizeof(int16_t) * 2);
        if (framesWritten == (size_t)fill) {
            fill = 0;
            return true;
        }
        if (framesWritten > 0) {
            memmove(buf, buf + framesWritten * 2, (fill - framesWritten) * sizeof(int16_t) * 2);
            fill -= (int)framesWritten;
        }
        return false;
    }

    void drain_blocking() {
        while (!try_flush()) {
            delay(1);
        }
    }

    static constexpr int BUF_FRAMES = 256;
    int16_t buf[BUF_FRAMES * 2];
    int fill = 0;
};

Es8311Output *output = nullptr;
AudioFileSourceFS *source = nullptr;
// Read-ahead RAM buffer wrapped around `source` (see speaker_play()) - not
// AudioFileSourceFS's own doing, that's a bare passthrough to File::read().
// AudioGeneratorWAV (unlike the MP3 decoder) pulls from its source in tiny
// hardcoded 128-byte gulps (its own buffSize, not ours to tune), one SD
// read per gulp - fine for MP3, where one SD read feeds many more
// milliseconds of *decoded* audio, but for raw PCM WAV each gulp is only a
// few ms of playtime, so playback lives or dies on every single SD read
// latency spike. Without this, an ordinary SD latency blip starves the
// I2S TX DMA faster than it can be refilled, and the codec's underrun
// behavior is to keep re-clocking out its last-received samples rather
// than go silent - heard as the same second or two of audio looping
// several times in a row. AudioFileSourceBuffer pre-fills a RAM buffer
// ahead of the decoder's tiny reads (both opportunistically on read() and
// via loop(), pumped every AudioGenerator::loop() call), so those SD
// latency blips get absorbed by RAM instead of stalling the DMA feed.
AudioFileSourceBuffer *bufferedSource = nullptr;
// Base-class pointer - either an AudioGeneratorMP3 or an AudioGeneratorWAV,
// chosen by speaker_play() from the file's extension. Both share the same
// AudioGenerator interface (begin()/loop()/stop()), so nothing past that
// choice needs to know which one it actually is.
AudioGenerator *gen = nullptr;
bool playing = false;

// True if `filename` ends in ".wav" (case-insensitive) - the only other
// extension AUDIO_EXTS (storage.h) lists is ".mp3", so anything that isn't
// this is assumed to be that.
bool has_wav_extension(const char *filename) {
    size_t len = strlen(filename);
    return len >= 4 && strcasecmp(filename + len - 4, ".wav") == 0;
}

// Overwrites the whole TX DMA ring with silence - called from
// speaker_play(), before that track's own real audio starts flowing and
// before it unmutes. txChan is never disabled between tracks (see
// cleanup()'s comment on why: BCLK/WS have to keep toggling), so whatever
// the *previous* track's own last few descriptors happened to hold is
// still sitting there, physically queued, when the next track unmutes -
// on real hardware that's audible as the new track starting with a
// snippet of the *previous* one repeated a handful of times before its
// own content takes over (confirmed: same file played three times in a
// row glitches at the end the first time, then at the *start* the next
// two - the previous play's leftover tail, not this play's own content).
// Blocking is fine here (unlike ConsumeSample()'s own non-blocking
// writes): this is a one-time, track-start cost - one ring's worth of
// frames, well under half a second even at MIC_SAMPLE_RATE - not a
// per-sample one.
void prime_tx_silence() {
    if (txChan == nullptr) return;
    static const int16_t silence[256 * 2] = {0};
    int framesLeft = TX_DMA_RING_FRAMES;
    while (framesLeft > 0) {
        int batch = framesLeft < 256 ? framesLeft : 256;
        size_t toWrite = (size_t)batch * sizeof(int16_t) * 2;
        size_t written = 0;
        i2s_channel_write(txChan, silence, toWrite, &written, portMAX_DELAY);
        framesLeft -= batch;
    }
}

// AudioGeneratorMP3's own buff/mad_stream/mad_frame/mad_synth (~29KB
// combined, mostly libmad's mad_frame/mad_synth structs) default to a
// plain `new`/malloc() each time speaker_play() creates one - lazily
// allocated once here instead and kept for the process's lifetime, so a
// long-running device only ever pays that ~29KB allocation's
// fragmentation risk once, on the first MP3 played, rather than on every
// track.
uint8_t *mp3DecodeBuffer = nullptr;

bool ensure_mp3_buffer() {
    if (mp3DecodeBuffer) return true;
    mp3DecodeBuffer = (uint8_t *)heap_caps_malloc(AudioGeneratorMP3::preAllocSize(), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    return mp3DecodeBuffer != nullptr;
}

void cleanup() {
    delete gen;
    gen = nullptr;
    delete bufferedSource; // must go before `source` - wraps it, doesn't own it
    bufferedSource = nullptr;
    delete source;
    source = nullptr;
    if (playing) {
        // BCLK/WS keep toggling once the I2S channel is enabled regardless
        // of whether anything new is being written to it - with no more
        // real samples coming in, the DMA just keeps re-clocking out
        // whatever was left in its last descriptor(s) (this track's own
        // tail), which the codec dutifully turns into audible noise until
        // muted. Muting the codec's DAC output (not just stopping our own
        // writes) is what actually silences it - es8311_set_mute(false) in
        // speaker_play() undoes this for the next track. Mute FIRST, then
        // sd_end(): SD_MMC.end() isn't instant, and every ms it spends
        // unmounting was, with the order this used to be, a ms of that
        // leftover tail playing on a still-unmuted DAC - on real hardware
        // that was long enough to be heard as this track's own ending
        // looping, every single time.
        es8311_set_mute(true);
        sd_end();
    }
    playing = false;
}

} // namespace

bool speaker_begin() {
    if (hwReady) return true;

    pinMode(PA_EN_PIN, OUTPUT);
    pinMode(PA_CTRL_PIN, OUTPUT);
    // Active-LOW, not active-high like every other enable pin here - per
    // Waveshare's own ESP-IDF example (board_power_bsp.cpp's
    // POWEER_Audio_ON()/OFF(), same GPIO42/Audio_PWR_PIN), 0=on, 1=off.
    // Driving it HIGH (this file's original guess, matching the *other*
    // enable pins' polarity) left the analog audio rail powered off the
    // whole time - I2C still ACKed every register write because the
    // digital/logic rail is separate, which is what made this so
    // confusing to track down: every codec register readback matched a
    // known-good driver exactly, yet no sound, because the rail those
    // registers actually control was never powered.
    digitalWrite(PA_EN_PIN, LOW); // power the codec+amp analog rail
    delay(10); // let the rail settle before talking I2C to the codec

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);

    // ESP8266Audio's own errors (bad frame sync, I/O errors, ...) go to
    // audioLogger, which defaults to a silent sink - point it at Serial so
    // they actually show up instead of just failing quietly.
    audioLogger = &Serial;

    bool codecOk = es8311_init(DEFAULT_SAMPLE_RATE, DEFAULT_VOLUME);
    Serial.printf("speaker: es8311_init -> %s\n", codecOk ? "ok" : "FAILED (I2C error - codec not responding on SDA=47/SCL=48 @0x18?)");
    if (!codecOk) {
        digitalWrite(PA_EN_PIN, HIGH); // rail off (active-low, see above)
        return false;
    }
    bool i2sOk = i2s_configure(DEFAULT_SAMPLE_RATE);
    Serial.printf("speaker: i2s_configure -> %s\n", i2sOk ? "ok" : "FAILED");
    if (!i2sOk) {
        digitalWrite(PA_EN_PIN, HIGH); // rail off (active-low, see above)
        return false;
    }

    digitalWrite(PA_CTRL_PIN, HIGH); // un-shutdown the NS4150B amp
    hwReady = true;
    output = new Es8311Output();
    // AudioOutput's own constructor (ESP8266Audio/src/AudioOutput.h) never
    // initializes gainF2P6 (the fixed-point gain Amplify() multiplies every
    // decoded sample by) - it's whatever garbage byte `new` happened to
    // hand back, and nothing else in this codebase calls SetGain() to
    // give it a real value. That's what made the raw-tone test (which
    // writes straight to I2S, bypassing Amplify() entirely) audible while
    // real MP3 playback (which routes every sample through it) stayed
    // silent even with the PA_EN fix in place.
    output->SetGain(1.0f);
    Serial.println("speaker: hardware ready");
    return true;
}

void speaker_play(const char *filename) {
    speaker_stop();

    if (!speaker_begin()) {
        Serial.println("speaker_play: speaker_begin() failed, not playing");
        return;
    }
    // Insurance against the DAC being unmuted here with nothing real ever
    // written to the TX ring yet - es8311_init() (inside speaker_begin(),
    // only on this device's very first speaker_begin() call) unmutes right
    // after bringing the codec up, before this function has primed
    // anything into the ring below.
    es8311_set_mute(true);
    if (!sd_begin()) {
        Serial.println("speaker_play: sd_begin() failed, not playing");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);
    source = new AudioFileSourceFS(sd_fs(), path);
    if (!source->isOpen()) {
        Serial.printf("speaker_play: couldn't open %s\n", path);
        delete source;
        source = nullptr;
        sd_end();
        return;
    }

    bool isWav = has_wav_extension(filename);
    // WAV needs no persistent decode buffer (AudioGeneratorWAV just
    // streams PCM straight through) - only reserve/reuse the MP3 one
    // (mp3DecodeBuffer) when actually about to decode MP3.
    if (!isWav && !ensure_mp3_buffer()) {
        Serial.println("speaker_play: couldn't allocate MP3 decode buffer (out of memory), not playing");
        delete source;
        source = nullptr;
        sd_end();
        return;
    }

    // Stay muted through setup below (gen->begin() finalizes the I2S rate
    // for this track, and prime_tx_silence() clears the TX ring right
    // after) - only unmute once that's all done, see prime_tx_silence()'s
    // comment for why unmuting any earlier lets the *previous* track's
    // leftover DMA content bleed into this one's start.

    // See bufferedSource's own comment (above) for why this wrapper exists -
    // gen decodes from it, never straight from `source`.
    bufferedSource = new AudioFileSourceBuffer(source, 4096);

    gen = isWav ? static_cast<AudioGenerator *>(new AudioGeneratorWAV())
                : static_cast<AudioGenerator *>(new AudioGeneratorMP3(mp3DecodeBuffer, AudioGeneratorMP3::preAllocSize()));
    playing = true;
    bool started = gen->begin(bufferedSource, output);
    Serial.printf("speaker_play: gen->begin(%s) -> %s\n", path, started ? "ok" : "FAILED");
    if (started) {
        prime_tx_silence();
        es8311_set_mute(false); // undo cleanup()'s mute from any previous track
    } else {
        cleanup();
    }
}

void speaker_stop() {
    if (!playing) return;
    if (gen) gen->stop();
    cleanup();
    Serial.println("speaker_stop: stopped");
}

void speaker_process() {
    if (!playing || !gen) return;
    if (!gen->loop()) {
        Serial.println("speaker_process: gen->loop() returned false, stopping (track ended or decode error - see audioLogger output above)");
        cleanup();
    }
}

bool speaker_is_playing() {
    return playing;
}

// -----------------------------------------------------------------------
// Mic recording. Shares this file's I2C/I2S/PA_EN bring-up (speaker_begin())
// and i2s_configure() with playback above - see this file's/speaker.h's
// top comments for why recording and playback live in one module. Written
// straight to uncompressed 16-bit PCM WAV (write_wav_header()/
// patch_wav_header() below) - see MIC_SAMPLE_RATE's comment above for why.
// -----------------------------------------------------------------------

namespace {

File recordFile;
bool recording = false;
char recordFilename[64];
uint32_t recordedDataBytes = 0; // PCM bytes written so far - patch_wav_header() needs the final count
char micErrorMessage[96] = ""; // see mic_last_error()

// Logs `msg` to Serial (as every failure path here already did) and also
// stashes it for mic_last_error() - ui_epaper.cpp shows that on screen,
// since normal use has no serial monitor attached to see the Serial line.
void set_mic_error(const char *msg) {
    Serial.println(msg);
    strncpy(micErrorMessage, msg, sizeof(micErrorMessage) - 1);
    micErrorMessage[sizeof(micErrorMessage) - 1] = '\0';
}

void write_u32le(File &f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    f.write(b, 4);
}

void write_u16le(File &f, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    f.write(b, 2);
}

// Writes a canonical 44-byte PCM WAV header (RIFF/fmt/data, no extension
// chunks) with the RIFF chunk size and data chunk size fields left as
// zero placeholders - patch_wav_header() below overwrites just those two
// once mic_stop_recording() knows the final PCM byte count. ESP32 is
// little-endian, matching WAV's own byte order, so the mono int16 samples
// mic_process() writes straight after this header need no conversion.
void write_wav_header(File &f, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
    uint16_t blockAlign = (uint16_t)(channels * (bitsPerSample / 8));
    uint32_t byteRate = sampleRate * blockAlign;
    f.write((const uint8_t *)"RIFF", 4);
    write_u32le(f, 0); // patched by patch_wav_header()
    f.write((const uint8_t *)"WAVE", 4);
    f.write((const uint8_t *)"fmt ", 4);
    write_u32le(f, 16); // fmt chunk size (PCM, no extension)
    write_u16le(f, 1);  // audio format 1 = PCM
    write_u16le(f, channels);
    write_u32le(f, sampleRate);
    write_u32le(f, byteRate);
    write_u16le(f, blockAlign);
    write_u16le(f, bitsPerSample);
    f.write((const uint8_t *)"data", 4);
    write_u32le(f, 0); // patched by patch_wav_header()
}

// Seeks back into the two size fields write_wav_header() left as
// placeholders and fills them in now that `dataBytes` (the PCM payload
// actually written) is known - called once, by mic_stop_recording().
void patch_wav_header(File &f, uint32_t dataBytes) {
    f.seek(4); // RIFF chunk size = everything after these first 8 bytes
    write_u32le(f, 36 + dataBytes);
    f.seek(40); // data chunk size
    write_u32le(f, dataBytes);
}

} // namespace

bool mic_start_recording(char *filenameOut, size_t filenameOutLen) {
    speaker_stop(); // mutual exclusion - see this file's top comment

    if (!speaker_begin()) {
        set_mic_error("Couldn't start recording: codec not responding (I2C error)");
        return false;
    }
    // Mute DAC before recording, unconditionally - cleanup() (see its
    // comment) only mutes on the playback-stop path, so a fresh boot that
    // goes straight to Record (es8311_init() leaves the DAC unmuted) never
    // hits that. With the DAC unmuted, txChan stays enabled the whole time
    // (i2s_configure() below always re-enables it) with nothing ever
    // written to it during a pure recording session, so it just re-clocks
    // whatever garbage was left in its DMA descriptors out the speaker -
    // which the mic, inches away at 24dB gain, faithfully records as
    // noise that was never actually in the room.
    es8311_set_mute(true);
    if (!i2s_configure(MIC_SAMPLE_RATE) || rxChan == nullptr) {
        set_mic_error("Couldn't start recording: I2S setup failed");
        return false;
    }
    if (!sd_begin()) {
        set_mic_error("Couldn't start recording: SD card not available");
        return false;
    }

    char name[64];
    if (!next_recording_filename(name, sizeof(name))) {
        set_mic_error("Couldn't start recording: no free RECnnnn.wav name");
        sd_end();
        return false;
    }
    char path[80];
    snprintf(path, sizeof(path), "/%s", name);
    recordFile = sd_fs().open(path, FILE_WRITE);
    if (!recordFile) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Couldn't start recording: couldn't create %s", path);
        set_mic_error(msg);
        sd_end();
        return false;
    }

    write_wav_header(recordFile, MIC_SAMPLE_RATE, /*channels=*/1, /*bitsPerSample=*/16);
    recordedDataBytes = 0;

    es8311_set_mic_gain(MIC_GAIN_CODE);
    es8311_set_mic_enabled(true);
    if (i2s_channel_enable(rxChan) != ESP_OK) {
        set_mic_error("Couldn't start recording: I2S mic channel enable failed");
        es8311_set_mic_enabled(false);
        recordFile.close();
        sd_end();
        return false;
    }
    rxEnabled = true;

    micErrorMessage[0] = '\0'; // clear any previous failure now that this one succeeded

    strncpy(recordFilename, name, sizeof(recordFilename) - 1);
    recordFilename[sizeof(recordFilename) - 1] = '\0';
    if (filenameOut) {
        strncpy(filenameOut, name, filenameOutLen - 1);
        filenameOut[filenameOutLen - 1] = '\0';
    }
    recording = true;
    Serial.printf("mic_start_recording: recording to %s\n", path);
    return true;
}

void mic_stop_recording() {
    if (!recording) return;

    patch_wav_header(recordFile, recordedDataBytes);
    size_t finalSize = recordFile.size(); // logged below, before close() invalidates it
    recordFile.close();
    sd_end();

    i2s_channel_disable(rxChan);
    rxEnabled = false;
    es8311_set_mic_enabled(false);

    recording = false;
    Serial.printf("mic_stop_recording: saved %s (%u bytes)\n", recordFilename, (unsigned)finalSize);
}

void mic_process() {
    if (!recording) return;

    // A modest chunk per call, read non-blockingly (timeout 0) - loop()
    // calls this every iteration regardless of recording state (same as
    // speaker_process()), so there's no need to wait around here for a
    // full DMA descriptor; whatever's already landed gets drained, the
    // rest comes on the next call.
    int16_t stereoBuf[128 * 2];
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rxChan, stereoBuf, sizeof(stereoBuf), &bytesRead, 0);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Recording stopped: I2S read failed (%d)", (int)err);
        set_mic_error(msg);
        mic_stop_recording();
        return;
    }

    size_t framesRead = bytesRead / (2 * sizeof(int16_t));
    if (framesRead == 0) return;

    // Codec's ADC output is stereo-framed (see this file's top comment on
    // the shared TX/RX slot config) but the mic itself is single-ended
    // into one input - left slot only, same channel the reference
    // driver's own "ADCL" reference signal comment names. Pull those out
    // into a mono buffer and write it straight to the WAV file.
    int16_t monoBuf[128];
    for (size_t i = 0; i < framesRead; i++) monoBuf[i] = stereoBuf[i * 2];
    recordFile.write((const uint8_t *)monoBuf, framesRead * sizeof(int16_t));
    recordedDataBytes += (uint32_t)(framesRead * sizeof(int16_t));
}

bool mic_is_recording() {
    return recording;
}

const char *mic_last_error() {
    return micErrorMessage;
}

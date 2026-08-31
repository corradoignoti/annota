#include "speaker.h"

// See speaker.h's top comment for why this whole file only exists in the
// esp32-s3-epaper154 build - it's excluded from esp32-cyd's build_src_filter
// entirely (platformio.ini), and this #ifdef is defense-in-depth on top of
// that, same reasoning as display_epaper.cpp/ui_epaper.cpp's own.
#ifdef BOARD_ESP32S3_EPAPER154

#include <Arduino.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>

// mp3_shine_esp32 (see platformio.ini's lib_deps comment) is a plain C
// library with no library.json of its own, and its headers - unlike every
// other dependency's here - don't guard themselves with
// `#ifdef __cplusplus extern "C"`. Its .c files still compile (and export
// their symbols) as C regardless, so without this wrapper we'd emit
// C++-mangled *calls* to shine_initialise()/shine_encode_buffer_
// interleaved()/etc. that never match the unmangled symbols the library
// itself built - a link error, not a compile error, so it'd only surface
// at the very end of a build.
extern "C" {
#include <layer3.h>
}

#include "es8311.h"
#include "storage.h"
#include "wifi_manager.h"

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
// speech-friendly rate and a low bitrate - smaller files, faster to
// stream to whichever AI provider transcribe.cpp is compiled for. 16kHz
// is one of es8311.h's fixed 256x-MCLK family members, so no codec
// clock-plan changes are needed switching between this and playback's
// 44.1kHz. ES8311_MIC_GAIN_24DB (see es8311.h) is a reasonable starting
// gain for a board-mounted mic at conversational distance - not tuned
// against real hardware yet, may need adjusting once it is.
constexpr uint32_t MIC_SAMPLE_RATE = 16000;
constexpr int MIC_BITRATE_KBPS = 32;
constexpr int MIC_GAIN_CODE = 4; // ES8311_MIC_GAIN_24DB

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
        chanCfg.dma_desc_num = 8;
        chanCfg.dma_frame_num = 480;
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

// Buffers decoded samples and blocking-writes them to the I2S DMA in
// chunks - that block is what paces playback to the real sample rate.
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
        MakeSampleStereo16(sample);
        buf[fill * 2] = Amplify(sample[LEFTCHANNEL]);
        buf[fill * 2 + 1] = Amplify(sample[RIGHTCHANNEL]);
        fill++;
        if (fill == BUF_FRAMES) flushBuf();
        return true;
    }

    void flush() override {
        flushBuf();
    }

    bool stop() override {
        flushBuf();
        return true;
    }

private:
    void flushBuf() {
        if (fill == 0 || txChan == nullptr) {
            fill = 0;
            return;
        }
        size_t written = 0;
        i2s_channel_write(txChan, buf, fill * sizeof(int16_t) * 2, &written, portMAX_DELAY);
        fill = 0;
    }

    static constexpr int BUF_FRAMES = 256;
    int16_t buf[BUF_FRAMES * 2];
    int fill = 0;
};

Es8311Output *output = nullptr;
AudioFileSourceFS *source = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;
bool playing = false;

// AudioGeneratorMP3's own buff/mad_stream/mad_frame/mad_synth (~29KB
// combined, mostly libmad's mad_frame/mad_synth structs) default to a
// plain `new`/malloc() each time speaker_play() creates one - on a board
// with plenty of free heap that's fine, but this board's is a tight,
// heavily-churned budget (mic recording's encoder needing a ~40-50KB
// working set of its own - see mic_start_recording()'s comment) where a
// single ~20KB+ contiguous request can fail to find room even when the
// *total* free byte count looks adequate - that's what made a
// just-recorded file (proven fine - it played back fine off-device) fail
// mp3->begin() on-device with no other symptom.
//
// A single *permanent* static reservation would sidestep that
// fragmentation risk completely, but this board doesn't have room for
// both this buffer AND the encoder's own working set resident at once -
// they'd need to fit in the same scarce free-heap budget even though
// they're never actually needed at the same time (recording always stops
// playback first, and vice versa). So instead this is heap memory this
// module explicitly hands back and forth between the two: released by
// mic_start_recording() (giving its ~29KB to the encoder, the same way
// wifi_suspend_for_memory() gives back WiFi's), and reallocated
// opportunistically by mic_stop_recording() right after - the least
// fragmented moment it'll ever get another shot at, since that's right
// after the encoder's own allocations were just freed. ensure_mp3_buffer()
// (called from speaker_play() too) is the fallback for whenever that
// eager reallocation didn't happen to succeed.
uint8_t *mp3DecodeBuffer = nullptr;

bool ensure_mp3_buffer() {
    if (mp3DecodeBuffer) return true;
    mp3DecodeBuffer = (uint8_t *)heap_caps_malloc(AudioGeneratorMP3::preAllocSize(), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    return mp3DecodeBuffer != nullptr;
}

void release_mp3_buffer() {
    heap_caps_free(mp3DecodeBuffer);
    mp3DecodeBuffer = nullptr;
}

void cleanup() {
    delete mp3;
    mp3 = nullptr;
    delete source;
    source = nullptr;
    if (playing) {
        sd_end();
        // BCLK/WS keep toggling once the I2S channel is enabled regardless
        // of whether anything new is being written to it - with no more
        // real samples coming in, the DMA just keeps re-clocking out
        // whatever was left in its last descriptor(s), which the codec
        // dutifully turns into audible noise. Muting the codec's DAC
        // output (not just stopping our own writes) is what actually
        // silences it; es8311_set_mute(false) in speaker_play() undoes
        // this for the next track.
        es8311_set_mute(true);
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

    if (!ensure_mp3_buffer()) {
        Serial.println("speaker_play: couldn't allocate MP3 decode buffer (out of memory), not playing");
        delete source;
        source = nullptr;
        sd_end();
        return;
    }

    es8311_set_mute(false); // undo cleanup()'s mute from any previous track

    mp3 = new AudioGeneratorMP3(mp3DecodeBuffer, AudioGeneratorMP3::preAllocSize());
    playing = true;
    bool started = mp3->begin(source, output);
    Serial.printf("speaker_play: mp3->begin(%s) -> %s\n", path, started ? "ok" : "FAILED");
    if (!started) {
        cleanup();
    }
}

void speaker_stop() {
    if (!playing) return;
    if (mp3) mp3->stop();
    cleanup();
    Serial.println("speaker_stop: stopped");
}

void speaker_process() {
    if (!playing || !mp3) return;
    if (!mp3->loop()) {
        Serial.println("speaker_process: mp3->loop() returned false, stopping (track ended or decode error - see audioLogger output above)");
        cleanup();
    }
}

bool speaker_is_playing() {
    return playing;
}

// -----------------------------------------------------------------------
// Mic recording. Shares this file's I2C/I2S/PA_EN bring-up (speaker_begin())
// and i2s_configure() with playback above - see this file's/speaker.h's
// top comments for why recording and playback live in one module. Encoded
// with mp3_shine_esp32 (platformio.ini's lib_deps comment), a fixed-point
// port of the old Shine encoder - no Psychoacoustic model, so quality is
// well below a "real" MP3 encoder's, but more than sufficient for a voice
// memo headed for an AI transcription API (transcribe.h), and light enough
// to run in real time on this chip.
// -----------------------------------------------------------------------

namespace {

shine_t shineEncoder = nullptr;
File recordFile;
bool recording = false;
char recordFilename[64];
int shineSamplesPerPass = 0;
int16_t monoAccum[SHINE_MAX_SAMPLES]; // filled from rxChan's left channel until a full pass is ready
int monoAccumFill = 0;
char micErrorMessage[96] = ""; // see mic_last_error()
bool wifiWasConnectedForRecording = false; // see wifi_manager.h's wifi_suspend_for_memory()

// Logs `msg` to Serial (as every failure path here already did) and also
// stashes it for mic_last_error() - ui_epaper.cpp shows that on screen,
// since normal use has no serial monitor attached to see the Serial line.
void set_mic_error(const char *msg) {
    Serial.println(msg);
    strncpy(micErrorMessage, msg, sizeof(micErrorMessage) - 1);
    micErrorMessage[sizeof(micErrorMessage) - 1] = '\0';
}

// Undoes mic_start_recording()'s wifi_suspend_for_memory(), if it did
// one - called from every mic_start_recording() failure return (nothing
// actually started, no reason to keep WiFi off) and from
// mic_stop_recording() (a recording that did start is now over). Only
// asks wifi_manager.h to reconnect if WiFi was actually up before -
// otherwise this would pop the "couldn't connect" timeout dialog after
// every recording on a device that was already offline on purpose.
void restore_wifi_if_needed() {
    if (wifiWasConnectedForRecording) {
        wifi_request_reconnect(); // picked up by main.cpp's loop(), right after this same ui_process_input() call returns
        wifiWasConnectedForRecording = false;
    }
}

// Encodes whatever's in monoAccum (padding with silence to a full pass if
// it's short - shine only ever accepts exactly shineSamplesPerPass samples
// per call) and writes the result out, then resets the accumulator.
// Shared by mic_process()'s normal per-pass flushes and
// mic_stop_recording()'s final partial one.
void encode_and_write_accum() {
    if (monoAccumFill == 0) return;
    for (int i = monoAccumFill; i < shineSamplesPerPass; i++) monoAccum[i] = 0;
    int written = 0;
    unsigned char *data = shine_encode_buffer_interleaved(shineEncoder, monoAccum, &written);
    if (written > 0 && data != nullptr) recordFile.write(data, written);
    monoAccumFill = 0;
}

} // namespace

bool mic_start_recording(char *filenameOut, size_t filenameOutLen) {
    speaker_stop(); // mutual exclusion - see this file's top comment

    // This board has no PSRAM (see platformio.ini's board comment) and
    // the encoder below needs a ~70KB working set - more than this chip
    // has free once WiFi/LVGL/etc already claimed their share of 320KB
    // total SRAM. WiFi's own stack normally holds tens of KB of that, so
    // borrow it back for the duration of the recording (restored by
    // restore_wifi_if_needed(), below and in mic_stop_recording()) -
    // pointless (and disruptive - see that function's comment) to do this
    // if the device wasn't even online to begin with.
    wifiWasConnectedForRecording = (WiFi.status() == WL_CONNECTED);
    if (wifiWasConnectedForRecording) wifi_suspend_for_memory();
    release_mp3_buffer(); // give playback's ~29KB back too - see its own comment

    if (!speaker_begin()) {
        set_mic_error("Couldn't start recording: codec not responding (I2C error)");
        restore_wifi_if_needed();
        return false;
    }
    if (!i2s_configure(MIC_SAMPLE_RATE) || rxChan == nullptr) {
        set_mic_error("Couldn't start recording: I2S setup failed");
        restore_wifi_if_needed();
        return false;
    }
    if (!sd_begin()) {
        set_mic_error("Couldn't start recording: SD card not available");
        restore_wifi_if_needed();
        return false;
    }

    char name[64];
    if (!next_recording_filename(name, sizeof(name))) {
        set_mic_error("Couldn't start recording: no free RECnnnn.mp3 name");
        sd_end();
        restore_wifi_if_needed();
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
        restore_wifi_if_needed();
        return false;
    }

    shine_config_t cfg = {};
    shine_set_config_mpeg_defaults(&cfg.mpeg);
    cfg.mpeg.mode = MONO;
    cfg.mpeg.bitr = MIC_BITRATE_KBPS;
    cfg.wave.channels = PCM_MONO;
    cfg.wave.samplerate = (int)MIC_SAMPLE_RATE;
    if (shine_check_config(cfg.wave.samplerate, cfg.mpeg.bitr) < 0) {
        set_mic_error("Couldn't start recording: encoder rejected samplerate/bitrate");
        recordFile.close();
        sd_end();
        restore_wifi_if_needed();
        return false;
    }
    shineEncoder = shine_initialise(&cfg);
    if (!shineEncoder) {
        // Report the actual free-heap number rather than just guessing
        // "out of memory" - the only way to tell "genuinely too little
        // RAM" from some other alloc failure without a serial monitor
        // attached. Should be rare now that WiFi's RAM is freed above;
        // if this still fires with a low number, there's nothing left to
        // free - the encoder's footprint itself would need shrinking.
        char msg[96];
        snprintf(msg, sizeof(msg), "Couldn't start recording: encoder init failed (%u bytes free internal heap)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        set_mic_error(msg);
        recordFile.close();
        sd_end();
        restore_wifi_if_needed();
        return false;
    }
    shineSamplesPerPass = shine_samples_per_pass(shineEncoder);
    monoAccumFill = 0;

    es8311_set_mic_gain(MIC_GAIN_CODE);
    es8311_set_mic_enabled(true);
    if (i2s_channel_enable(rxChan) != ESP_OK) {
        set_mic_error("Couldn't start recording: I2S mic channel enable failed");
        es8311_set_mic_enabled(false);
        shine_close(shineEncoder);
        shineEncoder = nullptr;
        recordFile.close();
        sd_end();
        restore_wifi_if_needed();
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

    encode_and_write_accum(); // flush the last, possibly partial, pass
    int written = 0;
    unsigned char *data = shine_flush(shineEncoder, &written);
    if (written > 0 && data != nullptr) recordFile.write(data, written);
    shine_close(shineEncoder);
    shineEncoder = nullptr;

    // Try reclaiming playback's buffer right now, before WiFi reconnects
    // and starts allocating/fragmenting things again below - the
    // least-fragmented this heap will be until the next full reboot, now
    // that the encoder's own allocations are all freed. Best-effort:
    // speaker_play()'s own ensure_mp3_buffer() call retries later if this
    // doesn't happen to succeed.
    if (!ensure_mp3_buffer()) {
        Serial.println("mic_stop_recording: couldn't reclaim MP3 decode buffer yet - speaker_play() will retry");
    }

    size_t finalSize = recordFile.size(); // logged below, before close() invalidates it
    recordFile.close();
    sd_end();

    i2s_channel_disable(rxChan);
    rxEnabled = false;
    es8311_set_mic_enabled(false);

    recording = false;
    restore_wifi_if_needed();
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
    for (size_t i = 0; i < framesRead; i++) {
        // Codec's ADC output is stereo-framed (see this file's top
        // comment on the shared TX/RX slot config) but the mic itself is
        // single-ended into one input - left slot only, same channel the
        // reference driver's own "ADCL" reference signal comment names.
        monoAccum[monoAccumFill++] = stereoBuf[i * 2];
        if (monoAccumFill == shineSamplesPerPass) encode_and_write_accum();
    }
}

bool mic_is_recording() {
    return recording;
}

const char *mic_last_error() {
    return micErrorMessage;
}

#endif // BOARD_ESP32S3_EPAPER154

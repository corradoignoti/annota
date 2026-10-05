#include "display.h"

#ifdef BOARD_EPAPER_397

#include <Arduino.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

// -----------------------------------------------------------------------
// Waveshare ESP32-S3-ePaper-3.97: 800x480 mono e-paper (SSD1677-class
// controller), no touch. Buttons: display_buttons.cpp. Used in portrait:
// LVGL renders a 480x800 frame (SCREEN_W x SCREEN_H) and disp_flush_cb()
// rotates it onto the 800x480 panel (PANEL_W x PANEL_H) - every panel
// command below is in panel coordinates.
//
// Init/refresh command sequences below are taken from Waveshare's own
// example repo (waveshareteam/ESP32-S3-ePaper-3.97,
// Arduino/examples/02_E-Paper_Example/EPD_3in97.cpp and the factory
// firmware's ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/
// epaper_port.c) - ported to Arduino's SPIClass, same as the 1.54's driver
// (display_epaper.cpp). Unlike the 1.54, this panel's waveforms live in the
// controller's OTP: no LUT tables to upload, the 0x22 "display update
// control" byte alone picks full (0xF7) vs partial (0xFF) refresh.
// -----------------------------------------------------------------------

static const int PANEL_W = 800;
static const int PANEL_H = 480;
static_assert(PANEL_W == SCREEN_H && PANEL_H == SCREEN_W, "board.h's screen size must be the panel's, rotated");

// Which way portrait is rotated onto the panel. false: the UI's top edge
// lands on the panel's right (landscape) edge, i.e. turn the board a
// quarter-turn counter-clockwise from landscape to read it. true: the
// other way round (180 degrees from false).
static const bool EPD_PORTRAIT_FLIP = true;

static const int EPD_BUF_LEN = (PANEL_W * PANEL_H) / 8; // 48000, 1 bit/px

// The 1bpp panel image (48 KB) lives in PSRAM - too big for internal DRAM,
// which WiFi/lwIP/TLS need (see display_epaper.cpp's draw_buf comment).
// LVGL itself renders in LV_DISPLAY_RENDER_MODE_PARTIAL into a small strip
// buffer in *internal* RAM instead of a full 768 KB RGB565 frame in PSRAM:
// software rendering into PSRAM was the slowest step of a screen change,
// and the strip is converted into epd_buf as each piece arrives, so only
// the areas LVGL actually redrew are touched. The panel itself is refreshed
// once, on the last strip of a frame (lv_display_flush_is_last()).
static uint8_t *epd_buf;
static lv_display_t *display;
static const int DRAW_BUF_LINES = 20;
static const size_t DRAW_BUF_BYTES = SCREEN_W * DRAW_BUF_LINES * sizeof(lv_color_t); // ~19 KB
static lv_color_t *draw_buf;

// One Serial line per panel refresh with where the time went - for tuning.
static const bool EPD_LOG_TIMING = true;
static uint32_t frame_start_ms = 0; // first strip of the frame being flushed

// Partial refreshes leave a little ghosting behind each time; a full
// (flashing) refresh every this many flushes clears it.
static const uint16_t EPD_FULL_REFRESH_EVERY = 30;
static uint16_t partials_since_full = 0;

// Waveshare's factory firmware drives this panel at 20 MHz over a dedicated
// SPI host; their Arduino example bit-bangs it far slower. 10 MHz moves the
// 48 KB frame in ~40 ms with margin left on the GPIO-matrix-routed lines.
static const uint32_t EPD_SPI_HZ = 10000000;

// Upper bound on any single BUSY wait (a full refresh takes ~3.5 s). Past
// it the panel is assumed unresponsive - logged, and the caller carries on
// rather than hanging setup()/loop() forever.
static const uint32_t EPD_BUSY_TIMEOUT_MS = 10000;

// -----------------------------------------------------------------------
// SSD1677-class command/data plumbing
// -----------------------------------------------------------------------

static void epd_read_busy() {
    delay(10);
    uint32_t start = millis();
    while (digitalRead(EPD_BUSY_PIN) == HIGH) { // HIGH: busy, LOW: idle
        if (millis() - start > EPD_BUSY_TIMEOUT_MS) {
            Serial.println("display: BUSY stuck high - panel not responding (power/wiring?)");
            return;
        }
        delay(5);
    }
}

static void epd_cmd(uint8_t command) {
    digitalWrite(EPD_DC_PIN, LOW);
    digitalWrite(EPD_CS_PIN, LOW);
    SPI.transfer(command);
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(EPD_DC_PIN, HIGH); // leave DC high - every following byte defaults to data
}

static void epd_data(uint8_t data) {
    digitalWrite(EPD_DC_PIN, HIGH);
    digitalWrite(EPD_CS_PIN, LOW);
    SPI.transfer(data);
    digitalWrite(EPD_CS_PIN, HIGH);
}

static void epd_write_bytes(const uint8_t *buf, int len) {
    digitalWrite(EPD_DC_PIN, HIGH);
    digitalWrite(EPD_CS_PIN, LOW);
    // writeBytes(), not transfer(): no MISO wired, nothing to read back,
    // and transfer() would overwrite buf with the (meaningless) reply.
    SPI.writeBytes(buf, len);
    digitalWrite(EPD_CS_PIN, HIGH);
}

// 20/2/20 ms - Waveshare's Arduino example's timing (the factory firmware
// uses 50/2/50); runs before every partial refresh, so it's on every
// screen change's critical path.
static void epd_hw_reset() {
    digitalWrite(EPD_RST_PIN, HIGH);
    delay(20);
    digitalWrite(EPD_RST_PIN, LOW);
    delay(2);
    digitalWrite(EPD_RST_PIN, HIGH);
    delay(20);
}

static void epd_turn_on_display(uint8_t mode) {
    epd_cmd(0x22); // display update control: 0xF7 full, 0xFF partial
    epd_data(mode);
    epd_cmd(0x20); // master activation
    epd_read_busy();
}

// Full-refresh init - EPD_3IN97_Init().
static void epd_init_full() {
    epd_hw_reset();
    epd_read_busy();
    epd_cmd(0x12); // SWRESET
    epd_read_busy();

    epd_cmd(0x18); // temperature sensor: internal
    epd_data(0x80);

    epd_cmd(0x0C); // booster soft-start
    epd_data(0xAE);
    epd_data(0xC7);
    epd_data(0xC3);
    epd_data(0xC0);
    epd_data(0x80);

    epd_cmd(0x01); // driver output control
    epd_data((PANEL_H - 1) % 256);
    epd_data((PANEL_H - 1) / 256);
    epd_data(0x02);

    epd_cmd(0x3C); // border waveform
    epd_data(0x01);

    epd_cmd(0x11); // data entry mode
    epd_data(0x01);

    epd_cmd(0x44); // RAM X start/end
    epd_data(0x00);
    epd_data(0x00);
    epd_data((PANEL_W - 1) % 256);
    epd_data((PANEL_W - 1) / 256);

    epd_cmd(0x45); // RAM Y start/end
    epd_data((PANEL_H - 1) % 256);
    epd_data((PANEL_H - 1) / 256);
    epd_data(0x00);
    epd_data(0x00);

    epd_cmd(0x4E); // RAM X counter
    epd_data(0x00);
    epd_data(0x00);
    epd_cmd(0x4F); // RAM Y counter
    epd_data(0x00);
    epd_data(0x00);
    epd_read_busy();
}

// Seeds both the current and "previous frame" RAM banks with the same
// image and does a full refresh - EPD_3IN97_Display_Base(). Partial
// refreshes afterwards diff against bank 0x26.
static void epd_display_base_image() {
    epd_init_full();
    epd_cmd(0x24);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_cmd(0x26);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_turn_on_display(0xF7);
}

// Whole-screen partial refresh - the factory firmware's
// EPD_Display_Partial(img, 0, 0, W, H).
static void epd_display_partial() {
    epd_hw_reset();

    epd_cmd(0x18);
    epd_data(0x80);

    epd_cmd(0x3C); // border waveform: follow LUT (partial)
    epd_data(0x80);

    epd_cmd(0x44);
    epd_data(0x00);
    epd_data(0x00);
    epd_data((PANEL_W - 1) & 0xFF);
    epd_data(((PANEL_W - 1) >> 8) & 0xFF);

    // Y window end-first, exactly as the factory firmware's
    // EPD_Display_Partial() (the Arduino example's start-first order
    // differs; the factory one is what's proven with a full-screen window).
    epd_cmd(0x45);
    epd_data((PANEL_H - 1) & 0xFF);
    epd_data(((PANEL_H - 1) >> 8) & 0xFF);
    epd_data(0x00);
    epd_data(0x00);

    epd_cmd(0x4E);
    epd_data(0x00);
    epd_data(0x00);
    epd_cmd(0x4F);
    epd_data(0x00);
    epd_data(0x00);

    epd_cmd(0x24);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_turn_on_display(0xFF);
}

// -----------------------------------------------------------------------
// LVGL bridge - LVGL renders RGB565 strips (LV_DISPLAY_RENDER_MODE_PARTIAL),
// thresholded to 1bpp and rotated into epd_buf here; the panel refresh
// happens once per frame, on its last strip.
// -----------------------------------------------------------------------

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t t0 = millis();
    if (frame_start_ms == 0) frame_start_ms = t0;
    const uint16_t *px = (const uint16_t *)px_map;
    const int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++) {
        // Portrait row y is one panel column (a quarter-turn, see
        // EPD_PORTRAIT_FLIP): fixed byte/bit within each panel row, the
        // panel row stepping by one per portrait x.
        const int panelX = EPD_PORTRAIT_FLIP ? y : PANEL_W - 1 - y;
        const uint8_t bit = 0x80 >> (panelX & 0x07);
        uint8_t *dst;
        int step;
        if (EPD_PORTRAIT_FLIP) {
            dst = epd_buf + (PANEL_H - 1 - area->x1) * (PANEL_W / 8) + (panelX >> 3);
            step = -(PANEL_W / 8);
        } else {
            dst = epd_buf + area->x1 * (PANEL_W / 8) + (panelX >> 3);
            step = PANEL_W / 8;
        }
        for (int i = 0; i < w; i++) {
            if (*px++ >= 0x7FFF) { // white - see display_epaper.cpp
                *dst |= bit;
            } else {
                *dst &= ~bit;
            }
            dst += step;
        }
    }
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }

    uint32_t t1 = millis();
    bool full = ++partials_since_full >= EPD_FULL_REFRESH_EVERY;
    if (full) {
        partials_since_full = 0;
        epd_display_base_image();
    } else {
        epd_display_partial();
    }
    if (EPD_LOG_TIMING) {
        // render+convert: from the frame's first strip to its last one
        // being converted (LVGL's drawing interleaved with the conversion
        // above); panel: SPI transfer + the refresh itself.
        Serial.printf("display: %s frame, render+convert %lu ms, panel %lu ms\n", full ? "full" : "partial",
                      (unsigned long)(t1 - frame_start_ms), (unsigned long)(millis() - t1));
    }
    frame_start_ms = 0;
    lv_display_flush_ready(disp);
}

void display_init_panel() {
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_RST_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT_PULLUP); // factory firmware pulls BUSY up too
    digitalWrite(EPD_CS_PIN, HIGH);

    SPI.begin(EPD_SCK_PIN, /*miso=*/-1, EPD_MOSI_PIN, EPD_CS_PIN);
    SPI.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));

    epd_buf = (uint8_t *)heap_caps_malloc(EPD_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!epd_buf) {
        Serial.println("display: no PSRAM for the e-paper buffer");
        return;
    }
    memset(epd_buf, 0xFF, EPD_BUF_LEN); // 0xFF = all white
    Serial.printf("display: panel init (BUSY=%d)\n", digitalRead(EPD_BUSY_PIN));
    epd_display_base_image();
    Serial.println("display: base image done");
}

void display_init_input() {
    display_buttons_init();

    lv_init();
    // This lvgl build (9.2.2) ignores lv_conf.h's LV_TICK_CUSTOM macro -
    // tick source is wired at runtime instead.
    lv_tick_set_cb(millis);

    display = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(display, disp_flush_cb);
    draw_buf = (lv_color_t *)heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!draw_buf) draw_buf = (lv_color_t *)heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!draw_buf || !epd_buf) {
        Serial.println("display: out of memory for the display buffers");
        return;
    }
    lv_display_set_buffers(display, draw_buf, NULL, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    // No LVGL indev - see display_epaper.cpp.
}

void display_suspend_touch() {} // no shared SPI peripheral to hand off - see display.h
void display_resume_touch() {}

#endif // BOARD_EPAPER_397

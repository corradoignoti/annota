#include "display.h"

#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>

// -----------------------------------------------------------------------
// Waveshare ESP32-S3-ePaper-1.54: 200x200 mono e-paper (SSD1681-class
// controller) + 2 onboard buttons (BOOT/GPIO0, PWR/GPIO18), no touch.
//
// Pin table and the whole init/LUT/command sequence below are taken from
// Waveshare's own example repo (waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/Arduino/10_LVGL_V9_Test/src/display/epaper_driver_bsp.cpp) -
// ported from raw ESP-IDF spi_master calls to Arduino's SPIClass to match
// the rest of this codebase's style (uses SPIClass directly rather than
// pulling in a panel-specific SPI wrapper lib). The full/partial LUT
// tables are copied verbatim; hand-rolling
// e-paper waveform LUTs from scratch isn't something to improvise.
// -----------------------------------------------------------------------

#define EPD_SCK_PIN  12
#define EPD_MOSI_PIN 13
#define EPD_CS_PIN   11
#define EPD_DC_PIN   10
#define EPD_RST_PIN  9
#define EPD_BUSY_PIN 8
#define EPD_PWR_PIN  6 // active-low: LOW powers the panel on

#define BOOT_BUTTON_PIN 0  // "next" - active-low, has an onboard pull-up
#define PWR_BUTTON_PIN  18 // "select" - active-low, has an onboard pull-up

static const uint8_t WF_FULL[159] = {
    0x80, 0x48, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x48, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x80, 0x48, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x48, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xA, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x8, 0x1, 0x0, 0x8, 0x1, 0x0, 0x2,
    0xA, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
    0x22, 0x17, 0x41, 0x0, 0x32, 0x20,
};

static const uint8_t WF_PARTIAL[159] = {
    0x0, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x80, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x1, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
    0x02, 0x17, 0x41, 0xB0, 0x32, 0x28,
};

static const int EPD_BUF_LEN = (SCREEN_W * SCREEN_H) / 8; // 5000, 1 bit/px

static uint8_t epd_buf[EPD_BUF_LEN];
static lv_display_t *display;
static lv_color_t draw_buf[SCREEN_W * SCREEN_H] __attribute__((aligned(4))); // full-frame RGB565

// -----------------------------------------------------------------------
// SSD1681-class command/data plumbing
// -----------------------------------------------------------------------

static void epd_read_busy() {
    while (digitalRead(EPD_BUSY_PIN) == HIGH) { // HIGH: busy, LOW: idle
        delay(5);
    }
}

static void epd_write_byte(uint8_t b) {
    digitalWrite(EPD_CS_PIN, LOW);
    SPI.transfer(b);
    digitalWrite(EPD_CS_PIN, HIGH);
}

static void epd_cmd(uint8_t command) {
    digitalWrite(EPD_DC_PIN, LOW);
    epd_write_byte(command);
    digitalWrite(EPD_DC_PIN, HIGH); // leave DC high - every following byte defaults to data
}

static void epd_data(uint8_t data) {
    digitalWrite(EPD_DC_PIN, HIGH);
    epd_write_byte(data);
}

static void epd_write_bytes(const uint8_t *buf, int len) {
    digitalWrite(EPD_DC_PIN, HIGH);
    digitalWrite(EPD_CS_PIN, LOW);
    // writeBytes(), not transfer(): transfer() writes the received byte
    // back into the same buffer, which would corrupt the const LUT tables
    // above (flash-resident, not writable) - this bus has no MISO wired
    // anyway (see display_init_panel()), so there's nothing worth reading.
    SPI.writeBytes(buf, len);
    digitalWrite(EPD_CS_PIN, HIGH);
}

static void epd_set_window(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd) {
    epd_cmd(0x44); // SET_RAM_X_ADDRESS_START_END_POSITION
    epd_data((xStart >> 3) & 0xFF);
    epd_data((xEnd >> 3) & 0xFF);

    epd_cmd(0x45); // SET_RAM_Y_ADDRESS_START_END_POSITION
    epd_data(yStart & 0xFF);
    epd_data((yStart >> 8) & 0xFF);
    epd_data(yEnd & 0xFF);
    epd_data((yEnd >> 8) & 0xFF);
}

static void epd_set_cursor(uint16_t x, uint16_t y) {
    epd_cmd(0x4E); // SET_RAM_X_ADDRESS_COUNTER
    epd_data(x & 0xFF);

    epd_cmd(0x4F); // SET_RAM_Y_ADDRESS_COUNTER
    epd_data(y & 0xFF);
    epd_data((y >> 8) & 0xFF);
}

static void epd_set_lut(const uint8_t *lut) {
    epd_cmd(0x32);
    epd_write_bytes(lut, 153);
    epd_read_busy();

    epd_cmd(0x3F);
    epd_data(lut[153]);

    epd_cmd(0x03);
    epd_data(lut[154]);

    epd_cmd(0x04);
    epd_data(lut[155]);
    epd_data(lut[156]);
    epd_data(lut[157]);

    epd_cmd(0x2C);
    epd_data(lut[158]);
}

static void epd_turn_on_display(bool partial) {
    epd_cmd(0x22);
    epd_data(partial ? 0xCF : 0xC7);
    epd_cmd(0x20);
    epd_read_busy();
}

static void epd_hw_reset() {
    digitalWrite(EPD_RST_PIN, HIGH);
    delay(50);
    digitalWrite(EPD_RST_PIN, LOW);
    delay(20);
    digitalWrite(EPD_RST_PIN, HIGH);
    delay(50);
}

static void epd_init_full() {
    epd_hw_reset();
    epd_read_busy();
    epd_cmd(0x12); // SWRESET
    epd_read_busy();

    epd_cmd(0x01); // driver output control
    epd_data(0xC7);
    epd_data(0x00);
    epd_data(0x01);

    epd_cmd(0x11); // data entry mode
    epd_data(0x01);

    epd_set_window(0, SCREEN_H - 1, SCREEN_W - 1, 0);

    epd_cmd(0x3C); // border waveform
    epd_data(0x01);

    epd_cmd(0x18);
    epd_data(0x80);

    epd_cmd(0x22); // load temperature + waveform setting
    epd_data(0xB1);
    epd_cmd(0x20);

    epd_set_cursor(0, SCREEN_H - 1);
    epd_read_busy();

    epd_set_lut(WF_FULL);
}

static void epd_init_partial() {
    epd_hw_reset();
    epd_read_busy();

    epd_set_lut(WF_PARTIAL);

    epd_cmd(0x37);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x40);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x00);

    epd_cmd(0x3C); // border waveform
    epd_data(0x80);

    epd_cmd(0x22);
    epd_data(0xC0);
    epd_cmd(0x20);
    epd_read_busy();
}

static void epd_display_full() {
    epd_cmd(0x24);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_turn_on_display(false);
}

static void epd_display_base_image() {
    // Seeds both the current and "previous frame" RAM banks with the same
    // image so the first partial update afterwards doesn't ghost against
    // stale RAM content from before boot.
    epd_cmd(0x24);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_cmd(0x26);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_turn_on_display(false);
}

static void epd_display_partial() {
    epd_cmd(0x24);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_turn_on_display(true);
}

static void epd_set_pixel(int x, int y, bool white) {
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    int index = y * (SCREEN_W / 8) + (x >> 3);
    uint8_t bit = 7 - (x & 0x07);
    if (white) {
        epd_buf[index] |= (1 << bit);
    } else {
        epd_buf[index] &= ~(1 << bit);
    }
}

// -----------------------------------------------------------------------
// LVGL bridge - thresholds the RGB565 framebuffer LVGL renders into down
// to 1bpp on flush, same approach as Waveshare's own LVGL v9 example for
// this board. LV_DISPLAY_RENDER_MODE_FULL guarantees flush_cb sees the
// whole screen in one call every time (never a sub-rect), which matters
// here: an e-paper "partial refresh" still means resending the *entire*
// panel RAM window each time, just with a faster/lower-ghosting waveform
// than a full refresh - there's no such thing as updating a sub-rect on
// this controller.
// -----------------------------------------------------------------------

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint16_t *px = (uint16_t *)px_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            // RGB565: anything past the midpoint reads as white, else black
            // - this UI is built for a light background with dark text
            // (see ui_epaper.cpp); a dark theme would threshold to
            // mostly-black on this panel.
            epd_set_pixel(x, y, *px >= 0x7FFF);
            px++;
        }
    }
    epd_display_partial();
    lv_display_flush_ready(disp);
}

void display_init_panel() {
    pinMode(EPD_PWR_PIN, OUTPUT);
    digitalWrite(EPD_PWR_PIN, LOW); // active-low power enable
    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_RST_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);

    SPI.begin(EPD_SCK_PIN, /*miso=*/-1, EPD_MOSI_PIN, EPD_CS_PIN);
    // 4MHz is a deliberately conservative starting point, not a verified
    // one - Waveshare's own ESP-IDF example drives this panel at 40MHz.
    // Bump this once real hardware confirms it's stable; nothing here has
    // been tested on an actual board yet.
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

    epd_init_full();
    memset(epd_buf, 0xFF, EPD_BUF_LEN); // 0xFF = all white, matches EPD_1IN54G_WHITE convention
    epd_display_base_image();
    epd_init_partial();
}

void display_init_input() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(PWR_BUTTON_PIN, INPUT_PULLUP);

    lv_init();
    // This lvgl build (9.2.2) ignores lv_conf.h's LV_TICK_CUSTOM macro -
    // tick source is wired at runtime instead.
    lv_tick_set_cb(millis);

    display = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(display, disp_flush_cb);
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    // No LVGL indev registered here - ui_epaper.cpp polls the two buttons
    // directly (via display_button_poll() below) and drives its own
    // list/menu state machine instead of routing through LVGL's
    // click/group-navigation machinery, which is built around continuous
    // pointer/encoder input this 2-button, slow-refresh panel doesn't have.
}

void display_suspend_touch() {} // no shared SPI peripheral to hand off - see display.h
void display_resume_touch() {}

// -----------------------------------------------------------------------
// Button polling - see display.h's DisplayButton/display_button_poll().
// -----------------------------------------------------------------------

static const uint32_t BUTTON_DEBOUNCE_MS = 30;
static const uint32_t BUTTON_LONG_PRESS_MS = 700;

struct ButtonState {
    uint8_t pin;
    uint32_t pressedSinceMs = 0; // 0 while not pressed
    bool longFired = false;
};
static ButtonState buttonStates[2] = {{BOOT_BUTTON_PIN}, {PWR_BUTTON_PIN}};

DisplayButtonEvent display_button_poll(DisplayButton b) {
    ButtonState &s = buttonStates[(int)b];
    bool pressed = digitalRead(s.pin) == LOW; // active-low
    uint32_t now = millis();

    if (pressed) {
        if (s.pressedSinceMs == 0) {
            s.pressedSinceMs = now;
            s.longFired = false;
        } else if (!s.longFired && (now - s.pressedSinceMs) >= BUTTON_LONG_PRESS_MS) {
            s.longFired = true;
            return DisplayButtonEvent::kLong;
        }
        return DisplayButtonEvent::kNone;
    }

    if (s.pressedSinceMs != 0) {
        uint32_t heldMs = now - s.pressedSinceMs;
        bool wasLong = s.longFired;
        s.pressedSinceMs = 0;
        s.longFired = false;
        if (!wasLong && heldMs >= BUTTON_DEBOUNCE_MS) {
            return DisplayButtonEvent::kShort;
        }
    }
    return DisplayButtonEvent::kNone;
}

bool display_button_raw_pressed(DisplayButton b) {
    return digitalRead(buttonStates[(int)b].pin) == LOW; // active-low
}

// See display.h's comment for why this runs its own hold timer instead of
// reusing display_button_poll()'s per-button one.
static const uint32_t FORGET_WIFI_COMBO_HOLD_MS = 5000;
static uint32_t comboPressedSinceMs = 0; // 0 while not both held
static bool comboFired = false;

bool display_forget_wifi_combo_poll() {
    bool bothPressed = display_button_raw_pressed(DisplayButton::kNext) && display_button_raw_pressed(DisplayButton::kSelect);
    if (!bothPressed) {
        comboPressedSinceMs = 0;
        comboFired = false;
        return false;
    }

    uint32_t now = millis();
    if (comboPressedSinceMs == 0) {
        comboPressedSinceMs = now;
        return false;
    }
    if (!comboFired && (now - comboPressedSinceMs) >= FORGET_WIFI_COMBO_HOLD_MS) {
        comboFired = true;
        return true;
    }
    return false;
}

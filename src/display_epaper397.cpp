#include "display.h"

#ifdef BOARD_EPAPER_397

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

// -----------------------------------------------------------------------
// Waveshare ESP32-S3-ePaper-3.97: 800x480 e-paper (rendered 1bpp-
// thresholded here, not this panel's native 4-gray - see disp_flush_cb()
// below), a 3-way rotary nav switch (Up/GPIO4, Function/GPIO5,
// Down/GPIO6) + a separate Boot/GPIO0 button, no touch. Whole file gated
// on BOARD_EPAPER_397 - see platformio.ini and display.h.
//
// Command sequence below is ported from Waveshare's own example repo
// (waveshareteam/ESP32-S3-ePaper-3.97, Arduino/examples/02_E-Paper_Example/
// EPD_3in97.cpp/DEV_Config.cpp) - from their bit-banged GPIO SPI to
// Arduino's SPIClass, same as display_epaper.cpp's own SSD1681 driver was
// ported from Waveshare's ESP-IDF example. Unlike the 154 board's SSD1681,
// this controller has no manually-uploaded waveform LUT - refresh quality
// is selected per-call via the 0x22 "display update control" byte
// (0xF7 = full, 0xFF = partial), so there are no WF_FULL/WF_PARTIAL
// tables here.
// -----------------------------------------------------------------------

#define EPD_SCK_PIN  11
#define EPD_MOSI_PIN 12
#define EPD_CS_PIN   10
#define EPD_RST_PIN  46 // ESP32-S3 boot-strapping pin (VDD_SPI select) - Waveshare's own reference design, trust it
#define EPD_DC_PIN   9
#define EPD_BUSY_PIN 3

// The panel controller's own RAM is fixed at these physical (landscape)
// dimensions - the 0x44/0x45 RAM-window commands, epd_buf's 1bpp packing,
// and epd_set_pixel() are all addressed in this physical coordinate
// space, regardless of how the panel is mounted. display.h's SCREEN_W/
// SCREEN_H are the LOGICAL (portrait, rotated 90 degrees) dimensions
// LVGL/ui_epaper.cpp actually see - disp_flush_cb() below is what
// translates between the two.
static const int PANEL_W = 800;
static const int PANEL_H = 480;

// -----------------------------------------------------------------------
// Panel power: NOT a GPIO on this board. The panel's analog drive rail
// (VSH/VSL/VGH/VGL - what actually moves the electrophoretic ink) is
// switched by the onboard AXP2101 PMIC over I2C, not any pin this MCU
// drives directly. Confirmed the hard way: without this, the panel
// accepts every SPI command and BUSY toggles with plausible, spec-
// matching refresh durations (its digital logic runs off a separate,
// always-on rail) - the screen just never visibly changes, since the
// rail(s) that actually move ink are off. The barebones Arduino example
// this driver is otherwise ported from (Arduino/examples/02_E-Paper_Example)
// doesn't do this step at all and simply doesn't work standalone.
//
// Register sequence below is copied from the factory-shipped board
// bring-up, not guessed or derived from a generic example: 78/xiaozhi-esp32
// (upstream of Waveshare's own ESP32-AIChats binary releases for this
// board), main/boards/waveshare/esp32-s3-epaper-3.97/waveshare-s3-epaper-3.97.cc's
// `Pmic` constructor. That file enables THREE rails together - ALDO1,
// ALDO2, AND ALDO3, all at 3.3V (register 0x90 = 0x07, i.e. bits 0-2) -
// not just ALDO3 alone as Waveshare's own simpler ESP-IDF example
// (epaper_port.c's EPD_Power_ON(), ALDO3/bit 2 only) suggested; that
// narrower single-rail version was tried first against real hardware here
// and did NOT bring the panel up, confirming this board genuinely needs
// all three. This repo doesn't pull in xiaozhi-esp32's full Axp2101/
// I2cDevice class hierarchy for three register writes, so it's
// reimplemented here as direct register access instead - same registers,
// same values, same order (disable-all-first, then set voltages, then
// enable together) as that proven-working reference.
#define AXP2101_I2C_ADDR         0x34
#define AXP2101_REG_DC_ONOFF     0x80 // DCDC on/off control - write 0x01 to disable all but DC1
#define AXP2101_REG_LDO_ONOFF0   0x90 // LDO on/off control 0 - bits 0/1/2 = ALDO1/ALDO2/ALDO3 enable
#define AXP2101_REG_LDO_ONOFF1   0x91 // LDO on/off control 1 - unrelated rails, disabled defensively
#define AXP2101_REG_DC1_VOL      0x82 // DC1 voltage, 100mV/step from 1500mV
#define AXP2101_REG_ALDO1_VOL    0x92 // ALDO1 voltage, 100mV/step from 500mV
#define AXP2101_REG_ALDO2_VOL    0x93 // ALDO2 voltage, 100mV/step from 500mV
#define AXP2101_REG_ALDO3_VOL    0x94 // ALDO3 voltage, 100mV/step from 500mV

static bool axp2101_write_reg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(AXP2101_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

// Brings up the AXP2101 rails the panel (and, per the reference, DC1/
// ALDO1/ALDO2 alongside it) need, once, at boot - this codebase never
// powers them down again afterward (same as the 154 board's EPD_PWR_PIN,
// which is likewise only ever driven once in display_init_panel()), so
// there's no matching disable step here.
static void epd_power_on() {
    // I2C_SDA_PIN/I2C_SCL_PIN (41/42) are speaker.cpp's names for this
    // same shared bus - duplicated here rather than shared through a
    // header since this is the only other file that needs them, and this
    // call must run before speaker_begin() ever does (display_init_panel()
    // is main.cpp's very first hardware init). Wire.begin() is safe to
    // call again later from speaker_begin() on the same pins.
    Wire.begin(41, 42);
    Wire.setClock(400000);

    bool ok = true;
    ok &= axp2101_write_reg(AXP2101_REG_DC_ONOFF, 0x01);   // disable all DCs but DC1
    ok &= axp2101_write_reg(AXP2101_REG_LDO_ONOFF0, 0x00); // disable all LDOs first
    ok &= axp2101_write_reg(AXP2101_REG_LDO_ONOFF1, 0x00);
    ok &= axp2101_write_reg(AXP2101_REG_DC1_VOL, (3300 - 1500) / 100);
    ok &= axp2101_write_reg(AXP2101_REG_ALDO1_VOL, (3300 - 500) / 100);
    ok &= axp2101_write_reg(AXP2101_REG_ALDO2_VOL, (3300 - 500) / 100);
    ok &= axp2101_write_reg(AXP2101_REG_ALDO3_VOL, (3300 - 500) / 100);
    ok &= axp2101_write_reg(AXP2101_REG_LDO_ONOFF0, 0x07); // enable ALDO1+ALDO2+ALDO3 together
    Serial.printf("epd397: AXP2101 power-on sequence %s\n", ok ? "ok" : "I2C FAILED");
}

#define DOWN_BUTTON_PIN     6 // "next" - active-low, internal pull-up (button_bsp.c)
#define FUNCTION_BUTTON_PIN 5 // "select" - active-low, internal pull-up
#define UP_BUTTON_PIN       4 // "prev" - active-low, internal pull-up
#define BOOT_BUTTON_PIN     0 // combo-only, see display_forget_wifi_combo_poll()

static const int EPD_BUF_LEN = (PANEL_W * PANEL_H) / 8; // 48000, 1 bit/px, physical (landscape) layout

static uint8_t epd_buf[EPD_BUF_LEN];
static lv_display_t *display;
// RGB565 full-frame render buffer: 800*480*2 = 768,000 bytes, far too big
// for internal SRAM (~340KB DIRAM on this chip) - must live in PSRAM.
// Allocated in display_init_input(), never freed (lives for the process).
static lv_color_t *draw_buf = nullptr;

// -----------------------------------------------------------------------
// Command/data plumbing - same shape as display_epaper.cpp's, using
// Arduino's hardware SPIClass rather than Waveshare's bit-banged GPIO SPI.
// -----------------------------------------------------------------------

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
    // back into the same buffer, which would corrupt epd_buf's contents
    // mid-send - this bus has no MISO wired anyway (see
    // display_init_panel()), so there's nothing worth reading.
    SPI.writeBytes(buf, len);
    digitalWrite(EPD_CS_PIN, HIGH);
}

static void epd_read_busy() {
    // The initial delay is load-bearing, not cosmetic - the reference
    // driver this is ported from (EPD_3IN97_ReadBusy) waits 100ms before
    // the first poll, since BUSY may not have asserted yet immediately
    // after the triggering command.
    delay(100);
    while (digitalRead(EPD_BUSY_PIN) == HIGH) { // HIGH: busy, LOW: idle
        delay(10);
    }
}

static void epd_hw_reset() {
    digitalWrite(EPD_RST_PIN, HIGH);
    delay(50);
    digitalWrite(EPD_RST_PIN, LOW);
    delay(2);
    digitalWrite(EPD_RST_PIN, HIGH);
    delay(50);
}

static void epd_turn_on_display_full() {
    epd_cmd(0x22);
    epd_data(0xF7);
    epd_cmd(0x20);
    epd_read_busy();
}

static void epd_turn_on_display_part() {
    epd_cmd(0x22);
    epd_data(0xFF);
    epd_cmd(0x20);
    epd_read_busy();
}

// Ported from EPD_3IN97_Init() - panel-specific magic constants (the 0x0C
// booster soft-start sequence especially) copied verbatim, not to be
// second-guessed.
static void epd_init_full() {
    epd_hw_reset();
    epd_read_busy();
    epd_cmd(0x12); // SWRESET
    epd_read_busy();

    epd_cmd(0x18); // temperature sensor select
    epd_data(0x80); // internal sensor

    epd_cmd(0x0C); // booster soft start control
    epd_data(0xAE);
    epd_data(0xC7);
    epd_data(0xC3);
    epd_data(0xC0);
    epd_data(0x80);

    epd_cmd(0x01); // driver output control
    epd_data((PANEL_H - 1) & 0xFF);
    epd_data(((PANEL_H - 1) >> 8) & 0xFF);
    epd_data(0x02);

    epd_cmd(0x3C); // border waveform
    epd_data(0x01);

    epd_cmd(0x11); // data entry mode
    epd_data(0x01);

    epd_cmd(0x44); // set RAM-X address start/end position
    epd_data(0x00);
    epd_data(0x00);
    epd_data((PANEL_W - 1) & 0xFF);
    epd_data(((PANEL_W - 1) >> 8) & 0xFF);

    epd_cmd(0x45); // set RAM-Y address start/end position
    epd_data((PANEL_H - 1) & 0xFF);
    epd_data(((PANEL_H - 1) >> 8) & 0xFF);
    epd_data(0x00);
    epd_data(0x00);

    epd_cmd(0x4E); // set RAM-X address counter
    epd_data(0x00);
    epd_data(0x00);
    epd_cmd(0x4F); // set RAM-Y address counter
    epd_data(0x00);
    epd_data(0x00);
    epd_read_busy();
}

// Single-bank full display, used once at boot right after epd_init_full().
// Deliberately does NOT also write bank 0x26 ("previous frame") the way
// display_epaper.cpp's SSD1681 base-image seed does - Waveshare's own
// shipped, factory-tested firmware for this exact board (78/xiaozhi-esp32,
// main/boards/waveshare/esp32-s3-epaper-3.97/custom_lcd_display.cc's
// EPD_Display(), called from that file's constructor right after EPD_Init())
// only ever writes 0x24 here, never 0x26 - copied verbatim rather than
// carrying over an assumption from the 154 board's different controller.
static void epd_display_full() {
    epd_cmd(0x24);
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_turn_on_display_full();
}

// Every subsequent update, called from disp_flush_cb() on every LVGL
// flush. Deliberately does NOT reset or re-address the panel (no 0x18/
// 0x3C/0x44/0x45/0x4E/0x4F here) - that's the single biggest divergence
// from the barebones Arduino/ESP-IDF example this file was originally
// ported from (both of which reset+re-address on every partial call, the
// same shape as display_epaper.cpp's SSD1681 driver uses for ITS partial
// updates). That reset-every-call approach came from an UNTESTED example
// and turned out not to be what real hardware needs: this board's panel
// stayed completely blank under it despite BUSY toggling with plausible,
// spec-matching timing the whole time. Waveshare's own shipped firmware
// (custom_lcd_display.cc's EPD_DisplayPart(), called from every
// lvgl_flush_cb()) just resends 0x24 + the buffer and triggers the
// partial-style update (0x22/0xFF) directly, reusing the RAM window and
// border-waveform state epd_init_full() already established once at boot
// - copied exactly, since it's what's actually proven to work on this
// silicon rather than what the plain example implied.
static void epd_display_partial() {
    epd_cmd(0x24); // write B/W image to RAM
    epd_write_bytes(epd_buf, EPD_BUF_LEN);
    epd_turn_on_display_part();
}

// x/y are PHYSICAL (landscape) panel coordinates, not logical/portrait
// ones - disp_flush_cb() below does the logical-to-physical rotation
// before calling this.
static void epd_set_pixel(int x, int y, bool white) {
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return;
    int index = y * (PANEL_W / 8) + (x >> 3);
    uint8_t bit = 7 - (x & 0x07);
    if (white) {
        epd_buf[index] |= (1 << bit);
    } else {
        epd_buf[index] &= ~(1 << bit);
    }
}

// -----------------------------------------------------------------------
// LVGL bridge - same threshold-to-1bpp-on-flush approach as
// display_epaper.cpp's (native 4-gray rendering is out of scope for this
// port - see this file's top comment).
// -----------------------------------------------------------------------

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint16_t *px = (uint16_t *)px_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            // RGB565: anything past the midpoint reads as white, else black
            // - this UI is built for a light background with dark text
            // (see ui_epaper.cpp); a dark theme would threshold to
            // mostly-black on this panel.
            //
            // x/y here are LOGICAL (portrait) coordinates - rotate 90
            // degrees counter-clockwise into the panel's PHYSICAL
            // (landscape) RAM coordinates before writing. Confirmed
            // against real hardware: the clockwise mapping
            // (`epd_set_pixel(PANEL_W - 1 - y, x, ...)`) came out upside
            // down/mirrored, so this is the direction this panel actually
            // needs, not an arbitrary choice.
            int xPhys = y;
            int yPhys = SCREEN_W - 1 - x;
            epd_set_pixel(xPhys, yPhys, *px >= 0x7FFF);
            px++;
        }
    }
    epd_display_partial();
    lv_display_flush_ready(disp);
}

void display_init_panel() {
    epd_power_on();
    // 500ms, not a token settle delay - matches the longer of Waveshare's
    // own two Arduino-example precedents for this exact panel
    // (Init_Fast()/Init_4GRAY() both wait 500ms after power-on before
    // resetting; plain Init() waits only 10ms, which is what this used to
    // be here - bumped after 10ms alone wasn't enough to bring the panel
    // up on real hardware).
    delay(500);

    pinMode(EPD_CS_PIN, OUTPUT);
    pinMode(EPD_DC_PIN, OUTPUT);
    pinMode(EPD_RST_PIN, OUTPUT);
    pinMode(EPD_BUSY_PIN, INPUT);
    digitalWrite(EPD_CS_PIN, HIGH);

    SPI.begin(EPD_SCK_PIN, /*miso=*/-1, EPD_MOSI_PIN, EPD_CS_PIN);
    // 20MHz - matches custom_lcd_display.cc's spi_port_init()
    // (devcfg.clock_speed_hz = 20MHz), the proven-working shipped
    // firmware's own value, rather than a from-scratch conservative guess.
    SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

    epd_init_full();
    memset(epd_buf, 0xFF, EPD_BUF_LEN); // 0xFF = all white
    epd_display_full();
}

void display_init_input() {
    pinMode(DOWN_BUTTON_PIN, INPUT_PULLUP);
    pinMode(FUNCTION_BUTTON_PIN, INPUT_PULLUP);
    pinMode(UP_BUTTON_PIN, INPUT_PULLUP);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    lv_init();
    // This lvgl build (9.2.2) ignores lv_conf.h's LV_TICK_CUSTOM macro -
    // tick source is wired at runtime instead.
    lv_tick_set_cb(millis);

    draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * SCREEN_W * SCREEN_H, MALLOC_CAP_SPIRAM);
    if (!draw_buf) {
        // Nothing useful can render without this buffer - fail loudly and
        // immediately rather than limping on with a null LVGL buffer.
        Serial.println("display_init_input: PSRAM allocation for draw_buf failed");
        while (true) delay(1000);
    }

    display = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(display, disp_flush_cb);
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(lv_color_t) * SCREEN_W * SCREEN_H, LV_DISPLAY_RENDER_MODE_FULL);
    // No LVGL indev registered here - ui_epaper.cpp polls the buttons
    // directly (via display_button_poll() below) and drives its own
    // list/menu state machine instead of routing through LVGL's
    // click/group-navigation machinery, which is built around continuous
    // pointer/encoder input this button-driven, slow-refresh panel doesn't
    // have.
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
// Indexed by DisplayButton (display.h): kNext, kSelect, kPrev, kBoot. Every
// slot here is a real, distinct pin - unlike the 154 board, no sentinel
// needed.
static ButtonState buttonStates[4] = {{DOWN_BUTTON_PIN}, {FUNCTION_BUTTON_PIN}, {UP_BUTTON_PIN}, {BOOT_BUTTON_PIN}};

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
    bool bothPressed = display_button_raw_pressed(DisplayButton::kBoot) && display_button_raw_pressed(DisplayButton::kSelect);
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

#endif // BOARD_EPAPER_397

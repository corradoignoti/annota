#include "display.h"

#include <Arduino.h>
#include <SPI.h>

// -----------------------------------------------------------------------
// Waveshare ESP32-S3-ePaper-1.54: 200x200 mono e-paper (SSD1681-class
// controller) + 2 onboard buttons (BOOT/GPIO0, PWR/GPIO18), no touch.
//
// The panel itself is driven by GxEPD2's GxEPD2_154_D67 class (Good
// Display GDEH0154D67/GDEY0154D67 family - same SSD1681 command set and
// waveform family this board's own panel is from), which owns the
// command/LUT/hardware-reset sequence internally - this file only wires
// up SPI/pins and the two onboard buttons, plus a thin display_present()
// wrapper ui_epaper.cpp calls after drawing into display_epd()'s buffer.
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

// Function-local static rather than a plain file-scope global: display_epd()
// is called from other translation units' own static initializers (e.g.
// ui_epaper.cpp caching a reference at namespace scope), and cross-TU
// static-init order is unspecified - a Meyer's-singleton-style local static
// guarantees this is constructed on first actual use instead.
EpdDisplay &display_epd() {
    static EpdDisplay epd(GxEPD2_154_D67(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));
    return epd;
}

// See display.h's comment: still a full-panel RAM rewrite either way on
// this controller (no real sub-rect update), partial just picks the
// faster/lower-ghosting waveform.
void display_present(bool partial) { display_epd().display(partial); }

void display_init_panel() {
    pinMode(EPD_PWR_PIN, OUTPUT);
    digitalWrite(EPD_PWR_PIN, LOW); // active-low power enable

    // This board's SPI pins aren't the default VSPI/HSPI ones, so SPI
    // bring-up + speed stay under our control via selectSPI() rather than
    // GxEPD2's own default SPI::begin(). 4MHz is a deliberately
    // conservative starting point, not a verified one - Waveshare's own
    // ESP-IDF example drives this panel at 40MHz. Bump this once real
    // hardware confirms it's stable; nothing here has been tested on an
    // actual board yet.
    SPI.begin(EPD_SCK_PIN, /*miso=*/-1, EPD_MOSI_PIN, EPD_CS_PIN);
    EpdDisplay &epd = display_epd();
    epd.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));

    // serial_diag_bitrate=0 disables GxEPD2's own diagnostic Serial prints
    // (Serial's already in use for this firmware's own logging - see
    // main.cpp). initial=true runs the panel's full hardware-reset/init
    // sequence, replacing this file's old hand-rolled epd_init_full().
    epd.init(/*serial_diag_bitrate=*/0, /*initial=*/true, /*reset_duration=*/10, /*pulldown_rst_mode=*/false);
    epd.setRotation(0);
    epd.setFullWindow();
    epd.fillScreen(GxEPD_WHITE);
    epd.display(false); // full refresh: seeds the panel's RAM banks + paints the initial white screen
}

void display_init_input() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(PWR_BUTTON_PIN, INPUT_PULLUP);
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

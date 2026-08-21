#include "display.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

// -----------------------------------------------------------------------
// Display / touch plumbing (TFT_eSPI <-> LVGL v9)
//
// On the CYD, the XPT2046 touch controller is NOT on the display's shared
// SPI bus (TFT_MISO/MOSI/SCLK from User_Setup.h) despite TOUCH_CS being
// defined there for TFT_eSPI's own (unused) touch support - it's wired to
// its own separate SPI pins. That's why TFT_eSPI's tft.getTouch() /
// calibrateTouch() see nothing: talking to the wrong bus entirely. Drive
// the touch chip directly with XPT2046_Touchscreen instead.
// -----------------------------------------------------------------------

static TFT_eSPI tft = TFT_eSPI();
static Preferences prefs;

#define XPT2046_CLK  25
#define XPT2046_MISO 39
#define XPT2046_MOSI 32
#define XPT2046_CS   33
#define XPT2046_IRQ  36

// If the pointer ends up mirrored/swapped from your finger after flashing,
// try 1, 2 or 3 here and reflash (touch panel mounting varies by unit).
#define TOUCH_ROTATION 0

static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

static lv_display_t *display;
static lv_color_t draw_buf[SCREEN_W * 40] __attribute__((aligned(4))); // ~25KB partial render buffer

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)px_map, w * h, true);
    tft.endWrite();

    lv_display_flush_ready(disp);
}

// Resistive touch needs a per-panel calibration (raw ADC range -> screen
// px) or readings land off-target. Run once (two-corner tap), then cache
// in NVS so later boots skip straight to mapping with the stored range.
struct TouchCal {
    int16_t rawXMin, rawYMin;
    int16_t rawXMax, rawYMax;
};
static TouchCal touchCal;

static TS_Point read_debounced_point() {
    while (!touchscreen.touched()) {
        delay(10);
    }
    delay(30); // let the resistive reading settle
    return touchscreen.getPoint();
}

static void wait_for_release() {
    while (touchscreen.touched()) {
        delay(10);
    }
    delay(150); // debounce so the next prompt doesn't catch the same tap
}

static void draw_target(int x, int y, const char *label) {
    tft.fillScreen(TFT_BLACK);
    tft.drawFastHLine(x - 10, y, 20, TFT_MAGENTA);
    tft.drawFastVLine(x, y - 10, 20, TFT_MAGENTA);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, SCREEN_H / 2);
    tft.println(label);
}

static void calibrate_touch() {
    prefs.begin("annota", false);
    if (prefs.getBytesLength("touchCal2") == sizeof(touchCal)) {
        prefs.getBytes("touchCal2", &touchCal, sizeof(touchCal));
        prefs.end();
        return;
    }
    prefs.end();

    draw_target(20, 20, "Touch the marker (top-left)");
    TS_Point tl = read_debounced_point();
    wait_for_release();

    draw_target(SCREEN_W - 20, SCREEN_H - 20, "Touch the marker (bottom-right)");
    TS_Point br = read_debounced_point();
    wait_for_release();

    touchCal.rawXMin = tl.x;
    touchCal.rawYMin = tl.y;
    touchCal.rawXMax = br.x;
    touchCal.rawYMax = br.y;

    prefs.begin("annota", false);
    prefs.putBytes("touchCal2", &touchCal, sizeof(touchCal));
    prefs.end();

    tft.fillScreen(TFT_BLACK);
}

// Resistive/IRQ touch reads can drop a sample or two mid-drag. Reporting
// a release on every dropout turns one continuous drag into a string of
// unrelated micro-taps that never cross LVGL's scroll threshold, so hold
// the last known point for a short grace window before really releasing.
static const uint32_t TOUCH_RELEASE_GRACE_MS = 40;
static bool touch_down = false;
static uint32_t touch_last_seen_ms = 0;
static int16_t touch_last_x = 0, touch_last_y = 0;

static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint32_t now = millis();

    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        int32_t x = map(p.x, touchCal.rawXMin, touchCal.rawXMax, 0, SCREEN_W - 1);
        int32_t y = map(p.y, touchCal.rawYMin, touchCal.rawYMax, 0, SCREEN_H - 1);
        touch_last_x = constrain(x, 0, SCREEN_W - 1);
        touch_last_y = constrain(y, 0, SCREEN_H - 1);
        touch_last_seen_ms = now;
        touch_down = true;
    } else if (touch_down && (now - touch_last_seen_ms) >= TOUCH_RELEASE_GRACE_MS) {
        touch_down = false;
    }

    data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = touch_last_x;
    data->point.y = touch_last_y;
}

void display_init_panel() {
    tft.begin();
    tft.setRotation(0); // portrait, 240x320
}

void display_init_input() {
    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchSPI);
    touchscreen.setRotation(TOUCH_ROTATION);
    calibrate_touch();

    lv_init();
    // This lvgl build (9.2.2) ignores lv_conf.h's LV_TICK_CUSTOM macro -
    // tick source is wired at runtime instead.
    lv_tick_set_cb(millis);

    display = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(display, disp_flush_cb);
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read_cb);
}

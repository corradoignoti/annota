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

static const uint16_t SCREEN_W = 240;
static const uint16_t SCREEN_H = 320;

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

// -----------------------------------------------------------------------
// Mock MP3 catalog
//
// Placeholder until the SD card / internal flash (LittleFS) directory scan
// is wired in - same shape (filename + created timestamp) the real scan
// will need to produce.
// -----------------------------------------------------------------------

struct Mp3Entry {
    const char *filename;
    const char *created;
};

static const Mp3Entry MOCK_MP3_FILES[] = {
    {"session_intro.mp3",     "2026-08-12 09:41"},
    {"field_notes_01.mp3",    "2026-08-14 16:03"},
    {"interview_garcia.mp3",  "2026-08-15 11:27"},
    {"ambient_room_tone.mp3", "2026-08-17 07:55"},
    {"voice_memo_final.mp3",  "2026-08-19 20:12"},
    {"backup_take_02.mp3",    "2026-08-20 14:38"},
    {"walkthrough_draft.mp3", "2026-08-20 18:52"},
    {"outdoor_test_01.mp3",   "2026-08-21 07:10"},
    {"notes_to_self.mp3",     "2026-08-21 08:36"},
    {"final_mix_v3.mp3",      "2026-08-21 09:15"},
};
static const size_t MOCK_MP3_COUNT = sizeof(MOCK_MP3_FILES) / sizeof(MOCK_MP3_FILES[0]);

// -----------------------------------------------------------------------
// Main screen: title + scrollable list of rounded file cards
// -----------------------------------------------------------------------

static lv_style_t style_card;

static void build_main_screen() {
    lv_style_init(&style_card);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_bg_color(&style_card, lv_color_hex(0x2A2E3A));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 10);
    lv_style_set_pad_row(&style_card, 2);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14161C), 0);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 8, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MP3 Files");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (size_t i = 0; i < MOCK_MP3_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, MOCK_MP3_FILES[i].filename);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(name, lv_color_white(), 0);

        lv_obj_t *date = lv_label_create(card);
        lv_label_set_text(date, MOCK_MP3_FILES[i].created);
        lv_obj_set_style_text_font(date, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(date, lv_color_hex(0x9AA0AC), 0);
    }
}

// -----------------------------------------------------------------------

void setup() {
    Serial.begin(115200);

    tft.begin();
    tft.setRotation(0); // portrait, 240x320

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

    build_main_screen();
}

void loop() {
    // LV_TICK_CUSTOM=1 in lv_conf.h feeds lv_tick_get() from millis()
    // directly, no manual lv_tick_inc() needed here.
    lv_timer_handler();
    delay(5);
}

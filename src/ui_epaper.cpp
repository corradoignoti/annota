#include "ui.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <qrcode.h>  // ricmoo/QRCode - kWifiJoined/kWifiApActive's scannable QR codes

#include "display.h"
#include "fonts_it.h"
#include "i18n.h"
#include "reboot_combo.h"
#include "sleep.h"
#include "speaker.h"
#include "storage.h"
#include "transcribe.h"
#include "usb_drive.h"
#include "wifi_manager.h"

// -----------------------------------------------------------------------
// The on-device screen: WiFi status, a scrollable file list, and per-file
// Transcribe/Delete/View (.txt files only) - deliberately small in scope
// (no on-device Settings or WiFi credential entry - those stay on the
// existing web UI; see web_server.cpp). The panel is 200x200 mono with
// only 2 buttons and a slow (~1-2s) refresh, so this is a small explicit
// state machine driven by display.h's display_button_poll(), not a
// touch-driven widget tree:
//   Next (BOOT)   - cycle the current selection/menu option
//   Select (PWR)  - short press: open/confirm; long press: back out
// Every screen is rebuilt from scratch on each state change
// (lv_obj_clean() + repopulate) rather than kept as a tree of
// show/hide-toggled widgets - simpler to keep correct, and cheap next to
// the e-paper refresh itself dominating either way.
//
// Visual language: a black status bar pinned across the top (outside
// body, never cleared by render_body()), rounded bordered "cards" for
// every list/menu row (inverted black-on-white when selected - the only
// focus indicator this UI has, in place of touch highlighting), and a
// bordered info-card for every message-only screen. Every row/card gets
// a small leading icon from lvgl's built-in symbol font so screens read
// at a glance instead of as walls of plain text - all within mono 1bpp,
// no new image assets.
// -----------------------------------------------------------------------

enum class Screen {
    kNoCard,
    kList,
    kMainMenu,
    kActionMenu,
    kDeleteConfirm,
    kPlaying,
    kRecording,
    kMicError,
    kWifiSetup,
    kTranscribeProgress,
    kTranscribeResult,
    kSleeping,
    kWifiManage,
    kWifiApActive,
    kWifiJoined,
    kRebootConfirm,
    kTextView,
    kWifiScanning,
    kWifiJoinList,
    kHome,
    kDetails,
    kUsbDrive,
    kUsbDriveError,
    kUsbDriveRestarting,
    kVolumeMenu,
};

// -----------------------------------------------------------------------
// Per-board layout. Everything below that sizes or places something reads
// these instead of a literal, so the same screens scale from the 1.54's
// 200x200 panel to the 3.97's 480x800 portrait one (board.h's BOARD_EPAPER_*). The
// 1.54 column holds this file's original values, unchanged. Font roles:
// FONT_HINT (hint bar, QR captions), FONT_SMALL (status bar, subtitles,
// secondary lines), FONT_BODY (rows, titles, messages), FONT_ICON (an
// info card's big icon).
// -----------------------------------------------------------------------
#if BOARD_EPAPER_397
static const int16_t HEADER_H = 36;
static const int16_t ROW_H = 40;
static const int16_t HINT_H = 48; // fits add_hint()'s two wrapped lines
static const lv_font_t *const FONT_HINT = &lv_font_it_16;
static const lv_font_t *const FONT_SMALL = &lv_font_it_20;
static const lv_font_t *const FONT_BODY = &lv_font_it_24;
static const lv_font_t *const FONT_ICON = &lv_font_it_48;
static const int16_t PAD_X = 12;          // label inset inside a row/header/panel
static const int16_t LIST_X = 8;          // full-width rows' left/right margin
static const int16_t ROW_RADIUS = 6;
static const int16_t ROW_TITLE_DY = 3;    // two-slot row: filename line's top offset
static const int16_t ROW_INDENT = 46;     // two-slot row: subtitle indent, past icon + "  "
static const int16_t ROW_RULE_DY = 14;    // two-slot row: placeholder rule, below ROW_H
static const int16_t CARD_W = SCREEN_W - 40; // info/progress card width
static const int16_t CARD_PAD = 20;
static const int16_t CARD_BORDER = 3;
static const int16_t CARD_RADIUS = 12;
static const int16_t CARD_GAP = 12;       // between a card's stacked children
static const int16_t CARD_DY = -12;       // nudged above center, see add_info_card()
static const int16_t BAR_H = 28;
static const int16_t BAR_PAD = 5;
static const int16_t MENU_W = SCREEN_W - 32; // render_option_menu()'s panel width
static const int16_t MENU_TITLE_H = 36;
static const int16_t MENU_RULE_DY = 32;      // title underline, from the panel's top padding
static const int16_t MENU_PAD = 12;
static const int16_t MENU_PAD_COMPACT = 6;
static const int16_t MENU_MARGIN_Y = 24;
static const int16_t TEXT_PAD = 12;       // kTextView's viewport padding
static const int16_t STATUS_ICONS_W = 140; // header room kept for battery/SD icons
#else
static const int16_t HEADER_H = 20;
static const int16_t ROW_H = 20;
static const int16_t HINT_H = 30; // fits add_hint()'s two wrapped lines
static const lv_font_t *const FONT_HINT = &lv_font_it_10;
static const lv_font_t *const FONT_SMALL = &lv_font_it_12;
static const lv_font_t *const FONT_BODY = &lv_font_it_14;
static const lv_font_t *const FONT_ICON = &lv_font_it_28;
static const int16_t PAD_X = 6;
static const int16_t LIST_X = 4;
static const int16_t ROW_RADIUS = 4;
static const int16_t ROW_TITLE_DY = 1;
static const int16_t ROW_INDENT = 26;
static const int16_t ROW_RULE_DY = 7;
static const int16_t CARD_W = SCREEN_W - 24;
static const int16_t CARD_PAD = 10;
static const int16_t CARD_BORDER = 2;
static const int16_t CARD_RADIUS = 8;
static const int16_t CARD_GAP = 6;
static const int16_t CARD_DY = -8;
static const int16_t BAR_H = 14;
static const int16_t BAR_PAD = 3;
static const int16_t MENU_W = SCREEN_W - 16;
static const int16_t MENU_TITLE_H = 20;
static const int16_t MENU_RULE_DY = 14;
static const int16_t MENU_PAD = 6;
static const int16_t MENU_PAD_COMPACT = 3;
static const int16_t MENU_MARGIN_Y = 12;
static const int16_t TEXT_PAD = 4;
static const int16_t STATUS_ICONS_W = 60;
#endif
// kList reserves its own top row (below) for the Notes header,
// on top of HEADER_H/HINT_H.
static const int VISIBLE_ROWS = (SCREEN_H - HEADER_H - HINT_H - ROW_H) / ROW_H;

static lv_obj_t *header_label = nullptr;
static lv_obj_t *battery_label = nullptr;
static uint8_t battery_last_percent = 255; // sentinel - forces the first ui_set_battery_percent() paint
static lv_obj_t *body = nullptr;
static bool sd_present = false;
static Screen state = Screen::kNoCard;

// kList - the audio files (Notes) in mp3Files/mp3FileCount.
static size_t selected_index = 0;
static size_t top_index = 0;

// kList double-press-Select-to-jump-to-top gesture: a Select short press on
// any row but the first (Record) is held back rather than acting right
// away, in case a second one follows fast - if it does, that's the
// double-press gesture and selected_index just snaps to 0 (Record) with no
// action fired; if the window lapses with no second press, the held-back
// action (open kActionMenu for that row) fires late instead. A press on
// row 0 itself never needs this - jumping to where you already are is a
// no-op - so it still acts immediately, same as before this gesture existed.
static bool select_press_pending = false;
static uint32_t select_press_pending_since = 0;
static const uint32_t DOUBLE_PRESS_WINDOW_MS = 350;

// kActionMenu / kDeleteConfirm / kDetails - the file the menu/confirm was
// opened for, and which option is currently highlighted. active_file_index
// indexes mp3Files directly (Details reads created/size straight off it).
static char active_filename[64];
static size_t active_file_index = 0;
static int menu_index = 0;

#if BOARD_EPAPER_397
// kPlaying -> kVolumeMenu: holding rotary Up or Down this long while a
// track plays opens the volume menu. Timed off the raw pin state, since
// display_button_poll()'s own long press (700 ms) fires far too early.
static const uint32_t VOLUME_HOLD_MS = 3000;
static uint32_t volume_hold_since = 0; // 0 = neither Up nor Down held
#endif

// kActionMenu / kDeleteConfirm - an audio file's sibling "<basename>.txt"
// transcript, looked up when the action menu opens. If it exists, the menu
// offers View transcription in place of Transcribe, and Delete's confirm
// screen warns that it goes too (confirming deletes both).
static char active_transcript_name[64];
static bool active_has_transcript = false;

// kDetails - built once when Details is picked from kActionMenu (the audio
// length needs an SD read), so repaints don't touch the card again.
// details_compact - an audio file with a sibling transcript: twice the
// lines, so the card drops its icon and uses a smaller font to fit.
static char details_text[256];
static bool details_compact = false;

// kTextView - text_view_buffer holds the audio file's .txt transcript
// (read via read_text_file_preview() when View transcription is picked
// from kActionMenu), truncated
// to fit; transcripts are short speech-to-text output, comfortably under
// this size. text_view_scroll_px is the label's current negative y offset,
// clamped in render_body() to the label's actual laid-out height.
static char text_view_buffer[8192];
static int32_t text_view_scroll_px = 0;
static const int16_t TEXT_VIEW_SCROLL_STEP = 3 * ROW_H; // ~3-4 lines of FONT_BODY

// kWifiSetup
static char wifi_setup_ssid[64];

// kWifiApActive
static char wifi_ap_ip[16];

// kWifiJoined - just the URL; the sentence around it is formatted at
// render time (render_body()), so a language change repaints it too.
static char wifi_joined_url[64];

// kWifiApActive - STANDALONE_AP_SSID (wifi_manager.cpp) is a fixed literal
// with no password, so unlike wifi_joined_url above this needs no runtime
// buffer, just the WiFi-network-config QR payload format phones' camera
// apps recognize (T:nopass - an open network, so no P: field).
static const char *WIFI_AP_QR_DATA = "WIFI:T:nopass;S:Annota-AP;;";

// QR code rendering, shared by kWifiJoined (its http:// URL) and
// kWifiApActive (the AP's join string above, plus - 3.97 only - a second
// QR with the AP's http:// URL below it) - only one of the two screens is
// ever on screen at once, so they share the canvas backing buffers too. Version
// 4 (33x33 modules) at ECC_LOW gives 78 bytes of byte-mode capacity -
// comfortably more than either payload needs; add_qr() degrades to text-only (see its comment) if a payload
// ever doesn't fit. Scaled up 3px/module (99x99 canvas) onto an RGB565
// lv_canvas, drawn pixel-exact (no lv_image zoom/interpolation) since this
// display thresholds everything to 1bpp on flush and blurred edges would
// threshold unpredictably.
static const uint8_t WIFI_QR_VERSION = 4;
#if BOARD_EPAPER_397
static const uint8_t WIFI_QR_SCALE = 7;
#else
static const uint8_t WIFI_QR_SCALE = 3;
#endif
static const uint8_t WIFI_QR_MODULES = WIFI_QR_VERSION * 4 + 17;
static const uint16_t WIFI_QR_BUFFER_SIZE = (WIFI_QR_MODULES * WIFI_QR_MODULES + 7) / 8;
static const int16_t WIFI_QR_PX = WIFI_QR_MODULES * WIFI_QR_SCALE;
#if BOARD_EPAPER_397
// One buffer per QR on screen at once (kWifiApActive shows two). 231x231
// RGB565 is ~104 KB each - too much for internal DRAM (see
// display_epaper.cpp's draw_buf comment), so allocated in PSRAM on first use.
static const uint8_t WIFI_QR_SLOTS = 2;
static lv_color_t *wifi_qr_canvas_buf[WIFI_QR_SLOTS] = {};
#else
static const uint8_t WIFI_QR_SLOTS = 1;
static lv_color_t wifi_qr_canvas_buf[WIFI_QR_SLOTS][WIFI_QR_PX * WIFI_QR_PX];
#endif

// kTranscribeProgress / kTranscribeResult
static char transcribe_filename[64];
static char transcribe_phase[64];
static int transcribe_percent = -1; // upload bar fill, -1 = no bar
static char transcribe_detail[64];
static bool transcribe_ok = false;
static char transcribe_message[192];

static void render_body();

// tr() for text naming buttons: on knob boards (BOARD_HAS_KNOB - the 3.97's
// rotary Up/Down/Select + BOOT) the <ID>_K variant of the string, else the
// plain one (see i18n_strings.def).
static const char *knob_tr(Str plain, Str knob) {
    return tr(BOARD_HAS_KNOB ? knob : plain);
}

// Set by ui_request_rerender(), consumed by ui_process_input().
static bool rerender_requested = false;

// kList shows a synthetic "Record new" row pinned above the real files,
// kept as index 0 ahead of mp3Files rather than a separate widget/button
// so it reuses the same Next/Select navigation and clamp_selection() as
// every real row.

// Opens kActionMenu for whichever real file selected_index currently
// points at. Shared by kList's immediate Select-short path (row already at
// index 0's Record option doesn't use this) and the deferred path fired by
// the double-press-to-jump-to-top gesture's timeout - see
// select_press_pending's comment above.
static void open_action_menu_for_selected() {
    size_t fileIndex = selected_index - 1;
    active_file_index = fileIndex;
    strncpy(active_filename, mp3Files[fileIndex].filename, sizeof(active_filename) - 1);
    active_filename[sizeof(active_filename) - 1] = '\0';
    active_has_transcript = find_sibling_transcript(active_filename, active_transcript_name,
                                                    sizeof(active_transcript_name));
    menu_index = 0;
    state = Screen::kActionMenu;
    render_body();
}

static void format_size(uint32_t bytes, char *out, size_t outLen) {
    if (bytes < 1024) {
        snprintf(out, outLen, "%lu B", (unsigned long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, outLen, "%lu KB", (unsigned long)((bytes + 1023) / 1024));
    } else {
        unsigned long tenths = (unsigned long)(((uint64_t)bytes * 10 + 512 * 1024) / (1024 * 1024));
        snprintf(out, outLen, "%lu.%lu MB", tenths / 10, tenths % 10);
    }
}

// Fills details_text for kDetails from mp3Files[active_file_index]: name,
// size and playing time (read off the card - see storage.h's
// get_audio_duration_seconds()), plus the sibling transcript's size and
// date if it has one (active_has_transcript, looked up when the action
// menu opened).
static void build_details_text() {
    const Mp3Entry &entry = mp3Files[active_file_index];
    char size[16];
    format_size(entry.size, size, sizeof(size));
    details_compact = false;
    uint32_t secs = 0;
    char length[32];
    if (get_audio_duration_seconds(entry.filename, secs)) {
        snprintf(length, sizeof(length), tr(Str::DURATION_MIN_SEC), (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    } else {
        strncpy(length, tr(Str::UNKNOWN), sizeof(length) - 1);
        length[sizeof(length) - 1] = '\0';
    }
    uint32_t txtBytes = 0;
    char txtCreated[20];
    if (active_has_transcript &&
        get_file_info(active_transcript_name, txtBytes, txtCreated, sizeof(txtCreated))) {
        char txtSize[16];
        format_size(txtBytes, txtSize, sizeof(txtSize));
        snprintf(details_text, sizeof(details_text), tr(Str::DETAILS_AUDIO_TRANSCRIPT), entry.filename, size, length,
                 txtSize, txtCreated);
        details_compact = true;
        return;
    }
    snprintf(details_text, sizeof(details_text), tr(Str::DETAILS_AUDIO), entry.filename, size, length);
}

static size_t list_item_count() {
    return mp3FileCount + 1;
}

// kList row i's transcript title (Mp3Entry::title) - "" for an audio file
// with no titled transcript yet, so its row gets add_row()'s drawn-line
// placeholder and every audio row stays the same height. nullptr (single-
// line row) for the Record row.
static const char *list_item_title(size_t i) {
    if (i == 0) return nullptr;
    return mp3Files[i - 1].title;
}

// How many ROW_H slots kList row i takes: 2 when it has a title line
// (blank or not) under the filename, else 1. VISIBLE_ROWS counts slots,
// not rows.
static size_t list_item_slots(size_t i) {
    return list_item_title(i) ? 2 : 1;
}

static void clamp_selection() {
    size_t count = list_item_count();
    if (selected_index >= count) selected_index = count - 1;
    if (selected_index < top_index) top_index = selected_index;
    // Scroll down until top_index..selected_index fits in VISIBLE_ROWS slots.
    for (;;) {
        size_t used = 0;
        for (size_t i = top_index; i <= selected_index; i++) used += list_item_slots(i);
        if (used <= (size_t)VISIBLE_ROWS || top_index == selected_index) break;
        top_index++;
    }
}

// One rounded, bordered "card" row: a leading icon glyph plus label text,
// left-aligned, inverted (black bg, white text) when selected - the only
// "focus" indicator this UI has. parent/x/y/w let this serve both the
// full-width list (parent == body) and menu rows indented inside a
// bordered panel (see render_option_menu()). A non-null `subtitle` makes
// the card two slots tall (2 * ROW_H) with it on a second, smaller line
// under `text` - kList's transcript titles (see list_item_title()). An
// empty `subtitle` draws a short horizontal rule there instead.
static void add_row(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, const char *icon, const char *text, bool selected,
                    const char *subtitle = nullptr) {
    int16_t card_h = (subtitle ? 2 * ROW_H : ROW_H) - 2;
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, card_h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, ROW_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text_fmt(label, "%s  %s", icon, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, w - 2 * PAD_X);
    lv_obj_set_style_text_font(label, FONT_BODY, 0);
    lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), 0);
    if (!subtitle) {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, PAD_X, 0);
        return;
    }
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, PAD_X, ROW_TITLE_DY);

    // Indented to line up under `text`, past the icon and its two spaces.
    const int16_t indent = ROW_INDENT;
    if (!subtitle[0]) {
        // No title yet (untranscribed audio): a thin rule holds its place.
        lv_obj_t *rule = lv_obj_create(card);
        lv_obj_remove_style_all(rule);
        lv_obj_set_size(rule, (w - indent - PAD_X) * 2 / 3, 1);
        lv_obj_align(rule, LV_ALIGN_TOP_LEFT, indent, ROW_H + ROW_RULE_DY);
        lv_obj_set_style_bg_color(rule, selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        return;
    }
    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, subtitle);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, w - indent - PAD_X);
    lv_obj_set_style_text_font(sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, selected ? lv_color_white() : lv_color_black(), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, indent, ROW_H);
}

// A windowed list's top row: a leading icon, a label, and a bottom border
// separating it from the cards below - not a card itself (never
// selectable), so it's built directly rather than through add_row().
// `scrollable` - true once the list has more rows than VISIBLE_ROWS can
// show at once - draws a small down-arrow at the row's right edge (mirrors
// build_main_screen()'s right-aligned status icons) as the only hint that
// Next still reveals more: this list has no scrollbar, and Next wraps
// around rather than stopping at the last item (see ui_process_input()'s
// kList case), so the arrow stays fixed rather than tracking top_index/
// whether the view is currently at the bottom - there's always "more" to
// scroll to either way. Shared by kList and kWifiJoinList (its own icon/label).
static void render_list_header(const char *icon, const char *label_text, bool scrollable) {
    lv_obj_t *hdr = lv_obj_create(body);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, SCREEN_W, ROW_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_black(), 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(hdr);
    lv_label_set_text_fmt(label, "%s  %s", icon, label_text);
    lv_obj_set_style_text_font(label, FONT_BODY, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, PAD_X, -1);

    if (scrollable) {
        lv_obj_t *more = lv_label_create(hdr);
        lv_label_set_text(more, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_font(more, FONT_BODY, 0);
        lv_obj_set_style_text_color(more, lv_color_black(), 0);
        lv_obj_align(more, LV_ALIGN_RIGHT_MID, -PAD_X, -1);
    }
}

// Scannable QR codes, each with a wrapped caption below it, in a column
// filling body - used by kWifiJoined (its http:// URL) and kWifiApActive
// (the AP's WiFi-join string, plus its URL on the 3.97) instead of
// add_info_card()'s icon+text layout, since a QR code needs far more of
// body's limited space than a symbol-font glyph does. See WIFI_QR_* above
// for the encoding/rendering choices.
static lv_obj_t *add_qr_container() {
    lv_obj_t *cont = lv_obj_create(body);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H - HEADER_H - HINT_H);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 4, 0);
    return cont;
}

// One QR canvas plus its wrapped caption, appended to cont's column. slot
// picks the canvas backing buffer (< WIFI_QR_SLOTS) - each QR on screen at
// once needs its own, since LVGL only reads them at flush time.
static void add_qr(lv_obj_t *cont, uint8_t slot, const char *qr_data, const char *caption) {
    QRCode qr;
    uint8_t qr_bytes[WIFI_QR_BUFFER_SIZE];
#if BOARD_EPAPER_397
    if (!wifi_qr_canvas_buf[slot]) {
        wifi_qr_canvas_buf[slot] = (lv_color_t *)heap_caps_malloc(WIFI_QR_PX * WIFI_QR_PX * sizeof(lv_color_t),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (wifi_qr_canvas_buf[slot] && qrcode_initText(&qr, qr_bytes, WIFI_QR_VERSION, ECC_LOW, qr_data) == 0) {
#else
    if (qrcode_initText(&qr, qr_bytes, WIFI_QR_VERSION, ECC_LOW, qr_data) == 0) {
#endif
        lv_obj_t *canvas = lv_canvas_create(cont);
        lv_canvas_set_buffer(canvas, wifi_qr_canvas_buf[slot], WIFI_QR_PX, WIFI_QR_PX, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
        for (uint8_t my = 0; my < qr.size; my++) {
            for (uint8_t mx = 0; mx < qr.size; mx++) {
                if (!qrcode_getModule(&qr, mx, my)) continue;
                for (uint8_t py = 0; py < WIFI_QR_SCALE; py++) {
                    for (uint8_t px = 0; px < WIFI_QR_SCALE; px++) {
                        lv_canvas_set_px(canvas, mx * WIFI_QR_SCALE + px, my * WIFI_QR_SCALE + py,
                                         lv_color_black(), LV_OPA_COVER);
                    }
                }
            }
        }
    }

    lv_obj_t *msg = lv_label_create(cont);
    lv_label_set_text(msg, caption);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, SCREEN_W - 16);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, FONT_HINT, 0);
    lv_obj_set_style_text_color(msg, lv_color_black(), 0);
}

static void add_qr_screen(const char *qr_data, const char *caption) {
    add_qr(add_qr_container(), 0, qr_data, caption);
}

// A bordered, rounded card centered in body, with an optional big icon
// above a wrapped message - the info/dialog counterpart to add_row()'s
// list cards, used by every message-only screen below.
static void add_info_card(const char *icon, const char *text, const lv_font_t *font = FONT_BODY) {
    const int16_t pad = CARD_PAD;
    const int16_t card_w = CARD_W;

    lv_obj_t *card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, CARD_BORDER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_style_pad_row(card, CARD_GAP, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (icon && icon[0]) {
        lv_obj_t *icon_label = lv_label_create(card);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, FONT_ICON, 0);
        lv_obj_set_style_text_color(icon_label, lv_color_black(), 0);
    }

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, text);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, card_w - pad * 2);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, font, 0);
    lv_obj_set_style_text_color(msg, lv_color_black(), 0);

    lv_obj_align(card, LV_ALIGN_CENTER, 0, CARD_DY); // slightly above center, to balance against the hint bar below
}

// kTranscribeProgress's card: same frame as add_info_card(), holding the
// filename, the current phase line, an upload bar (only while
// transcribe_percent >= 0) and a small detail line. Bar is plain
// black-on-white with no radius/animation so the 1bpp threshold on flush
// stays crisp.
static void add_transcribe_progress_card() {
    const int16_t pad = CARD_PAD;
    const int16_t card_w = CARD_W;
    const int16_t inner_w = card_w - pad * 2;

    lv_obj_t *card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, CARD_BORDER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_style_pad_row(card, CARD_GAP, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // No big icon unlike add_info_card() - the bar and two-line phase/
    // detail text need that vertical room between header and hint bar.
    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, transcribe_filename);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, inner_w);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, FONT_SMALL, 0);
    lv_obj_set_style_text_color(name, lv_color_black(), 0);

    lv_obj_t *phase = lv_label_create(card);
    lv_label_set_text(phase, transcribe_phase);
    lv_label_set_long_mode(phase, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(phase, inner_w);
    lv_obj_set_style_text_align(phase, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(phase, FONT_BODY, 0);
    lv_obj_set_style_text_color(phase, lv_color_black(), 0);

    if (transcribe_percent >= 0) {
        lv_obj_t *bar = lv_bar_create(card);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, inner_w, BAR_H);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, CARD_BORDER, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, BAR_PAD, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, transcribe_percent, LV_ANIM_OFF);
    }

    if (transcribe_detail[0]) {
        lv_obj_t *detail = lv_label_create(card);
        lv_label_set_text(detail, transcribe_detail);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detail, inner_w);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(detail, FONT_SMALL, 0);
        lv_obj_set_style_text_color(detail, lv_color_black(), 0);
    }

    lv_obj_align(card, LV_ALIGN_CENTER, 0, CARD_DY);
}

// Wraps onto up to two lines instead of running off the panel edge -
// callers keep hint text short enough to fit HINT_H at that wrap width.
// The top border marks it off as a distinct status strip rather than
// trailing text.
static void add_hint(const char *text) {
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCREEN_W, HINT_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_black(), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(bar);
    lv_label_set_text(hint, text);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, SCREEN_W - 8);
    lv_obj_set_style_text_font(hint, FONT_HINT, 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 2);
}

// Shared by kActionMenu and kDeleteConfirm - a bordered panel holding a
// title line plus a cycle-and-confirm option list, one icon+label card
// per option (see add_row()). `note`, if given, is a wrapped line of text
// between the title and the options (kDeleteConfirm's transcript warning).
static void render_option_menu(const char *title, const char *const *icons, const char *const *options, int count,
                               const char *note = nullptr) {
    const int16_t title_h = MENU_TITLE_H;
    const int16_t panel_w = MENU_W;
    int16_t note_h = 0;
    if (note) {
        lv_point_t size;
        lv_text_get_size(&size, note, FONT_SMALL, 0, 0, panel_w - 2 * PAD_X, LV_TEXT_FLAG_NONE);
        note_h = (int16_t)size.y + 4;
    }
    // Tighten the padding and drop the top margin when the usual layout
    // would run into the hint bar (the audio action menu's 6 rows).
    const int16_t avail_h = SCREEN_H - HEADER_H - HINT_H;
    const bool compact = MENU_MARGIN_Y + MENU_PAD * 2 + title_h + note_h + count * ROW_H > avail_h;
    const int16_t pad = compact ? MENU_PAD_COMPACT : MENU_PAD;
    const int16_t panel_h = pad * 2 + title_h + note_h + count * ROW_H;
    const int16_t panel_y = compact ? 0 : MENU_MARGIN_Y;

    lv_obj_t *panel = lv_obj_create(body);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, CARD_BORDER, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_radius(panel, CARD_RADIUS, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(panel);
    lv_label_set_text(title_label, title);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title_label, panel_w - 2 * PAD_X);
    lv_obj_set_style_text_font(title_label, FONT_BODY, 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title_label, lv_color_black(), 0);
    lv_obj_set_pos(title_label, PAD_X, pad);

    lv_obj_t *rule = lv_obj_create(panel);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, panel_w - 2 * PAD_X, 1);
    lv_obj_set_pos(rule, PAD_X, pad + MENU_RULE_DY);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    int16_t y = pad + title_h;
    if (note) {
        lv_obj_t *note_label = lv_label_create(panel);
        lv_label_set_text(note_label, note);
        lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note_label, panel_w - 2 * PAD_X);
        lv_obj_set_style_text_font(note_label, FONT_SMALL, 0);
        lv_obj_set_style_text_align(note_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(note_label, lv_color_black(), 0);
        lv_obj_set_pos(note_label, PAD_X, y);
        y += note_h;
    }
    for (int i = 0; i < count; i++) {
        add_row(panel, PAD_X, y, panel_w - 2 * PAD_X, icons[i], options[i], i == menu_index);
        y += ROW_H;
    }

    add_hint(knob_tr(Str::HINT_MENU, Str::HINT_MENU_K));
}

static void render_body() {
    lv_obj_clean(body);

    switch (state) {
        case Screen::kNoCard:
            add_info_card(LV_SYMBOL_SD_CARD, tr(Str::NO_CARD));
            break;

        case Screen::kList: {
            size_t count = list_item_count();
            char header_label[48];
            snprintf(header_label, sizeof(header_label), "%s (%u)",
                     tr(Str::LIST_AUDIO_FILES),
                     (unsigned)mp3FileCount);
            size_t total_slots = 0;
            for (size_t i = 0; i < count; i++) total_slots += list_item_slots(i);
            render_list_header(LV_SYMBOL_AUDIO, header_label,
                                total_slots > (size_t)VISIBLE_ROWS);
            clamp_selection();
            int16_t y = ROW_H;
            size_t used_slots = 0;
            for (size_t i = top_index; i < count; i++) {
                size_t slots = list_item_slots(i);
                if (used_slots + slots > (size_t)VISIBLE_ROWS) break;
                const char *label = i == 0 ? tr(Str::LIST_RECORD_NEW) : mp3Files[i - 1].filename;
                const char *icon = i == 0 ? LV_SYMBOL_PLUS : LV_SYMBOL_AUDIO;
                add_row(body, LIST_X, y, SCREEN_W - 2 * LIST_X, icon, label, i == selected_index, list_item_title(i));
                y += slots * ROW_H;
                used_slots += slots;
            }
            add_hint(knob_tr(Str::HINT_LIST, Str::HINT_LIST_K));
            break;
        }

        // Reached from kList via a long Next press (see
        // ui_process_input()'s kList case) - a horizontal carousel of 3
        // cells, one full-screen icon+label card shown at a time
        // (add_info_card(), same helper kNoCard/kMicError/etc. use),
        // paged by Next; the hint bar's "(n/3)" is the position indicator
        // (this font has no page-dot glyphs baked in, so text stays the
        // safe choice - same idiom as "Audio Files (N)" above). Select
        // confirms: Notes opens kList on the audio files; File
        // transfer hands off to wifi_manager.h's request/process split
        // (see ui_process_input()'s kHome case for why state isn't
        // touched here for that branch); USB drive hands the card to a
        // USB host (usb_drive.h) and shows kUsbDrive.
        case Screen::kHome: {
            static const char *icons[] = {LV_SYMBOL_AUDIO, LV_SYMBOL_UPLOAD, LV_SYMBOL_USB};
            const char *labels[] = {tr(Str::HOME_NOTES), tr(Str::FILE_TRANSFER), tr(Str::USB_DRIVE)};
            render_list_header(LV_SYMBOL_HOME, tr(Str::HOME_TITLE), false);
            add_info_card(icons[menu_index], labels[menu_index]);
            char hint[96];
            snprintf(hint, sizeof(hint), knob_tr(Str::HINT_HOME, Str::HINT_HOME_K), (int)menu_index + 1);
            add_hint(hint);
            break;
        }

        // A long Select press from kList opens this - refresh the file
        // list from SD, flip WiFi online/offline (label tracks live
        // status via wifi_is_connected(), not a state variable here - see
        // wifi_manager.h), ask to reboot, or back out with no action.
        // icons/options are plain locals, not `static`, since the
        // Offline/Online label changes with live WiFi status on every
        // render.
        case Screen::kMainMenu: {
            bool online = wifi_is_connected();
            const char *icons[] = {LV_SYMBOL_REFRESH, LV_SYMBOL_WIFI, LV_SYMBOL_POWER, LV_SYMBOL_CLOSE};
            const char *options[] = {tr(Str::MENU_REFRESH), tr(online ? Str::MENU_OFFLINE : Str::MENU_ONLINE),
                                     tr(Str::MENU_REBOOT), tr(Str::MENU_CLOSE)};
            render_option_menu(tr(Str::MENU_TITLE), icons, options, 4);
            break;
        }

        case Screen::kRebootConfirm: {
            static const char *icons[] = {LV_SYMBOL_POWER, LV_SYMBOL_CLOSE};
            const char *options[] = {tr(Str::REBOOT_CONFIRM), tr(Str::CANCEL)};
            render_option_menu(tr(Str::REBOOT_TITLE), icons, options, 2);
            break;
        }

        case Screen::kActionMenu: {
            // An audio file that already has a transcript gets View
            // transcription in Transcribe's slot.
            const char *icons[] = {LV_SYMBOL_PLAY,
                                   active_has_transcript ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EDIT,
                                   LV_SYMBOL_LIST,
                                   LV_SYMBOL_TRASH,
                                   LV_SYMBOL_CLOSE};
            const char *options[] = {tr(Str::ACTION_PLAY),
                                     active_has_transcript ? tr(Str::ACTION_VIEW_TRANSCRIPT)
                                                           : tr(Str::ACTION_TRANSCRIBE),
                                     tr(Str::ACTION_DETAILS),
                                     tr(Str::ACTION_DELETE),
                                     tr(Str::CANCEL)};
            render_option_menu(active_filename, icons, options, 5);
            break;
        }

        case Screen::kTextView: {
            const int16_t viewport_h = SCREEN_H - HEADER_H - HINT_H;
            lv_obj_t *viewport = lv_obj_create(body);
            lv_obj_remove_style_all(viewport);
            lv_obj_set_size(viewport, SCREEN_W, viewport_h);
            lv_obj_set_pos(viewport, 0, 0);
            lv_obj_set_style_pad_all(viewport, TEXT_PAD, 0);
            lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *label = lv_label_create(viewport);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(label, SCREEN_W - 2 * TEXT_PAD);
            lv_label_set_text(label, text_view_buffer);
            lv_obj_set_style_text_font(label, FONT_BODY, 0);
            lv_obj_set_style_text_color(label, lv_color_black(), 0);

            // Clip via a fixed-height parent (LVGL's default child-clip
            // behavior) with the label's y offset doing the "scrolling" -
            // same reasoning as kList's manual pagination: nothing in this
            // file uses lv_obj_scroll*(). Height is only known after
            // layout, so it's read back to clamp before the final position.
            lv_obj_update_layout(label);
            int32_t max_scroll = lv_obj_get_height(label) - (viewport_h - 2 * TEXT_PAD);
            if (max_scroll < 0) max_scroll = 0;
            if (text_view_scroll_px > max_scroll) text_view_scroll_px = max_scroll;
            lv_obj_set_y(label, -text_view_scroll_px);

            add_hint(knob_tr(Str::HINT_TEXT_VIEW, Str::HINT_TEXT_VIEW_K));
            break;
        }

        case Screen::kDetails:
            if (details_compact) {
                add_info_card(nullptr, details_text, FONT_SMALL);
            } else {
                add_info_card(LV_SYMBOL_LIST, details_text);
            }
            add_hint(tr(Str::HINT_SELECT_BACK));
            break;

        case Screen::kDeleteConfirm: {
            char title[128];
            snprintf(title, sizeof(title), tr(Str::DELETE_TITLE), active_filename);
            static const char *icons[] = {LV_SYMBOL_TRASH, LV_SYMBOL_CLOSE};
            if (active_has_transcript) {
                char warning[128];
                snprintf(warning, sizeof(warning), tr(Str::DELETE_ALSO_TRANSCRIPT), active_transcript_name);
                const char *options[] = {tr(Str::DELETE_CONFIRM_WITH_TEXT), tr(Str::CANCEL)};
                render_option_menu(title, icons, options, 2, warning);
            } else {
                const char *options[] = {tr(Str::DELETE_CONFIRM), tr(Str::CANCEL)};
                render_option_menu(title, icons, options, 2);
            }
            break;
        }

        // Full-screen, not render_option_menu()'s small floating panel -
        // only two items, so the extra room reads as a dedicated screen
        // rather than a transient menu. Header bar follows
        // render_list_header()'s look (border-bottom title row); rows are
        // the same add_row() cards kList's own rows use, full body width.
        case Screen::kWifiManage: {
            lv_obj_t *hdr = lv_obj_create(body);
            lv_obj_remove_style_all(hdr);
            lv_obj_set_size(hdr, SCREEN_W, ROW_H);
            lv_obj_set_pos(hdr, 0, 0);
            lv_obj_set_style_border_width(hdr, 1, 0);
            lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(hdr, lv_color_black(), 0);
            lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *hdr_label = lv_label_create(hdr);
            lv_label_set_text_fmt(hdr_label, LV_SYMBOL_WIFI "  %s", tr(Str::WIFI_TITLE));
            lv_obj_set_style_text_font(hdr_label, FONT_BODY, 0);
            lv_obj_set_style_text_color(hdr_label, lv_color_black(), 0);
            lv_obj_align(hdr_label, LV_ALIGN_LEFT_MID, PAD_X, -1);

            static const char *icons[] = {LV_SYMBOL_WIFI, LV_SYMBOL_UPLOAD};
            const char *options[] = {tr(Str::WIFI_JOIN_AP), tr(Str::WIFI_CREATE_AP)};
            int16_t y = ROW_H;
            for (int i = 0; i < 2; i++) {
                add_row(body, LIST_X, y, SCREEN_W - 2 * LIST_X, icons[i], options[i], i == menu_index);
                y += ROW_H;
            }
            add_hint(knob_tr(Str::HINT_MENU, Str::HINT_MENU_K));
            break;
        }

        case Screen::kWifiApActive: {
#if BOARD_EPAPER_397
            // Room for both steps on the tall panel: join the AP, then
            // open the web UI - so the second scan needs no typing.
            char url[32];
            snprintf(url, sizeof(url), "http://%s", wifi_ap_ip);
            char open_msg[128];
            snprintf(open_msg, sizeof(open_msg), tr(Str::WIFI_AP_QR_OPEN), url);
            lv_obj_t *cont = add_qr_container();
            lv_obj_set_style_pad_row(cont, CARD_GAP, 0);
            add_qr(cont, 0, WIFI_AP_QR_DATA, tr(Str::WIFI_AP_QR_JOIN));
            add_qr(cont, 1, url, open_msg);
#else
            char msg[160];
            snprintf(msg, sizeof(msg), tr(Str::WIFI_AP_MSG), wifi_ap_ip);
            add_qr_screen(WIFI_AP_QR_DATA, msg);
#endif
            add_hint(tr(Str::HINT_STOP_AP));
            break;
        }

        case Screen::kWifiJoined: {
            char msg[192];
            snprintf(msg, sizeof(msg), tr(Str::WIFI_JOINED_MSG), wifi_joined_url);
            add_qr_screen(wifi_joined_url, msg);
            add_hint(tr(Str::HINT_CLOSE_WIFI_OFF));
            break;
        }

        case Screen::kWifiScanning:
            add_info_card(LV_SYMBOL_WIFI, tr(Str::WIFI_SCANNING));
            break;

        case Screen::kWifiJoinList: {
            int count = wifi_scan_in_range_count();
            if (count == 0) {
                add_info_card(LV_SYMBOL_WARNING, tr(Str::WIFI_NO_KNOWN));
                add_hint(tr(Str::HINT_SELECT_CLOSE));
                break;
            }
            // Shares selected_index/top_index with kList - the two screens
            // are never shown at the same time, same as kActionMenu sharing
            // menu_index with kWifiManage.
            if (selected_index >= (size_t)count) selected_index = count - 1;
            if (selected_index < top_index) top_index = selected_index;
            if (selected_index >= top_index + VISIBLE_ROWS) top_index = selected_index - VISIBLE_ROWS + 1;

            char header_label[48];
            snprintf(header_label, sizeof(header_label), tr(Str::WIFI_NETWORKS), (unsigned)count);
            render_list_header(LV_SYMBOL_WIFI, header_label, count > VISIBLE_ROWS);
            int16_t y = ROW_H;
            for (size_t i = top_index; i < (size_t)count && (i - top_index) < (size_t)VISIBLE_ROWS; i++) {
                char ssid[WIFI_SSID_MAX_LEN + 1];
                wifi_scan_get_in_range_ssid(i, ssid, sizeof(ssid));
                add_row(body, LIST_X, y, SCREEN_W - 2 * LIST_X, LV_SYMBOL_WIFI, ssid, i == selected_index);
                y += ROW_H;
            }
            add_hint(transcribe_is_waiting_for_wifi() ? knob_tr(Str::HINT_JOIN_CANCEL, Str::HINT_JOIN_CANCEL_K)
                                                     : knob_tr(Str::HINT_JOIN_BACK, Str::HINT_JOIN_BACK_K));
            break;
        }

        case Screen::kPlaying: {
            char msg[128];
            snprintf(msg, sizeof(msg), tr(Str::PLAYING), active_filename);
            add_info_card(LV_SYMBOL_PLAY, msg);
            add_hint(knob_tr(Str::HINT_SELECT_STOP, Str::HINT_SELECT_STOP_K));
            break;
        }

        case Screen::kVolumeMenu: {
            static const char *icons[] = {LV_SYMBOL_VOLUME_MID, LV_SYMBOL_VOLUME_MID, LV_SYMBOL_VOLUME_MAX};
            const char *options[] = {tr(Str::VOLUME_LOW), tr(Str::VOLUME_MEDIUM), tr(Str::VOLUME_HIGH)};
            render_option_menu(tr(Str::VOLUME_TITLE), icons, options, 3);
            break;
        }

        case Screen::kRecording: {
            char msg[128];
            snprintf(msg, sizeof(msg), tr(Str::RECORDING), active_filename);
            add_info_card(LV_SYMBOL_AUDIO, msg);
            add_hint(tr(Str::HINT_SELECT_STOP));
            break;
        }

        case Screen::kMicError:
            add_info_card(LV_SYMBOL_WARNING, mic_last_error());
            add_hint(tr(Str::HINT_SELECT_CLOSE));
            break;

        // USB drive mode (usb_drive.h) - the SD card belongs to the USB
        // host until it ejects the drive, the cable is pulled, or the
        // user cancels; every one of those ends in a reboot
        // (kUsbDriveRestarting is the last thing painted before it).
        case Screen::kUsbDrive:
            render_list_header(LV_SYMBOL_USB, tr(Str::USB_DRIVE), false);
            add_info_card(LV_SYMBOL_USB, tr(Str::USB_CONNECTED));
            add_hint(knob_tr(Str::HINT_USB, Str::HINT_USB_K));
            break;

        case Screen::kUsbDriveError:
            add_info_card(LV_SYMBOL_WARNING, tr(Str::USB_ERROR));
            add_hint(tr(Str::HINT_SELECT_CLOSE));
            break;

        case Screen::kUsbDriveRestarting:
            add_info_card(LV_SYMBOL_REFRESH, tr(Str::USB_RESTARTING));
            break;

        case Screen::kWifiSetup: {
            char msg[192];
            snprintf(msg, sizeof(msg), tr(Str::WIFI_SETUP_MSG), wifi_setup_ssid);
            add_info_card(LV_SYMBOL_WIFI, msg);
            add_hint(knob_tr(Str::HINT_WORK_OFFLINE, Str::HINT_WORK_OFFLINE_K));
            break;
        }

        case Screen::kTranscribeProgress:
            add_transcribe_progress_card();
            add_hint(tr(Str::HINT_PLEASE_WAIT));
            break;

        case Screen::kTranscribeResult:
            add_info_card(transcribe_ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, transcribe_message);
            add_hint(tr(Str::HINT_SELECT_CLOSE));
            break;

        case Screen::kSleeping:
            add_info_card(LV_SYMBOL_POWER, knob_tr(Str::SLEEPING, Str::SLEEPING_K));
            break;
    }
}

void build_main_screen(bool sdPresent) {
    sd_present = sdPresent;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // A black status bar pinned across the top, outside body - never
    // touched by render_body()'s lv_obj_clean(), so WiFi status survives
    // every screen change.
    lv_obj_t *header_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(header_bar);
    lv_obj_set_size(header_bar, SCREEN_W, HEADER_H);
    lv_obj_set_pos(header_bar, 0, 0);
    lv_obj_set_style_bg_color(header_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header_bar, LV_OBJ_FLAG_SCROLLABLE);

    header_label = lv_label_create(header_bar);
    lv_obj_set_style_text_font(header_label, FONT_SMALL, 0);
    lv_obj_set_style_text_color(header_label, lv_color_white(), 0);
    lv_label_set_long_mode(header_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(header_label, "");
    lv_obj_align(header_label, LV_ALIGN_LEFT_MID, PAD_X, 0);

    // Right-side status icons: battery percentage (always) plus the SD
    // card icon (only if present) - grouped in one flex-row container so
    // battery text width (1-3 digits) doesn't need manual offset math
    // against the SD icon next to it. Main-axis START, not END: the
    // container's LV_SIZE_CONTENT width is measured from its children's
    // current positions, so END pushes a label that just grew (empty ->
    // "<icon> 100%") to a negative x, the container never widens, and the
    // label's left part - the battery icon - is clipped. The container
    // itself is right-aligned below, so START still hugs the right edge.
    lv_obj_t *status_icons = lv_obj_create(header_bar);
    lv_obj_remove_style_all(status_icons);
    lv_obj_set_size(status_icons, LV_SIZE_CONTENT, HEADER_H);
    lv_obj_set_style_bg_opa(status_icons, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(status_icons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_icons, 4, 0);
    lv_obj_align(status_icons, LV_ALIGN_RIGHT_MID, -PAD_X, 0);

    battery_label = lv_label_create(status_icons);
    lv_obj_set_style_text_font(battery_label, FONT_SMALL, 0);
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_label_set_text(battery_label, ""); // filled in by ui_set_battery_percent()
    battery_last_percent = 255;           // force the next ui_set_battery_percent() call to repaint

    if (sdPresent) {
        lv_obj_t *sd_icon = lv_label_create(status_icons);
        lv_label_set_text(sd_icon, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_font(sd_icon, FONT_SMALL, 0);
        lv_obj_set_style_text_color(sd_icon, lv_color_white(), 0);
    }

    lv_obj_set_width(header_label, SCREEN_W - STATUS_ICONS_W); // leaves room for status_icons

    body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, SCREEN_W, SCREEN_H - HEADER_H);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    selected_index = 0;
    top_index = 0;
    state = sd_present ? Screen::kList : Screen::kNoCard;
    render_body();
}

void ui_set_wifi_status(const char *text) {
    if (!header_label) return;
    lv_label_set_text(header_label, text);
    lv_timer_handler();
}

void ui_set_battery_percent(uint8_t percent) {
    if (!battery_label) return;
    if (percent > 100) percent = 100;
    if (percent == battery_last_percent) return; // unchanged - skip the full e-paper repaint
    battery_last_percent = percent;
    const char *icon = percent >= 90   ? LV_SYMBOL_BATTERY_FULL
                        : percent >= 60 ? LV_SYMBOL_BATTERY_3
                        : percent >= 40 ? LV_SYMBOL_BATTERY_2
                        : percent >= 15 ? LV_SYMBOL_BATTERY_1
                                        : LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(battery_label, "%s %u%%", icon, (unsigned)percent);
    lv_timer_handler();
}

void ui_show_wifi_setup_dialog(const char *setup_ssid) {
    if (state == Screen::kWifiSetup) return; // already shown - see ui.h's contract
    strncpy(wifi_setup_ssid, setup_ssid, sizeof(wifi_setup_ssid) - 1);
    wifi_setup_ssid[sizeof(wifi_setup_ssid) - 1] = '\0';
    state = Screen::kWifiSetup;
    render_body();
    lv_timer_handler();
}

void ui_hide_wifi_setup_dialog() {
    if (state != Screen::kWifiSetup) return;
    state = sd_present ? Screen::kList : Screen::kNoCard;
    render_body();
    lv_timer_handler();
}

// No Settings view here to refresh a retry button on - see ui.h's comment.
void ui_refresh_wifi_retry_button() {}

// Guards against a caller racing ahead of reality (e.g. wifi_ensure_connected()'s
// fast WiFi.status()-already-true path, which returns without touching the
// header) - without this check the header could still read a stale
// "working offline" from an earlier failed attempt while this QR claims a
// live connection. Falls back to kWifiManage (with the header status
// blanked, like wifi_go_offline()) rather than trusting the caller's ip.
void ui_show_wifi_joined_screen(const char *ip) {
    if (!wifi_is_connected()) {
        ui_set_wifi_status("");
        ui_show_wifi_manage_screen();
        return;
    }
    char status[64];
    snprintf(status, sizeof(status), LV_SYMBOL_WIFI " %s", ip);
    ui_set_wifi_status(status);
    snprintf(wifi_joined_url, sizeof(wifi_joined_url), "http://%s", ip);
    state = Screen::kWifiJoined;
    render_body();
    lv_timer_handler();
}

void ui_show_wifi_manage_screen() {
    state = Screen::kWifiManage;
    menu_index = 0;
    render_body();
    lv_timer_handler();
}

void ui_show_transcribe_progress(const char *filename) {
    strncpy(transcribe_filename, filename, sizeof(transcribe_filename) - 1);
    transcribe_filename[sizeof(transcribe_filename) - 1] = '\0';
    strncpy(transcribe_phase, tr(Str::TRANSCRIBE_STARTING), sizeof(transcribe_phase) - 1);
    transcribe_phase[sizeof(transcribe_phase) - 1] = '\0';
    transcribe_percent = -1;
    transcribe_detail[0] = '\0';
    state = Screen::kTranscribeProgress;
    render_body();
    lv_timer_handler();
}

void ui_show_wifi_join_for_transcribe() {
    // Straight to the scan rather than kWifiManage's Join/Create menu - a
    // standalone AP has no internet, so it's no use for transcribing.
    wifi_start_scan();
    state = Screen::kWifiScanning;
    render_body();
    lv_timer_handler();
}

void ui_update_transcribe_progress(const char *phaseLabel, int percent, const char *detail) {
    strncpy(transcribe_phase, phaseLabel, sizeof(transcribe_phase) - 1);
    transcribe_phase[sizeof(transcribe_phase) - 1] = '\0';
    transcribe_percent = percent;
    strncpy(transcribe_detail, detail, sizeof(transcribe_detail) - 1);
    transcribe_detail[sizeof(transcribe_detail) - 1] = '\0';
    state = Screen::kTranscribeProgress;
    render_body();
    // Called repeatedly mid-upload, often well inside LVGL's refresh
    // period since the last repaint - lv_timer_handler() alone would skip
    // the redraw then, so force it.
    lv_refr_now(NULL);
}

void ui_show_transcribe_result(bool ok, const char *message) {
    transcribe_ok = ok;
    strncpy(transcribe_message, message, sizeof(transcribe_message) - 1);
    transcribe_message[sizeof(transcribe_message) - 1] = '\0';
    state = Screen::kTranscribeResult;
    render_body();
    lv_timer_handler();
}

bool ui_is_sleep_blocked() {
    return state == Screen::kRecording || state == Screen::kPlaying || state == Screen::kVolumeMenu ||
           state == Screen::kTranscribeProgress ||
           state == Screen::kWifiApActive || state == Screen::kWifiJoined ||
           state == Screen::kUsbDrive;
}

void ui_show_usb_drive_restarting() {
    state = Screen::kUsbDriveRestarting;
    render_body();
    lv_timer_handler();
}

void ui_request_rerender() {
    rerender_requested = true;
}

void ui_show_sleep_screen() {
    state = Screen::kSleeping;
    render_body();
    lv_timer_handler();
}

void ui_process_input() {
    // Cheap no-ops when nothing's playing/recording (see speaker.h) -
    // called unconditionally so playback/recording keeps pumping every
    // loop() iteration, not just on a button edge like everything below.
    speaker_process();
    mic_process();
    if (rerender_requested) {
        rerender_requested = false;
        render_body();
    }
    if (state == Screen::kWifiScanning && wifi_scan_status() != WifiScanStatus::kRunning) {
        // Same unconditional-pump idiom as speaker_process()/mic_process()
        // above - wifi_scan_status() is a cheap, non-blocking check, safe
        // to call every loop() iteration while waiting.
        selected_index = 0;
        top_index = 0;
        state = Screen::kWifiJoinList;
        render_body();
    }
    if (state == Screen::kPlaying && !speaker_is_playing()) {
        // Track ended on its own (no Select press involved) - leave the
        // Playing screen the same way Select does.
        state = sd_present ? Screen::kList : Screen::kNoCard;
        render_body();
    }
    if (state == Screen::kRecording && !mic_is_recording()) {
        // mic_process() force-stopped on its own (I2S read error) - same
        // idea as the speaker_is_playing() check above, but recording
        // failing mid-way is worth surfacing rather than just dropping
        // back to the list silently.
        state = Screen::kMicError;
        render_body();
    }

    // Both buttons held = the "hold both to reboot" gesture, handled
    // entirely by reboot_combo.h's own task - skip the per-button poll
    // while it's in progress, see display_button_raw_pressed()'s comment.
    // (Knob boards: Select + Back, see reboot_combo.h.)
#if BOARD_HAS_KNOB
    if (display_button_raw_pressed(DisplayButton::kBack) && display_button_raw_pressed(DisplayButton::kSelect)) {
        return;
    }
#else
    if (display_button_raw_pressed(DisplayButton::kNext) && display_button_raw_pressed(DisplayButton::kSelect)) {
        return;
    }
#endif

    DisplayButtonEvent nextEv = display_button_poll(DisplayButton::kNext);
    DisplayButtonEvent selEv = display_button_poll(DisplayButton::kSelect);
    // kPrev/kBack only exist on knob boards (the 3.97's rotary Up and
    // BOOT) - always kNone elsewhere, so every branch below that reads
    // them is inert on the 1.54. prevEv short steps the selection back one
    // (the mirror of Next). Back short is, everywhere but kList, exactly a
    // long Select (every screen's existing "back out" path) - so it's
    // folded into selEv right here rather than handled per screen; kList
    // (the root screen, where a long Select opens the menu instead) maps
    // it to Home below.
    DisplayButtonEvent prevEv = display_button_poll(DisplayButton::kPrev);
    DisplayButtonEvent backEv = display_button_poll(DisplayButton::kBack);
    bool backToHome = false;
    if (backEv == DisplayButtonEvent::kShort && selEv == DisplayButtonEvent::kNone) {
        if (state == Screen::kList) {
            backToHome = true;
        } else {
            selEv = DisplayButtonEvent::kLong;
        }
    }

    // A held-back Select-short from kList (see select_press_pending's
    // comment) needs to fire even on a tick with no new button edge at
    // all, once its double-press window has lapsed - so this check runs
    // ahead of the "nothing happened" early return below.
    if (select_press_pending && state == Screen::kList &&
        millis() - select_press_pending_since >= DOUBLE_PRESS_WINDOW_MS) {
        select_press_pending = false;
        open_action_menu_for_selected();
    }

#if BOARD_EPAPER_397
    // Up/Down held VOLUME_HOLD_MS during playback opens the volume menu -
    // ahead of the early return below, since a hold produces no new edge.
    // The poll above already spent this press's long event (ignored on
    // kPlaying), so its release fires nothing on the menu.
    if (state == Screen::kPlaying &&
        (display_button_raw_pressed(DisplayButton::kNext) || display_button_raw_pressed(DisplayButton::kPrev))) {
        if (volume_hold_since == 0) {
            volume_hold_since = millis() | 1; // never 0 while held
        } else if (millis() - volume_hold_since >= VOLUME_HOLD_MS) {
            volume_hold_since = 0;
            menu_index = (int)speaker_get_volume_level();
            state = Screen::kVolumeMenu;
            sleep_reset_activity();
            render_body();
            return;
        }
    } else {
        volume_hold_since = 0;
    }
#endif

    if (nextEv == DisplayButtonEvent::kNone && selEv == DisplayButtonEvent::kNone &&
        prevEv == DisplayButtonEvent::kNone && backEv == DisplayButtonEvent::kNone) {
        return;
    }
    sleep_reset_activity(); // any button edge counts as activity - see sleep.h

    switch (state) {
        case Screen::kNoCard:
            break; // nothing to navigate - insert a card and reboot

        case Screen::kList:
            if (nextEv == DisplayButtonEvent::kLong || backToHome) {
                select_press_pending = false; // leaving kList's row set - drop any held-back press
                menu_index = 0;
                state = Screen::kHome;
                render_body();
                break;
            }
            if (prevEv == DisplayButtonEvent::kLong) {
                // Knob boards' jump-to-top (Record row) - their stand-in
                // for the 1.54's double-press-Select gesture.
                selected_index = 0;
                render_body();
                break;
            }
            if (selEv == DisplayButtonEvent::kLong) {
                // Refresh is one of the menu's own options, so no
                // separate rescan-on-long-press branch is needed below.
                select_press_pending = false; // leaving kList - drop any held-back press
                state = Screen::kMainMenu;
                menu_index = 0;
                render_body();
                break;
            }
            {
                size_t count = list_item_count();
                if (nextEv == DisplayButtonEvent::kShort || prevEv == DisplayButtonEvent::kShort) {
                    // Scrolling means this isn't a double-press-Select
                    // gesture in progress - fire the held-back press's
                    // action now rather than let it fire late after the
                    // selection has already moved on. That action leaves
                    // kList entirely (opens kActionMenu), so this Next
                    // press is consumed by it rather than also scrolling.
                    if (select_press_pending) {
                        select_press_pending = false;
                        open_action_menu_for_selected();
                        break;
                    }
                    if (nextEv == DisplayButtonEvent::kShort) {
                        selected_index = (selected_index + 1) % count;
                    } else {
                        selected_index = (selected_index + count - 1) % count;
                    }
                    render_body();
                } else if (selEv == DisplayButtonEvent::kShort) {
                    if (selected_index == 0) {
                        // Already on the Record row - jumping here would be
                        // a no-op, so act immediately, same as always.
                        char filename[64];
                        if (mic_start_recording(filename, sizeof(filename))) {
                            strncpy(active_filename, filename, sizeof(active_filename) - 1);
                            active_filename[sizeof(active_filename) - 1] = '\0';
                            state = Screen::kRecording;
                        } else {
                            // mic_last_error() has the reason - shown on
                            // screen since there's normally no serial
                            // monitor attached to see it logged there.
                            state = Screen::kMicError;
                        }
                        render_body();
                    } else if (BOARD_HAS_KNOB) {
                        // Knob boards act right away - no double-press
                        // window to wait out (Up held jumps to the top
                        // instead, see below), so Select feels instant.
                        open_action_menu_for_selected();
                    } else if (select_press_pending) {
                        // Second Select short-press within the window -
                        // double-press gesture: jump to the Record row
                        // instead of opening this row's action menu.
                        select_press_pending = false;
                        selected_index = 0;
                        render_body();
                    } else {
                        // First press on a non-Record row - hold it back
                        // in case a second one follows fast (see the
                        // pending-timeout check above, near the top of
                        // ui_process_input()).
                        select_press_pending = true;
                        select_press_pending_since = millis();
                    }
                }
            }
            break;

        case Screen::kHome:
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 3;
                render_body();
            } else if (prevEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 2) % 3;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    load_mp3_catalog();
                    selected_index = 0;
                    top_index = 0;
                    state = Screen::kList;
                    render_body();
                } else if (menu_index == 1) {
                    // Don't touch state/render here - wifi_manager.h's
                    // wifi_process_pending_file_transfer() (called right
                    // after this, same loop() iteration - see main.cpp)
                    // shows its own status/result screens, same reasoning
                    // as kActionMenu's Transcribe case above.
                    wifi_request_file_transfer();
                } else {
                    // The USB host writes raw sectors behind FATFS's back
                    // from here on - take WiFi (and with it the web file
                    // manager's SD handlers) out of the picture first.
                    if (wifi_is_connected()) {
                        wifi_go_offline();
                    }
                    state = usb_drive_start() ? Screen::kUsbDrive : Screen::kUsbDriveError;
                    render_body();
                }
            }
            break;

        // Refresh / Offline↔Online / Close - opened by a long Select press
        // from kList (see above). Offline/Online reuses the exact same
        // wifi_manager.h calls the web UI's Settings page drives
        // (wifi_go_offline() / wifi_request_reconnect()) - the latter is
        // non-blocking, consumed right after this by loop()'s
        // wifi_process_pending_reconnect(), same reasoning as the
        // Transcribe case below.
        case Screen::kMainMenu: {
            const int optionCount = 4;
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % optionCount;
                render_body();
            } else if (prevEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + optionCount - 1) % optionCount;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    load_mp3_catalog();
                    selected_index = 0;
                    top_index = 0;
                    state = Screen::kList;
                } else if (menu_index == 1) {
                    if (wifi_is_connected()) {
                        wifi_go_offline();
                    } else {
                        wifi_request_reconnect();
                    }
                    state = Screen::kList;
                } else if (menu_index == 2) {
                    state = Screen::kRebootConfirm;
                    menu_index = 0;
                } else {
                    // menu_index == 3 (Close): no action.
                    state = Screen::kList;
                }
                render_body();
            }
            break;
        }

        // Reboot needs its own confirm - unlike Refresh/Offline-Online,
        // it's disruptive enough (drops whatever's on screen, same as a
        // power cycle) to warrant the same guard as Delete/WiFi-management
        // below rather than firing straight off the menu row.
        case Screen::kRebootConfirm:
            if (nextEv == DisplayButtonEvent::kShort || prevEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    // Never returns - no state/render needed after.
                    reboot_now();
                }
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

        case Screen::kActionMenu: {
            // Option count/order tracks render_body()'s kActionMenu case:
            // {Play, Transcribe or View transcription, Details, Delete,
            // Cancel}.
            const int optionCount = 5;
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % optionCount;
                render_body();
            } else if (prevEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + optionCount - 1) % optionCount;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    speaker_play(active_filename);
                    state = Screen::kPlaying;
                    render_body();
                } else if (menu_index == 1 && active_has_transcript) {
                    read_text_file_preview(active_transcript_name, text_view_buffer, sizeof(text_view_buffer));
                    text_view_scroll_px = 0;
                    state = Screen::kTextView;
                    render_body();
                } else if (menu_index == 1 &&
                           mp3Files[active_file_index].size > TRANSCRIBE_MAX_FILE_BYTES) {
                    // Too big for the ESP32's own upload - never even
                    // queue it (see TRANSCRIBE_MAX_FILE_BYTES).
                    char msg[192];
                    snprintf(msg, sizeof(msg), tr(Str::TRANSCRIBE_TOO_LARGE),
                             TRANSCRIBE_MAX_FILE_BYTES / (1024.0 * 1024.0));
                    ui_show_transcribe_result(false, msg);
                } else if (menu_index == 1) {
                    // Don't touch state/render here - transcribe.h's
                    // transcribe_process_pending() (called right after
                    // this, from the same loop() iteration - see
                    // main.cpp) shows its own progress/result screens via
                    // ui_show_transcribe_progress()/ui_show_transcribe_result()
                    // moments from now, so redrawing the list first here
                    // would just be a wasted extra full-panel refresh.
                    transcribe_request(active_filename);
                } else if (menu_index == 2) {
                    build_details_text();
                    state = Screen::kDetails;
                    render_body();
                } else if (menu_index == 3) {
                    state = Screen::kDeleteConfirm;
                    menu_index = 0;
                    render_body();
                } else {
                    state = Screen::kList;
                    render_body();
                }
            }
            break;
        }

        case Screen::kDetails:
            // Back to the menu it was opened from, Details still highlighted.
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = Screen::kActionMenu;
                render_body();
            }
            break;

        case Screen::kDeleteConfirm:
            if (nextEv == DisplayButtonEvent::kShort || prevEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    if (delete_file(active_filename) && active_has_transcript) {
                        delete_file(active_transcript_name);
                    }
                    load_mp3_catalog();
                    selected_index = 0;
                    top_index = 0;
                }
                state = Screen::kList;
                render_body();
            }
            break;

        case Screen::kPlaying:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                speaker_stop();
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

#if BOARD_EPAPER_397
        // Playback keeps running underneath. Up/Down move the highlight;
        // Select (click) switches the playing track to that level right
        // away and saves it, staying in the menu so another level can be
        // tried. A back-out (long Select / BOOT) closes it - to kPlaying,
        // or to the list if the track ended meanwhile.
        case Screen::kVolumeMenu: {
            const int optionCount = (int)VolumeLevel::kCount;
            if (nextEv == DisplayButtonEvent::kShort || prevEv == DisplayButtonEvent::kShort) {
                menu_index = nextEv == DisplayButtonEvent::kShort ? (menu_index + 1) % optionCount
                                                                  : (menu_index + optionCount - 1) % optionCount;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                speaker_set_volume_level((VolumeLevel)menu_index, true);
            } else if (selEv == DisplayButtonEvent::kLong) {
                if (speaker_is_playing()) {
                    state = Screen::kPlaying;
                } else {
                    state = sd_present ? Screen::kList : Screen::kNoCard;
                }
                render_body();
            }
            break;
        }
#else
        case Screen::kVolumeMenu:
            break; // 3.97 only - never entered here
#endif

        case Screen::kRecording:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                mic_stop_recording();
                // Refresh so the just-finished recording shows up in the
                // list right away, same reason Delete re-scans below.
                load_mp3_catalog();
                selected_index = 0;
                top_index = 0;
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

        case Screen::kMicError:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

        case Screen::kUsbDrive:
            if (selEv == DisplayButtonEvent::kLong) {
                usb_drive_request_exit(); // main.cpp's usb_drive_process() reboots
            }
            break;

        case Screen::kUsbDriveError:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = Screen::kHome;
                render_body();
            }
            break;

        case Screen::kUsbDriveRestarting:
            break; // about to reboot

        case Screen::kWifiSetup:
            break; // informational only - see ui.h's contract

        case Screen::kTranscribeProgress:
            break; // informational only, until transcribe_process_pending() replaces it

        case Screen::kTranscribeResult:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                // Re-scan so the new transcript's title shows under its
                // audio file on kList (Mp3Entry::title).
                if (transcribe_ok && sd_present) load_mp3_catalog();
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

        case Screen::kTextView:
            if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort || (BOARD_HAS_KNOB && nextEv == DisplayButtonEvent::kShort)) {
                // Knob boards: Down (Next) scrolls down, Up (Prev) up -
                // Select still scrolls down too, same as on the 1.54.
                text_view_scroll_px += TEXT_VIEW_SCROLL_STEP;
                render_body(); // clamps to content height itself
            } else if ((!BOARD_HAS_KNOB && nextEv == DisplayButtonEvent::kShort) ||
                       prevEv == DisplayButtonEvent::kShort) {
                text_view_scroll_px -= TEXT_VIEW_SCROLL_STEP;
                if (text_view_scroll_px < 0) text_view_scroll_px = 0;
                render_body();
            }
            break;

        case Screen::kSleeping:
            break; // device deep-sleeps right after showing this - never reached

        case Screen::kWifiManage:
            if (nextEv == DisplayButtonEvent::kShort || prevEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    // Non-blocking (WiFi.scanNetworks(true) returns right
                    // away, the scan itself runs in the background) - safe
                    // to call directly here, same reasoning as
                    // wifi_start_standalone_ap() below. ui_process_input()'s
                    // per-tick poll above picks up completion and moves on
                    // to kWifiJoinList.
                    wifi_start_scan();
                    state = Screen::kWifiScanning;
                } else {
                    // Non-blocking, safe to call directly here - shows
                    // the AP's IP on kWifiApActive instead of falling
                    // back to kList.
                    wifi_start_standalone_ap(wifi_ap_ip, sizeof(wifi_ap_ip));
                    state = Screen::kWifiApActive;
                }
                render_body();
            }
            break;

        case Screen::kWifiScanning:
            break; // ui_process_input()'s per-tick poll above moves this along, not a button press

        case Screen::kWifiJoinList: {
            int count = wifi_scan_in_range_count();
            // Opened for a parked transcription (ui_show_wifi_join_for_transcribe())
            // rather than from kWifiManage - backing out abandons the
            // transcription and returns to the file list instead.
            bool forTranscribe = transcribe_is_waiting_for_wifi();
            if (count == 0) {
                if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                    if (forTranscribe) {
                        transcribe_cancel_wifi_wait();
                        state = sd_present ? Screen::kList : Screen::kNoCard;
                    } else {
                        state = Screen::kWifiManage;
                        menu_index = 0;
                    }
                    render_body();
                }
                break;
            }
            if (nextEv == DisplayButtonEvent::kShort) {
                selected_index = (selected_index + 1) % count;
                render_body();
            } else if (prevEv == DisplayButtonEvent::kShort) {
                selected_index = (selected_index + count - 1) % count;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                // "Previous screen" = the menu this list was opened from,
                // same one-level-back convention as every other screen here.
                if (forTranscribe) {
                    transcribe_cancel_wifi_wait();
                    state = sd_present ? Screen::kList : Screen::kNoCard;
                } else {
                    state = Screen::kWifiManage;
                    menu_index = 0;
                }
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                char ssid[WIFI_SSID_MAX_LEN + 1];
                wifi_scan_get_in_range_ssid(selected_index, ssid, sizeof(ssid));
                // wifi_process_pending_join() (loop(), after
                // lv_timer_handler()) does the actual blocking connect and
                // paints its own status - same reasoning as the reconnect/
                // setup-portal request/process splits elsewhere in this file.
                wifi_request_join_network(ssid);
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;
        }

        case Screen::kWifiApActive:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                wifi_go_offline();
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

        case Screen::kWifiJoined:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                wifi_go_offline();
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;
    }
}

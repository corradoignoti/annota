#include "ui.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
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
    kFileTransfer,
    kRebootConfirm,
    kTextView,
    kWifiScanning,
    kWifiJoinList,
    kHome,
    kDetails,
    kUsbDrive,
    kUsbDriveError,
    kUsbDriveRestarting,
};

static const int16_t HEADER_H = 20;
static const int16_t ROW_H = 20;
static const int16_t HINT_H = 30; // fits add_hint()'s two wrapped lines
// kList reserves its own top row (below) for the Audio/Text mode header,
// on top of HEADER_H/HINT_H.
static const int VISIBLE_ROWS = (SCREEN_H - HEADER_H - HINT_H - ROW_H) / ROW_H;

static lv_obj_t *header_label = nullptr;
static lv_obj_t *battery_label = nullptr;
static uint8_t battery_last_percent = 255; // sentinel - forces the first ui_set_battery_percent() paint
static lv_obj_t *body = nullptr;
static bool sd_present = false;
static Screen state = Screen::kNoCard;

// kList. showing_audio_files: true while mp3Files/mp3FileCount hold
// AUDIO_EXTS, false while showing .txt - set by picking Audio/Text on
// kHome (see ui_process_input()'s kHome case; reached from kList via a
// long Next press).
static bool showing_audio_files = true;
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

// kDetails - built once when Details is picked from kActionMenu (the audio
// length needs an SD read), so repaints don't touch the card again.
static char details_text[192];

// kTextView - text_view_buffer holds the .txt file's content (read via
// read_text_file_preview() when View is picked from kActionMenu), truncated
// to fit; transcripts are short speech-to-text output, comfortably under
// this size. text_view_scroll_px is the label's current negative y offset,
// clamped in render_body() to the label's actual laid-out height.
static char text_view_buffer[8192];
static int32_t text_view_scroll_px = 0;
static const int16_t TEXT_VIEW_SCROLL_STEP = 60; // ~3-4 lines at 14pt

// kWifiSetup
static char wifi_setup_ssid[64];

// kWifiApActive
static char wifi_ap_ip[16];

// kWifiJoined - just the URL; the sentence around it is formatted at
// render time (render_body()), so a language change repaints it too.
static char wifi_joined_url[64];

// kFileTransfer - the per-file counterpart to kWifiJoined above. url is
// sized for the worst case (a fully percent-encoded active_filename, see
// url_encode_component()) even though the QR code itself can't hold that
// much - add_qr_screen() degrades to text-only in that case (see its
// comment), and the message label below shows the plain filename either
// way, word-wrapped (formatted at render time, like kWifiJoined's).
static char file_transfer_name[64];
static char file_transfer_url[256];

// kWifiApActive - STANDALONE_AP_SSID (wifi_manager.cpp) is a fixed literal
// with no password, so unlike wifi_joined_url above this needs no runtime
// buffer, just the WiFi-network-config QR payload format phones' camera
// apps recognize (T:nopass - an open network, so no P: field).
static const char *WIFI_AP_QR_DATA = "WIFI:T:nopass;S:Annota-AP;;";

// QR code rendering, shared by kWifiJoined (its http:// URL),
// kWifiApActive (the AP's join string above), and kFileTransfer (a
// file-specific download URL, longer than either of the other two) - only
// one of the three is ever on screen at once, so they share one canvas
// backing buffer too. Version 4 (33x33 modules) at ECC_LOW gives 78 bytes
// of byte-mode capacity - comfortably more than the AP/joined payloads
// need, and enough for a download URL with a short-to-moderate filename;
// add_qr_screen() degrades to text-only (see its comment) if a payload
// ever doesn't fit. Scaled up 3px/module (99x99 canvas) onto an RGB565
// lv_canvas, drawn pixel-exact (no lv_image zoom/interpolation) since this
// display thresholds everything to 1bpp on flush and blurred edges would
// threshold unpredictably.
static const uint8_t WIFI_QR_VERSION = 4;
static const uint8_t WIFI_QR_SCALE = 3;
static const uint8_t WIFI_QR_MODULES = WIFI_QR_VERSION * 4 + 17;
static const uint16_t WIFI_QR_BUFFER_SIZE = (WIFI_QR_MODULES * WIFI_QR_MODULES + 7) / 8;
static const int16_t WIFI_QR_PX = WIFI_QR_MODULES * WIFI_QR_SCALE;
static lv_color_t wifi_qr_canvas_buf[WIFI_QR_PX * WIFI_QR_PX];

// kTranscribeProgress / kTranscribeResult
static char transcribe_filename[64];
static char transcribe_phase[64];
static int transcribe_percent = -1; // upload bar fill, -1 = no bar
static char transcribe_detail[64];
static bool transcribe_ok = false;
static char transcribe_message[192];

static void render_body();

// Set by ui_request_rerender(), consumed by ui_process_input().
static bool rerender_requested = false;

// kList shows a synthetic "Record new" row pinned above the real files -
// but only while showing_audio_files (a recording is itself an audio
// file; there's nothing to record onto the .txt transcript list), same
// rule the Transcribe entry below follows. Kept as index 0 ahead of
// mp3Files rather than a separate widget/button so it reuses the same
// Next/Select navigation and clamp_selection() as every real row.
static bool has_record_option() {
    return showing_audio_files;
}

// Opens kActionMenu for whichever real file selected_index currently
// points at. Shared by kList's immediate Select-short path (row already at
// index 0's Record option doesn't use this) and the deferred path fired by
// the double-press-to-jump-to-top gesture's timeout - see
// select_press_pending's comment above.
static void open_action_menu_for_selected() {
    size_t fileIndex = has_record_option() ? selected_index - 1 : selected_index;
    active_file_index = fileIndex;
    strncpy(active_filename, mp3Files[fileIndex].filename, sizeof(active_filename) - 1);
    active_filename[sizeof(active_filename) - 1] = '\0';
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

// Fills details_text for kDetails from mp3Files[active_file_index]: name +
// size for both lists, then the creation date for .txt or the playing time
// (read off the card - see storage.h's get_audio_duration_seconds()) for
// audio.
static void build_details_text() {
    const Mp3Entry &entry = mp3Files[active_file_index];
    char size[16];
    format_size(entry.size, size, sizeof(size));
    if (!showing_audio_files) {
        snprintf(details_text, sizeof(details_text), tr(Str::DETAILS_TEXT), entry.filename, size, entry.created);
        return;
    }
    uint32_t secs = 0;
    char length[32];
    if (get_audio_duration_seconds(entry.filename, secs)) {
        snprintf(length, sizeof(length), tr(Str::DURATION_MIN_SEC), (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    } else {
        strncpy(length, tr(Str::UNKNOWN), sizeof(length) - 1);
        length[sizeof(length) - 1] = '\0';
    }
    snprintf(details_text, sizeof(details_text), tr(Str::DETAILS_AUDIO), entry.filename, size, length);
}

static size_t list_item_count() {
    return has_record_option() ? mp3FileCount + 1 : mp3FileCount;
}

static void clamp_selection() {
    size_t count = list_item_count();
    if (count == 0) {
        selected_index = 0;
        top_index = 0;
        return;
    }
    if (selected_index >= count) selected_index = count - 1;
    if (selected_index < top_index) top_index = selected_index;
    if (selected_index >= top_index + VISIBLE_ROWS) top_index = selected_index - VISIBLE_ROWS + 1;
}

// One rounded, bordered "card" row: a leading icon glyph plus label text,
// left-aligned, inverted (black bg, white text) when selected - the only
// "focus" indicator this UI has. parent/x/y/w let this serve both the
// full-width list (parent == body) and menu rows indented inside a
// bordered panel (see render_option_menu()).
static void add_row(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, const char *icon, const char *text, bool selected) {
    int16_t card_h = ROW_H - 2;
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, card_h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text_fmt(label, "%s  %s", icon, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, w - 12);
    lv_obj_set_style_text_font(label, &lv_font_it_14, 0);
    lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, 0);
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
// scroll to either way. Shared by kList (icon/label built from
// showing_audio_files) and kWifiJoinList (its own icon/label).
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
    lv_obj_set_style_text_font(label, &lv_font_it_14, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, -1);

    if (scrollable) {
        lv_obj_t *more = lv_label_create(hdr);
        lv_label_set_text(more, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_font(more, &lv_font_it_14, 0);
        lv_obj_set_style_text_color(more, lv_color_black(), 0);
        lv_obj_align(more, LV_ALIGN_RIGHT_MID, -6, -1);
    }
}

// Percent-encodes src into out (RFC 3986 unreserved characters -
// letters/digits/'-'/'_'/'.'/'~' - passed through as-is, everything else
// as %XX), same rule web_server.cpp's own JS applies via
// encodeURIComponent() before hitting /api/download. Needed because
// ui_show_file_transfer_screen() below builds that same URL on-device, and
// ESP32 WebServer::arg() decodes the query string automatically - so the
// two sides already agree with no server-side change. Truncates (rather
// than overflowing) if out is too small; SD filenames this app deals with
// are root-only and mostly need no encoding at all (see sanitize_name() in
// web_server.cpp), so this only matters for edge-case characters like
// spaces.
static void url_encode_component(const char *src, char *out, size_t outLen) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < outLen; i++) {
        unsigned char c = (unsigned char)src[i];
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out[o++] = (char)c;
        } else if (o + 3 < outLen) {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        } else {
            break;
        }
    }
    out[o] = '\0';
}

// A scannable QR code plus a wrapped caption below it, filling body - used
// by kWifiJoined (its http:// URL), kWifiApActive (the AP's WiFi-join
// string), and kFileTransfer (a file's download URL) instead of
// add_info_card()'s icon+text layout, since a QR code needs far more of
// body's limited space than a symbol-font glyph does. See WIFI_QR_* above
// for the encoding/rendering choices.
static void add_qr_screen(const char *qr_data, const char *caption) {
    lv_obj_t *cont = lv_obj_create(body);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H - HEADER_H - HINT_H);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 4, 0);

    QRCode qr;
    uint8_t qr_bytes[WIFI_QR_BUFFER_SIZE];
    if (qrcode_initText(&qr, qr_bytes, WIFI_QR_VERSION, ECC_LOW, qr_data) == 0) {
        lv_obj_t *canvas = lv_canvas_create(cont);
        lv_canvas_set_buffer(canvas, wifi_qr_canvas_buf, WIFI_QR_PX, WIFI_QR_PX, LV_COLOR_FORMAT_RGB565);
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
    lv_obj_set_style_text_font(msg, &lv_font_it_10, 0);
    lv_obj_set_style_text_color(msg, lv_color_black(), 0);
}

// A bordered, rounded card centered in body, with an optional big icon
// above a wrapped message - the info/dialog counterpart to add_row()'s
// list cards, used by every message-only screen below.
static void add_info_card(const char *icon, const char *text) {
    const int16_t pad = 10;
    const int16_t card_w = SCREEN_W - 24;

    lv_obj_t *card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (icon && icon[0]) {
        lv_obj_t *icon_label = lv_label_create(card);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, &lv_font_it_28, 0);
        lv_obj_set_style_text_color(icon_label, lv_color_black(), 0);
    }

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, text);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, card_w - pad * 2);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, &lv_font_it_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_black(), 0);

    lv_obj_align(card, LV_ALIGN_CENTER, 0, -8); // slightly above center, to balance against the hint bar below
}

// kTranscribeProgress's card: same frame as add_info_card(), holding the
// filename, the current phase line, an upload bar (only while
// transcribe_percent >= 0) and a small detail line. Bar is plain
// black-on-white with no radius/animation so the 1bpp threshold on flush
// stays crisp.
static void add_transcribe_progress_card() {
    const int16_t pad = 10;
    const int16_t card_w = SCREEN_W - 24;
    const int16_t inner_w = card_w - pad * 2;

    lv_obj_t *card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
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
    lv_obj_set_style_text_font(name, &lv_font_it_12, 0);
    lv_obj_set_style_text_color(name, lv_color_black(), 0);

    lv_obj_t *phase = lv_label_create(card);
    lv_label_set_text(phase, transcribe_phase);
    lv_label_set_long_mode(phase, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(phase, inner_w);
    lv_obj_set_style_text_align(phase, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(phase, &lv_font_it_14, 0);
    lv_obj_set_style_text_color(phase, lv_color_black(), 0);

    if (transcribe_percent >= 0) {
        lv_obj_t *bar = lv_bar_create(card);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, inner_w, 14);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
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
        lv_obj_set_style_text_font(detail, &lv_font_it_12, 0);
        lv_obj_set_style_text_color(detail, lv_color_black(), 0);
    }

    lv_obj_align(card, LV_ALIGN_CENTER, 0, -8);
}

// Wraps onto up to two lines instead of running off the 200px panel edge -
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
    lv_obj_set_style_text_font(hint, &lv_font_it_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 2);
}

// Shared by kActionMenu and kDeleteConfirm - a bordered panel holding a
// title line plus a cycle-and-confirm option list, one icon+label card
// per option (see add_row()).
static void render_option_menu(const char *title, const char *const *icons, const char *const *options, int count) {
    const int16_t title_h = 20;
    const int16_t panel_w = SCREEN_W - 16;
    // Tighten the padding and drop the top margin when the usual layout
    // would run into the hint bar (the audio action menu's 6 rows).
    const int16_t avail_h = SCREEN_H - HEADER_H - HINT_H;
    const bool compact = 12 + 6 * 2 + title_h + count * ROW_H > avail_h;
    const int16_t pad = compact ? 3 : 6;
    const int16_t panel_h = pad * 2 + title_h + count * ROW_H;
    const int16_t panel_y = compact ? 0 : 12;

    lv_obj_t *panel = lv_obj_create(body);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(panel);
    lv_label_set_text(title_label, title);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title_label, panel_w - 12);
    lv_obj_set_style_text_font(title_label, &lv_font_it_14, 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title_label, lv_color_black(), 0);
    lv_obj_set_pos(title_label, 6, pad);

    lv_obj_t *rule = lv_obj_create(panel);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, panel_w - 12, 1);
    lv_obj_set_pos(rule, 6, pad + title_h - 6);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    int16_t y = pad + title_h;
    for (int i = 0; i < count; i++) {
        add_row(panel, 6, y, panel_w - 12, icons[i], options[i], i == menu_index);
        y += ROW_H;
    }

    add_hint(tr(Str::HINT_MENU));
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
                     tr(showing_audio_files ? Str::LIST_AUDIO_FILES : Str::LIST_TEXT_FILES),
                     (unsigned)mp3FileCount);
            render_list_header(showing_audio_files ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE, header_label,
                                count > (size_t)VISIBLE_ROWS);
            if (count == 0) {
                add_info_card(showing_audio_files ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE,
                               tr(showing_audio_files ? Str::LIST_NO_AUDIO : Str::LIST_NO_TEXT));
                add_hint(tr(Str::HINT_LIST_EMPTY));
                break;
            }
            clamp_selection();
            bool recordOption = has_record_option();
            int16_t y = ROW_H;
            for (size_t i = top_index; i < count && (i - top_index) < (size_t)VISIBLE_ROWS; i++) {
                const char *label = (recordOption && i == 0) ? tr(Str::LIST_RECORD_NEW) : mp3Files[recordOption ? i - 1 : i].filename;
                const char *icon = (recordOption && i == 0) ? LV_SYMBOL_PLUS : (showing_audio_files ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE);
                add_row(body, 4, y, SCREEN_W - 8, icon, label, i == selected_index);
                y += ROW_H;
            }
            add_hint(tr(Str::HINT_LIST));
            break;
        }

        // Reached from kList via a long Next press (see
        // ui_process_input()'s kList case) - a horizontal carousel of 4
        // cells, one full-screen icon+label card shown at a time
        // (add_info_card(), same helper kNoCard/kMicError/etc. use),
        // paged by Next; the hint bar's "(n/4)" is the position indicator
        // (this font has no page-dot glyphs baked in, so text stays the
        // safe choice - same idiom as "Audio Files (N)" above). Select
        // confirms: Audio/Text set showing_audio_files and open kList
        // (what the old long-Next toggle used to do directly); File
        // transfer hands off to wifi_manager.h's request/process split
        // (see ui_process_input()'s kHome case for why state isn't
        // touched here for that branch); USB drive hands the card to a
        // USB host (usb_drive.h) and shows kUsbDrive.
        case Screen::kHome: {
            static const char *icons[] = {LV_SYMBOL_AUDIO, LV_SYMBOL_FILE, LV_SYMBOL_UPLOAD, LV_SYMBOL_USB};
            const char *labels[] = {tr(Str::HOME_AUDIO), tr(Str::HOME_TEXT), tr(Str::FILE_TRANSFER),
                                    tr(Str::USB_DRIVE)};
            render_list_header(LV_SYMBOL_HOME, tr(Str::HOME_TITLE), false);
            add_info_card(icons[menu_index], labels[menu_index]);
            char hint[96];
            snprintf(hint, sizeof(hint), tr(Str::HINT_HOME), (int)menu_index + 1);
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
            // Play/Transcription only make sense for audio files, not the
            // .txt transcripts this same list shows when toggled. Details
            // (see kDetails) and File transfer (per-file download link +
            // QR, see kFileTransfer) apply to both.
            if (showing_audio_files) {
                static const char *icons[] = {LV_SYMBOL_PLAY, LV_SYMBOL_EDIT,   LV_SYMBOL_LIST,
                                               LV_SYMBOL_TRASH, LV_SYMBOL_UPLOAD, LV_SYMBOL_CLOSE};
                const char *options[] = {tr(Str::ACTION_PLAY),   tr(Str::ACTION_TRANSCRIBE), tr(Str::ACTION_DETAILS),
                                         tr(Str::ACTION_DELETE), tr(Str::FILE_TRANSFER),     tr(Str::CANCEL)};
                render_option_menu(active_filename, icons, options, 6);
            } else {
                static const char *icons[] = {LV_SYMBOL_EYE_OPEN, LV_SYMBOL_LIST, LV_SYMBOL_TRASH, LV_SYMBOL_UPLOAD,
                                               LV_SYMBOL_CLOSE};
                const char *options[] = {tr(Str::ACTION_VIEW), tr(Str::ACTION_DETAILS), tr(Str::ACTION_DELETE),
                                         tr(Str::FILE_TRANSFER), tr(Str::CANCEL)};
                render_option_menu(active_filename, icons, options, 5);
            }
            break;
        }

        case Screen::kTextView: {
            const int16_t viewport_h = SCREEN_H - HEADER_H - HINT_H;
            lv_obj_t *viewport = lv_obj_create(body);
            lv_obj_remove_style_all(viewport);
            lv_obj_set_size(viewport, SCREEN_W, viewport_h);
            lv_obj_set_pos(viewport, 0, 0);
            lv_obj_set_style_pad_all(viewport, 4, 0);
            lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *label = lv_label_create(viewport);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(label, SCREEN_W - 8);
            lv_label_set_text(label, text_view_buffer);
            lv_obj_set_style_text_font(label, &lv_font_it_14, 0);
            lv_obj_set_style_text_color(label, lv_color_black(), 0);

            // Clip via a fixed-height parent (LVGL's default child-clip
            // behavior) with the label's y offset doing the "scrolling" -
            // same reasoning as kList's manual pagination: nothing in this
            // file uses lv_obj_scroll*(). Height is only known after
            // layout, so it's read back to clamp before the final position.
            lv_obj_update_layout(label);
            int32_t max_scroll = lv_obj_get_height(label) - (viewport_h - 8);
            if (max_scroll < 0) max_scroll = 0;
            if (text_view_scroll_px > max_scroll) text_view_scroll_px = max_scroll;
            lv_obj_set_y(label, -text_view_scroll_px);

            add_hint(tr(Str::HINT_TEXT_VIEW));
            break;
        }

        case Screen::kDetails:
            add_info_card(LV_SYMBOL_LIST, details_text);
            add_hint(tr(Str::HINT_SELECT_BACK));
            break;

        case Screen::kDeleteConfirm: {
            char title[128];
            snprintf(title, sizeof(title), tr(Str::DELETE_TITLE), active_filename);
            static const char *icons[] = {LV_SYMBOL_TRASH, LV_SYMBOL_CLOSE};
            const char *options[] = {tr(Str::DELETE_CONFIRM), tr(Str::CANCEL)};
            render_option_menu(title, icons, options, 2);
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
            lv_obj_set_style_text_font(hdr_label, &lv_font_it_14, 0);
            lv_obj_set_style_text_color(hdr_label, lv_color_black(), 0);
            lv_obj_align(hdr_label, LV_ALIGN_LEFT_MID, 6, -1);

            static const char *icons[] = {LV_SYMBOL_WIFI, LV_SYMBOL_UPLOAD};
            const char *options[] = {tr(Str::WIFI_JOIN_AP), tr(Str::WIFI_CREATE_AP)};
            int16_t y = ROW_H;
            for (int i = 0; i < 2; i++) {
                add_row(body, 4, y, SCREEN_W - 8, icons[i], options[i], i == menu_index);
                y += ROW_H;
            }
            add_hint(tr(Str::HINT_MENU));
            break;
        }

        case Screen::kWifiApActive: {
            char msg[160];
            snprintf(msg, sizeof(msg), tr(Str::WIFI_AP_MSG), wifi_ap_ip);
            add_qr_screen(WIFI_AP_QR_DATA, msg);
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

        case Screen::kFileTransfer: {
            char msg[192];
            snprintf(msg, sizeof(msg), tr(Str::FILE_TRANSFER_MSG), file_transfer_name);
            add_qr_screen(file_transfer_url, msg);
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
                add_row(body, 4, y, SCREEN_W - 8, LV_SYMBOL_WIFI, ssid, i == selected_index);
                y += ROW_H;
            }
            add_hint(tr(transcribe_is_waiting_for_wifi() ? Str::HINT_JOIN_CANCEL : Str::HINT_JOIN_BACK));
            break;
        }

        case Screen::kPlaying: {
            char msg[128];
            snprintf(msg, sizeof(msg), tr(Str::PLAYING), active_filename);
            add_info_card(LV_SYMBOL_PLAY, msg);
            add_hint(tr(Str::HINT_SELECT_STOP));
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
            add_hint(tr(Str::HINT_USB));
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
            add_hint(tr(Str::HINT_WORK_OFFLINE));
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
            add_info_card(LV_SYMBOL_POWER, tr(Str::SLEEPING));
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
    lv_obj_set_style_text_font(header_label, &lv_font_it_12, 0);
    lv_obj_set_style_text_color(header_label, lv_color_white(), 0);
    lv_label_set_long_mode(header_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(header_label, "");
    lv_obj_align(header_label, LV_ALIGN_LEFT_MID, 6, 0);

    // Right-side status icons: battery percentage (always) plus the SD
    // card icon (only if present) - grouped in one flex-row container so
    // battery text width (1-3 digits) doesn't need manual offset math
    // against the SD icon next to it.
    lv_obj_t *status_icons = lv_obj_create(header_bar);
    lv_obj_remove_style_all(status_icons);
    lv_obj_set_size(status_icons, LV_SIZE_CONTENT, HEADER_H);
    lv_obj_set_style_bg_opa(status_icons, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(status_icons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_icons, 4, 0);
    lv_obj_align(status_icons, LV_ALIGN_RIGHT_MID, -6, 0);

    battery_label = lv_label_create(status_icons);
    lv_obj_set_style_text_font(battery_label, &lv_font_it_12, 0);
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_label_set_text(battery_label, ""); // filled in by ui_set_battery_percent()
    battery_last_percent = 255;           // force the next ui_set_battery_percent() call to repaint

    if (sdPresent) {
        lv_obj_t *sd_icon = lv_label_create(status_icons);
        lv_label_set_text(sd_icon, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_font(sd_icon, &lv_font_it_12, 0);
        lv_obj_set_style_text_color(sd_icon, lv_color_white(), 0);
    }

    lv_obj_set_width(header_label, SCREEN_W - 60); // leaves room for status_icons

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

// Same "really connected" guard as ui_show_wifi_joined_screen() above, same
// reasoning.
void ui_show_file_transfer_screen(const char *ip, const char *filename) {
    if (!wifi_is_connected()) {
        ui_set_wifi_status("");
        ui_show_wifi_manage_screen();
        return;
    }
    char status[64];
    snprintf(status, sizeof(status), LV_SYMBOL_WIFI " %s", ip);
    ui_set_wifi_status(status);
    char encoded[190];
    url_encode_component(filename, encoded, sizeof(encoded));
    snprintf(file_transfer_url, sizeof(file_transfer_url), "http://%s/api/download?name=%s", ip, encoded);
    strncpy(file_transfer_name, filename, sizeof(file_transfer_name) - 1);
    file_transfer_name[sizeof(file_transfer_name) - 1] = '\0';
    state = Screen::kFileTransfer;
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
    return state == Screen::kRecording || state == Screen::kPlaying || state == Screen::kTranscribeProgress ||
           state == Screen::kWifiApActive || state == Screen::kWifiJoined || state == Screen::kFileTransfer ||
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
    if (display_button_raw_pressed(DisplayButton::kNext) && display_button_raw_pressed(DisplayButton::kSelect)) {
        return;
    }

    DisplayButtonEvent nextEv = display_button_poll(DisplayButton::kNext);
    DisplayButtonEvent selEv = display_button_poll(DisplayButton::kSelect);

    // A held-back Select-short from kList (see select_press_pending's
    // comment) needs to fire even on a tick with no new button edge at
    // all, once its double-press window has lapsed - so this check runs
    // ahead of the "nothing happened" early return below.
    if (select_press_pending && state == Screen::kList &&
        millis() - select_press_pending_since >= DOUBLE_PRESS_WINDOW_MS) {
        select_press_pending = false;
        open_action_menu_for_selected();
    }

    if (nextEv == DisplayButtonEvent::kNone && selEv == DisplayButtonEvent::kNone) return;
    sleep_reset_activity(); // any button edge counts as activity - see sleep.h

    switch (state) {
        case Screen::kNoCard:
            break; // nothing to navigate - insert a card and reboot

        case Screen::kList:
            if (nextEv == DisplayButtonEvent::kLong) {
                select_press_pending = false; // leaving kList's row set - drop any held-back press
                menu_index = 0;
                state = Screen::kHome;
                render_body();
                break;
            }
            if (selEv == DisplayButtonEvent::kLong) {
                // Covers both the empty- and non-empty-list case (Refresh
                // is one of the menu's own options), so no separate
                // rescan-on-long-press branch is needed below anymore.
                select_press_pending = false; // leaving kList - drop any held-back press
                state = Screen::kMainMenu;
                menu_index = 0;
                render_body();
                break;
            }
            {
                size_t count = list_item_count();
                if (count == 0) break;
                if (nextEv == DisplayButtonEvent::kShort) {
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
                    selected_index = (selected_index + 1) % count;
                    render_body();
                } else if (selEv == DisplayButtonEvent::kShort) {
                    if (has_record_option() && selected_index == 0) {
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
                menu_index = (menu_index + 1) % 4;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0 || menu_index == 1) {
                    showing_audio_files = (menu_index == 0);
                    load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
                    selected_index = 0;
                    top_index = 0;
                    state = Screen::kList;
                    render_body();
                } else if (menu_index == 2) {
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
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
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
            if (nextEv == DisplayButtonEvent::kShort) {
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
            // {Play, Transcribe, Details, Delete, File transfer, Cancel} for
            // audio, {View, Details, Delete, File transfer, Cancel} for .txt
            // (no Play/Transcribe there - see that comment).
            int optionCount = showing_audio_files ? 6 : 5;
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % optionCount;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (showing_audio_files && menu_index == 0) {
                    speaker_play(active_filename);
                    state = Screen::kPlaying;
                    render_body();
                } else if (showing_audio_files && menu_index == 1 &&
                           mp3Files[active_file_index].size > TRANSCRIBE_MAX_FILE_BYTES) {
                    // Too big for the ESP32's own upload - never even
                    // queue it (see TRANSCRIBE_MAX_FILE_BYTES).
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "File too large to transcribe on the device (max %.3g MB).\n"
                             "Use the web interface's Transcribe button instead.",
                             TRANSCRIBE_MAX_FILE_BYTES / (1024.0 * 1024.0));
                    ui_show_transcribe_result(false, msg);
                } else if (showing_audio_files && menu_index == 1) {
                    // Don't touch state/render here - transcribe.h's
                    // transcribe_process_pending() (called right after
                    // this, from the same loop() iteration - see
                    // main.cpp) shows its own progress/result screens via
                    // ui_show_transcribe_progress()/ui_show_transcribe_result()
                    // moments from now, so redrawing the list first here
                    // would just be a wasted extra full-panel refresh.
                    transcribe_request(active_filename);
                } else if (!showing_audio_files && menu_index == 0) {
                    read_text_file_preview(active_filename, text_view_buffer, sizeof(text_view_buffer));
                    text_view_scroll_px = 0;
                    state = Screen::kTextView;
                    render_body();
                } else if (menu_index == (showing_audio_files ? 2 : 1)) {
                    build_details_text();
                    state = Screen::kDetails;
                    render_body();
                } else if (menu_index == (showing_audio_files ? 3 : 2)) {
                    state = Screen::kDeleteConfirm;
                    menu_index = 0;
                    render_body();
                } else if (menu_index == (showing_audio_files ? 4 : 3)) {
                    // Don't touch state/render here - wifi_manager.h's
                    // wifi_process_pending_file_link() (called right after
                    // this, same loop() iteration - see main.cpp) shows its
                    // own status/result screens, same reasoning as
                    // Transcribe above.
                    wifi_request_file_link(active_filename);
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
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    delete_file(active_filename);
                    load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
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

        case Screen::kRecording:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                mic_stop_recording();
                // Refresh so the just-finished recording shows up in the
                // list right away, same reason Delete re-scans below.
                load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
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
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

        case Screen::kTextView:
            if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                text_view_scroll_px += TEXT_VIEW_SCROLL_STEP;
                render_body(); // clamps to content height itself
            } else if (nextEv == DisplayButtonEvent::kShort) {
                text_view_scroll_px -= TEXT_VIEW_SCROLL_STEP;
                if (text_view_scroll_px < 0) text_view_scroll_px = 0;
                render_body();
            }
            break;

        case Screen::kSleeping:
            break; // device deep-sleeps right after showing this - never reached

        case Screen::kWifiManage:
            if (nextEv == DisplayButtonEvent::kShort) {
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

        case Screen::kFileTransfer:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                wifi_go_offline();
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;
    }
}

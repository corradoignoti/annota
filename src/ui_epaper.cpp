#include "ui.h"

#include <cstdio>
#include <cstring>
#include <lvgl.h>

#include "display.h"
#include "sleep.h"
#include "speaker.h"
#include "storage.h"
#include "transcribe.h"
#include "wifi_manager.h"

// -----------------------------------------------------------------------
// The on-device screen: WiFi status, a scrollable file list, and per-file
// Transcribe/Delete - deliberately small in scope (no on-device Settings,
// WiFi credential entry, or text-file preview - all of those stay on the
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
    kActionMenu,
    kDeleteConfirm,
    kPlaying,
    kRecording,
    kMicError,
    kWifiSetup,
    kWifiTimeoutDialog,
    kTranscribeProgress,
    kTranscribeResult,
    kDetails,
    kSleeping,
    kForgetWifiConfirm,
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
// AUDIO_EXTS, false while showing .txt - toggled by a long Next press
// (see ui_process_input()'s kList case).
static bool showing_audio_files = true;
static size_t selected_index = 0;
static size_t top_index = 0;

// kActionMenu / kDeleteConfirm / kDetails - the file the menu/confirm was
// opened for, and which option is currently highlighted. active_file_index
// indexes mp3Files directly (Details reads created/size straight off it).
static char active_filename[64];
static size_t active_file_index = 0;
static int menu_index = 0;

// kWifiSetup
static char wifi_setup_ssid[64];

// kWifiTimeoutDialog - forwarded to on Select, see wifi_manager.cpp's
// close_button_event_cb() (it ignores the lv_event_t* it's normally
// passed, so calling it with nullptr here is safe).
static lv_event_cb_t wifi_timeout_close_cb = nullptr;

// kTranscribeProgress / kTranscribeResult
static char transcribe_filename[64];
static bool transcribe_ok = false;
static char transcribe_message[128];

static void render_body();

// kList shows a synthetic "Record new" row pinned above the real files -
// but only while showing_audio_files (a recording is itself an audio
// file; there's nothing to record onto the .txt transcript list), same
// rule the Transcribe entry below follows. Kept as index 0 ahead of
// mp3Files rather than a separate widget/button so it reuses the same
// Next/Select navigation and clamp_selection() as every real row.
static bool has_record_option() {
    return showing_audio_files;
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
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, 0);
}

// kList's own top row: an icon, the Audio/Text mode label and file count,
// with a bottom border separating it from the cards below - not a card
// itself (never selectable), so it's built directly rather than through
// add_row(). `scrollable` - true once the list has more rows than
// VISIBLE_ROWS can show at once - draws a small down-arrow at the row's
// right edge (mirrors build_main_screen()'s right-aligned status icons)
// as the only hint that Next still reveals more: this list has no
// scrollbar, and Next wraps around rather than stopping at the last item
// (see ui_process_input()'s kList case), so the arrow stays fixed rather
// than tracking top_index/whether the view is currently at the bottom -
// there's always "more" to scroll to either way.
static void render_list_header(size_t count, bool scrollable) {
    lv_obj_t *hdr = lv_obj_create(body);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, SCREEN_W, ROW_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_black(), 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(hdr);
    lv_label_set_text_fmt(label, "%s  %s (%u)", showing_audio_files ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE,
                           showing_audio_files ? "Audio Files" : "Text Files", (unsigned)count);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, -1);

    if (scrollable) {
        lv_obj_t *more = lv_label_create(hdr);
        lv_label_set_text(more, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_font(more, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(more, lv_color_black(), 0);
        lv_obj_align(more, LV_ALIGN_RIGHT_MID, -6, -1);
    }
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
        lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(icon_label, lv_color_black(), 0);
    }

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, text);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, card_w - pad * 2);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_black(), 0);

    lv_obj_align(card, LV_ALIGN_CENTER, 0, -8); // slightly above center, to balance against the hint bar below
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
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 2);
}

// Shared by kActionMenu and kDeleteConfirm - a bordered panel holding a
// title line plus a cycle-and-confirm option list, one icon+label card
// per option (see add_row()).
static void render_option_menu(const char *title, const char *const *icons, const char *const *options, int count) {
    const int16_t pad = 6;
    const int16_t title_h = 20;
    const int16_t panel_w = SCREEN_W - 16;
    const int16_t panel_h = pad * 2 + title_h + count * ROW_H;
    const int16_t panel_y = 12;

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
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
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

    add_hint("Next: cycle   Select: choose, hold: back");
}

static void render_body() {
    lv_obj_clean(body);

    switch (state) {
        case Screen::kNoCard:
            add_info_card(LV_SYMBOL_SD_CARD, "Insert an SD card to see your audio files");
            break;

        case Screen::kList: {
            size_t count = list_item_count();
            render_list_header(mp3FileCount, count > (size_t)VISIBLE_ROWS);
            if (count == 0) {
                add_info_card(showing_audio_files ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE,
                               showing_audio_files ? "No audio files on the SD card" : "No text files on the SD card");
                add_hint("Sel(hold): rescan   Next(hold): switch");
                break;
            }
            clamp_selection();
            bool recordOption = has_record_option();
            int16_t y = ROW_H;
            for (size_t i = top_index; i < count && (i - top_index) < (size_t)VISIBLE_ROWS; i++) {
                const char *label = (recordOption && i == 0) ? "Record new" : mp3Files[recordOption ? i - 1 : i].filename;
                const char *icon = (recordOption && i == 0) ? LV_SYMBOL_PLUS : (showing_audio_files ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE);
                add_row(body, 4, y, SCREEN_W - 8, icon, label, i == selected_index);
                y += ROW_H;
            }
            add_hint("Next: move, hold: switch   Sel: open, hold: rescan");
            break;
        }

        case Screen::kActionMenu: {
            // Play/Transcription only make sense for audio files, not the
            // .txt transcripts this same list shows when toggled.
            if (showing_audio_files) {
                static const char *icons[] = {LV_SYMBOL_PLAY, LV_SYMBOL_EDIT, LV_SYMBOL_LIST, LV_SYMBOL_TRASH, LV_SYMBOL_CLOSE};
                static const char *options[] = {"Play", "Transcribe", "Details", "Delete", "Cancel"};
                render_option_menu(active_filename, icons, options, 5);
            } else {
                static const char *icons[] = {LV_SYMBOL_LIST, LV_SYMBOL_TRASH, LV_SYMBOL_CLOSE};
                static const char *options[] = {"Details", "Delete", "Cancel"};
                render_option_menu(active_filename, icons, options, 3);
            }
            break;
        }

        case Screen::kDetails: {
            const Mp3Entry &entry = mp3Files[active_file_index];
            char msg[160];
            snprintf(msg, sizeof(msg), "%s\n\nCreated: %s\nSize: %lu KB", entry.filename, entry.created,
                     (unsigned long)((entry.size + 1023) / 1024));
            add_info_card(LV_SYMBOL_LIST, msg);
            add_hint("Select: close");
            break;
        }

        case Screen::kDeleteConfirm: {
            char title[96];
            snprintf(title, sizeof(title), "Delete %s?", active_filename);
            static const char *icons[] = {LV_SYMBOL_TRASH, LV_SYMBOL_CLOSE};
            static const char *options[] = {"Confirm delete", "Cancel"};
            render_option_menu(title, icons, options, 2);
            break;
        }

        case Screen::kForgetWifiConfirm: {
            static const char *icons[] = {LV_SYMBOL_TRASH, LV_SYMBOL_CLOSE};
            static const char *options[] = {"Forget & reboot", "Cancel"};
            render_option_menu("Forget saved WiFi?", icons, options, 2);
            break;
        }

        case Screen::kPlaying: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Playing %s...", active_filename);
            add_info_card(LV_SYMBOL_PLAY, msg);
            add_hint("Select: stop");
            break;
        }

        case Screen::kRecording: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Recording %s...", active_filename);
            add_info_card(LV_SYMBOL_AUDIO, msg);
            add_hint("Select: stop");
            break;
        }

        case Screen::kMicError:
            add_info_card(LV_SYMBOL_WARNING, mic_last_error());
            add_hint("Select: close");
            break;

        case Screen::kWifiSetup: {
            char msg[128];
            snprintf(msg, sizeof(msg), "Join WiFi network \"%s\" from your phone or laptop to set up this device's WiFi.", wifi_setup_ssid);
            add_info_card(LV_SYMBOL_WIFI, msg);
            break;
        }

        case Screen::kWifiTimeoutDialog:
            add_info_card(LV_SYMBOL_WARNING, "Couldn't connect to WiFi. The device is running offline.");
            add_hint("Select: close");
            break;

        case Screen::kTranscribeProgress: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Transcribing %s...", transcribe_filename);
            add_info_card(LV_SYMBOL_REFRESH, msg);
            break;
        }

        case Screen::kTranscribeResult:
            add_info_card(transcribe_ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, transcribe_message);
            add_hint("Select: close");
            break;

        case Screen::kSleeping:
            add_info_card(LV_SYMBOL_POWER, "Sleeping...\nHold Select to wake");
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
    lv_obj_set_style_text_font(header_label, &lv_font_montserrat_12, 0);
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
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_label_set_text(battery_label, ""); // filled in by ui_set_battery_percent()
    battery_last_percent = 255;           // force the next ui_set_battery_percent() call to repaint

    if (sdPresent) {
        lv_obj_t *sd_icon = lv_label_create(status_icons);
        lv_label_set_text(sd_icon, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_font(sd_icon, &lv_font_montserrat_12, 0);
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
    if (state != Screen::kWifiSetup && state != Screen::kWifiTimeoutDialog) return;
    state = sd_present ? Screen::kList : Screen::kNoCard;
    render_body();
    lv_timer_handler();
}

void ui_show_wifi_timeout_dialog(lv_event_cb_t close_cb) {
    wifi_timeout_close_cb = close_cb;
    state = Screen::kWifiTimeoutDialog;
    render_body();
    lv_timer_handler();
}

// No Settings view here to refresh a retry button on - see ui.h's comment.
void ui_refresh_wifi_retry_button() {}

void ui_show_transcribe_progress(const char *filename) {
    strncpy(transcribe_filename, filename, sizeof(transcribe_filename) - 1);
    transcribe_filename[sizeof(transcribe_filename) - 1] = '\0';
    state = Screen::kTranscribeProgress;
    render_body();
    lv_timer_handler();
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
    return state == Screen::kRecording || state == Screen::kPlaying || state == Screen::kTranscribeProgress;
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

    // Checked ahead of display_button_poll() below, and independent of it -
    // see display.h's comment on why a two-button hold must never also
    // reach that per-button state machine (it would fire its own,
    // shorter-threshold kLong on one of them first). While both are held,
    // skip the individual poll entirely for this iteration: nextEv/selEv
    // both come back kNone below either way, since bothHeld's early return
    // never even reaches the poll calls.
    if (display_forget_wifi_combo_poll()) {
        sleep_reset_activity();
        if (state == Screen::kRecording) mic_stop_recording();
        if (state == Screen::kPlaying) speaker_stop();
        state = Screen::kForgetWifiConfirm;
        menu_index = 0;
        render_body();
        return;
    }
    if (display_button_raw_pressed(DisplayButton::kNext) && display_button_raw_pressed(DisplayButton::kSelect)) {
        return;
    }

    DisplayButtonEvent nextEv = display_button_poll(DisplayButton::kNext);
    DisplayButtonEvent selEv = display_button_poll(DisplayButton::kSelect);
    if (nextEv == DisplayButtonEvent::kNone && selEv == DisplayButtonEvent::kNone) return;
    sleep_reset_activity(); // any button edge counts as activity - see sleep.h

    switch (state) {
        case Screen::kNoCard:
            break; // nothing to navigate - insert a card and reboot

        case Screen::kList:
            if (nextEv == DisplayButtonEvent::kLong) {
                showing_audio_files = !showing_audio_files;
                load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
                selected_index = 0;
                top_index = 0;
                render_body();
                break;
            }
            {
                size_t count = list_item_count();
                if (count == 0) {
                    if (selEv == DisplayButtonEvent::kLong) {
                        load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
                        render_body();
                    }
                    break;
                }
                if (nextEv == DisplayButtonEvent::kShort) {
                    selected_index = (selected_index + 1) % count;
                    render_body();
                } else if (selEv == DisplayButtonEvent::kShort) {
                    if (has_record_option() && selected_index == 0) {
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
                    } else {
                        size_t fileIndex = has_record_option() ? selected_index - 1 : selected_index;
                        active_file_index = fileIndex;
                        strncpy(active_filename, mp3Files[fileIndex].filename, sizeof(active_filename) - 1);
                        active_filename[sizeof(active_filename) - 1] = '\0';
                        menu_index = 0;
                        state = Screen::kActionMenu;
                        render_body();
                    }
                } else if (selEv == DisplayButtonEvent::kLong) {
                    load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
                    selected_index = 0;
                    top_index = 0;
                    render_body();
                }
            }
            break;

        case Screen::kActionMenu: {
            // Option count/order tracks render_body()'s kActionMenu case:
            // {Play, Transcribe, Details, Delete, Cancel} for audio,
            // {Details, Delete, Cancel} for .txt (no Play/Transcribe there
            // - see that comment).
            int optionCount = showing_audio_files ? 5 : 3;
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
                } else if (showing_audio_files && menu_index == 1) {
                    // Don't touch state/render here - transcribe.h's
                    // transcribe_process_pending() (called right after
                    // this, from the same loop() iteration - see
                    // main.cpp) shows its own progress/result screens via
                    // ui_show_transcribe_progress()/ui_show_transcribe_result()
                    // moments from now, so redrawing the list first here
                    // would just be a wasted extra full-panel refresh.
                    transcribe_request(active_filename);
                } else if (menu_index == (showing_audio_files ? 2 : 0)) {
                    state = Screen::kDetails;
                    render_body();
                } else if (menu_index == (showing_audio_files ? 3 : 1)) {
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

        case Screen::kWifiSetup:
            break; // informational only - see ui.h's contract

        case Screen::kWifiTimeoutDialog:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                if (wifi_timeout_close_cb) wifi_timeout_close_cb(nullptr);
            }
            break;

        case Screen::kTranscribeProgress:
            break; // informational only, until transcribe_process_pending() replaces it

        case Screen::kTranscribeResult:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;

        case Screen::kDetails:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = Screen::kActionMenu;
                render_body();
            }
            break;

        case Screen::kSleeping:
            break; // device deep-sleeps right after showing this - never reached

        case Screen::kForgetWifiConfirm:
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                render_body();
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    // Never returns (ESP.restart()) - no state/render
                    // needed after.
                    wifi_forget_and_reboot();
                }
                state = sd_present ? Screen::kList : Screen::kNoCard;
                render_body();
            }
            break;
    }
}

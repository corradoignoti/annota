#include "ui.h"

#include <cstdio>
#include <lvgl.h>

#include "display.h"
#include "storage.h"

// -----------------------------------------------------------------------
// Main screen: title + scrollable list of rounded file cards, or an
// "insert an SD card" prompt when there's no card to read from. A Refresh
// button pinned below the list re-scans the SD card without a reboot.
// -----------------------------------------------------------------------

static lv_style_t style_card;
static lv_obj_t *wifi_status_label = nullptr;
static lv_obj_t *wifi_dialog = nullptr;
static lv_obj_t *file_list = nullptr;
static lv_obj_t *file_list_title = nullptr;
// Toggled by the file/audio button - true while showing mp3Files/
// mp3FileCount as .mp3 files, false while showing them as .txt files.
static bool showing_audio_files = true;

static void show_insert_card_message(lv_obj_t *scr) {
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Insert an SD card to see your audio files");
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(80));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x9AA0AC), 0);
}

// Wipes and repopulates `list` from mp3Files/mp3FileCount. Used both for
// the initial build and for the Refresh button.
static void render_file_list(lv_obj_t *list) {
    lv_obj_clean(list);

    if (mp3FileCount == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, showing_audio_files ? "No audio files found" : "No text files found");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9AA0AC), 0);
        return;
    }

    for (size_t i = 0; i < mp3FileCount; i++) {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, mp3Files[i].filename);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(name, lv_color_white(), 0);

        lv_obj_t *date = lv_label_create(card);
        lv_label_set_text(date, mp3Files[i].created);
        lv_obj_set_style_text_font(date, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(date, lv_color_hex(0x9AA0AC), 0);
    }
}

// SD access here shares the same SPI peripheral as touch (see storage.h /
// display.h) - pause touch, re-scan, resume, same dance web_server.cpp
// does for its file-manager requests.
static void refresh_button_event_cb(lv_event_t *e) {
    (void)e;
    display_suspend_touch();
    load_file_catalog(showing_audio_files ? ".mp3" : ".txt");
    display_resume_touch();

    if (file_list) {
        render_file_list(file_list);
    }
}

// Toggles between the .mp3 and .txt catalogs, re-scanning the SD card and
// swapping the button's own icon plus the list title to match. Same SPI
// pause/claim/release/resume dance as refresh_button_event_cb.
static void file_button_event_cb(lv_event_t *e) {
    lv_obj_t *label = static_cast<lv_obj_t *>(lv_event_get_user_data(e));

    showing_audio_files = !showing_audio_files;

    display_suspend_touch();
    load_file_catalog(showing_audio_files ? ".mp3" : ".txt");
    display_resume_touch();

    lv_label_set_text(label, showing_audio_files ? LV_SYMBOL_FILE : LV_SYMBOL_AUDIO);
    if (file_list_title) {
        lv_label_set_text(file_list_title, showing_audio_files ? "Audio Files" : "Text Files");
    }
    if (file_list) {
        render_file_list(file_list);
    }
}

static void add_bottom_buttons(lv_obj_t *scr) {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *refresh_btn = lv_button_create(row);
    lv_obj_set_flex_grow(refresh_btn, 1);
    lv_obj_add_event_cb(refresh_btn, refresh_button_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH);
    lv_obj_center(refresh_label);

    lv_obj_t *file_btn = lv_button_create(row);
    lv_obj_set_flex_grow(file_btn, 1);

    lv_obj_t *file_label = lv_label_create(file_btn);
    lv_label_set_text(file_label, LV_SYMBOL_FILE);
    lv_obj_add_event_cb(file_btn, file_button_event_cb, LV_EVENT_CLICKED, file_label);
    lv_obj_center(file_label);
}

void build_main_screen(bool sd_present) {
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

    wifi_status_label = lv_label_create(scr);
    lv_label_set_text(wifi_status_label, "");
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wifi_status_label, lv_pct(100));
    lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wifi_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x9AA0AC), 0);

    if (!sd_present) {
        show_insert_card_message(scr);
        return;
    }

    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Audio Files");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    file_list_title = title;
    showing_audio_files = true;

    // flex_grow(1) makes the list eat exactly the space left over after the
    // title/status labels above and the Refresh button below take theirs -
    // that's what keeps the (scrollable, unbounded-content) list from ever
    // overlapping the button rather than just pushing it off-screen.
    file_list = lv_obj_create(scr);
    lv_obj_remove_style_all(file_list);
    lv_obj_set_width(file_list, lv_pct(100));
    lv_obj_set_flex_grow(file_list, 1);
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(file_list, 8, 0);
    lv_obj_add_flag(file_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_AUTO);

    render_file_list(file_list);
    add_bottom_buttons(scr);
}

void ui_set_wifi_status(const char *text) {
    if (!wifi_status_label) return;
    lv_label_set_text(wifi_status_label, text);
    lv_timer_handler();
}

void ui_show_wifi_setup_dialog(const char *setup_ssid) {
    if (wifi_dialog) return;

    // Parented to the top layer, not the screen, so it floats above
    // build_main_screen()'s content (and its flex layout) untouched.
    wifi_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wifi_dialog);
    lv_obj_set_size(wifi_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(wifi_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_dialog, LV_OPA_70, 0);
    lv_obj_clear_flag(wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wifi_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card = lv_obj_create(wifi_dialog);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2E3A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_width(card, lv_pct(85));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "WiFi Setup Needed");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *msg = lv_label_create(card);
    char text[128];
    snprintf(text, sizeof(text),
             "On your phone or laptop, join the \"%s\" WiFi network, then "
             "pick your network in the page that opens.",
             setup_ssid);
    lv_label_set_text(msg, text);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x9AA0AC), 0);

    lv_timer_handler();
}

void ui_hide_wifi_setup_dialog() {
    if (!wifi_dialog) return;
    lv_obj_delete(wifi_dialog);
    wifi_dialog = nullptr;
    lv_timer_handler();
}

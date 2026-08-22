#include "ui.h"

#include <WiFi.h>
#include <cstdio>
#include <lvgl.h>

#include <ctime>

#include "display.h"
#include "storage.h"
#include "transcribe.h"
#include "wifi_manager.h"

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
static lv_obj_t *settings_view = nullptr;
static lv_obj_t *refresh_btn = nullptr;
static lv_obj_t *file_btn = nullptr;
static lv_obj_t *sd_info_label = nullptr;
static lv_obj_t *wifi_retry_btn = nullptr;
static lv_obj_t *clock_label = nullptr;
static lv_timer_t *clock_timer = nullptr;
// Toggled by the file/audio button - true while showing mp3Files/
// mp3FileCount as audio files (AUDIO_EXTS), false while showing them as
// .txt files.
static bool showing_audio_files = true;

// Settings view's AI provider API key field, plus the on-screen keyboard
// it (and only it) uses - see build_settings_view().
static lv_obj_t *api_key_textarea = nullptr;
static lv_obj_t *api_key_keyboard = nullptr;
static lv_obj_t *api_key_status_label = nullptr;

// Shared by the transcribe confirm/progress/result dialogs below - only
// one of the three is ever up at once, so they reuse one top-layer slot.
static lv_obj_t *transcribe_dialog = nullptr;
static char transcribe_target_filename[64];

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

// Forward-declared so render_file_list() (defined next) can wire it up;
// defined below alongside the confirm dialog it opens.
static void card_long_press_cb(lv_event_t *e);

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

        // Transcription only makes sense for audio files, not the .txt
        // transcripts this same list shows when toggled - so only audio
        // cards get the long-press handler. i is this entry's index into
        // mp3Files at render time; card_long_press_cb captures the
        // filename immediately (not this index) since the array can be
        // rescanned/reordered later.
        if (showing_audio_files) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(card, card_long_press_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)i);
        }
    }
}

// SD access here shares the same SPI peripheral as touch (see storage.h /
// display.h) - pause touch, re-scan, resume, same dance web_server.cpp
// does for its file-manager requests.
static void refresh_button_event_cb(lv_event_t *e) {
    (void)e;
    display_suspend_touch();
    load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
    display_resume_touch();

    if (file_list) {
        render_file_list(file_list);
    }
}

// Toggles between the audio (AUDIO_EXTS) and .txt catalogs, re-scanning the SD card and
// swapping the button's own icon plus the list title to match. Same SPI
// pause/claim/release/resume dance as refresh_button_event_cb.
static void file_button_event_cb(lv_event_t *e) {
    lv_obj_t *label = static_cast<lv_obj_t *>(lv_event_get_user_data(e));

    showing_audio_files = !showing_audio_files;

    display_suspend_touch();
    load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
    display_resume_touch();

    lv_label_set_text(label, showing_audio_files ? LV_SYMBOL_FILE : LV_SYMBOL_AUDIO);
    if (file_list_title) {
        lv_label_set_text(file_list_title, showing_audio_files ? "Audio Files" : "Text Files");
    }
    if (file_list) {
        render_file_list(file_list);
    }
}

// Refreshes clock_label with the current wall-clock time and whether it's
// been NTP-synced. Ticks once a second via clock_timer while the settings
// view is open (see settings_button_event_cb()); called once more directly
// on open so the label isn't blank for that first second.
static void clock_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!clock_label) return;

    time_t now = time(nullptr);
    struct tm tmInfo;
    localtime_r(&now, &tmInfo);
    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmInfo);

    char text[48];
    snprintf(text, sizeof(text), "%s  %s", timeStr,
             wifi_clock_synced() ? LV_SYMBOL_OK " NTP synced" : LV_SYMBOL_WARNING " not synced");
    lv_label_set_text(clock_label, text);
}

// Forward-declared so the Reconnect WiFi button (built below) can refresh
// its own disabled state right after a retry attempt finishes.
static void update_settings_info();

// Forward-declared so the Delete WiFi Setup button (built below) can wire
// up its click handler; defined below alongside the confirmation dialog it
// opens.
static void forget_button_event_cb(lv_event_t *e);

// Shows/hides api_key_keyboard, and detaches it from api_key_textarea on
// hide - wired to the textarea's own FOCUSED/DEFOCUSED events below.
// api_key_keyboard is created lazily (on first focus) since it's the
// only text input in the app; parented to lv_layer_top() so it floats
// over settings_view instead of squeezing it, same overlay trick the
// dialogs use.
static void api_key_keyboard_event_cb(lv_event_t *e) {
    (void)e;
    if (api_key_keyboard) lv_obj_add_flag(api_key_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void api_key_textarea_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        if (!api_key_keyboard) {
            api_key_keyboard = lv_keyboard_create(lv_layer_top());
            lv_obj_add_event_cb(api_key_keyboard, api_key_keyboard_event_cb, LV_EVENT_READY, nullptr);
            lv_obj_add_event_cb(api_key_keyboard, api_key_keyboard_event_cb, LV_EVENT_CANCEL, nullptr);
        }
        lv_keyboard_set_textarea(api_key_keyboard, api_key_textarea);
        lv_obj_remove_flag(api_key_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(api_key_keyboard);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (api_key_keyboard) lv_obj_add_flag(api_key_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

// Saves whatever's currently in api_key_textarea to NVS (transcribe.h) -
// an empty field clears the saved key. Doesn't validate the key itself
// (that only happens for real the next time a transcription is
// attempted); this just confirms the save with api_key_status_label.
static void api_key_save_event_cb(lv_event_t *e) {
    (void)e;
    if (!api_key_textarea) return;
    const char *text = lv_textarea_get_text(api_key_textarea);
    ai_provider_set_api_key(text);
    if (api_key_status_label) {
        lv_label_set_text(api_key_status_label, text[0] ? "API key saved" : "API key cleared");
    }
    if (api_key_keyboard) lv_obj_add_flag(api_key_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// Builds the settings view as a sibling of file_list, occupying the same
// flex slot between the title and the bottom button row - so unlike a
// top-layer overlay, it never covers the row of buttons below it. Created
// once, hidden, and toggled by settings_button_event_cb() alongside
// file_list/file_list_title. sd_info_label starts blank; it's filled in by
// update_settings_info() each time the view is opened. clock_timer starts
// paused and is only resumed while the view is visible.
static void build_settings_view(lv_obj_t *scr) {
    settings_view = lv_obj_create(scr);
    lv_obj_remove_style_all(settings_view);
    lv_obj_set_width(settings_view, lv_pct(100));
    lv_obj_set_flex_grow(settings_view, 1);
    lv_obj_set_flex_flow(settings_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(settings_view, 4, 0);
    // Scrollable (unlike a plain static settings panel): the API key
    // field + buttons pushed this past one screen's worth of content, so
    // this needs to scroll the same way file_list does.
    lv_obj_add_flag(settings_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(settings_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(settings_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(settings_view);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    clock_label = lv_label_create(settings_view);
    lv_label_set_text(clock_label, "");
    lv_obj_set_width(clock_label, lv_pct(100));
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);

    clock_timer = lv_timer_create(clock_timer_cb, 1000, nullptr);
    lv_timer_pause(clock_timer);

    sd_info_label = lv_label_create(settings_view);
    lv_label_set_text(sd_info_label, "");
    lv_label_set_long_mode(sd_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sd_info_label, lv_pct(100));
    lv_obj_set_style_text_font(sd_info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sd_info_label, lv_color_hex(0x9AA0AC), 0);

    // Asks wifi_manager.cpp to re-run its connect attempt on demand -
    // e.g. after it gave up and the user fixed the router, or just wants
    // to retry without waiting for the timeout dialog to reappear on its
    // own (it doesn't - that only fires from wifi_manager.cpp's own
    // attempts). Only requests it (see wifi_request_reconnect()'s
    // comment for why this can't just call the blocking retry directly
    // from an LV_EVENT_CLICKED handler); wifi_manager.cpp calls back
    // into ui_refresh_wifi_retry_button() once the attempt actually runs.
    wifi_retry_btn = lv_button_create(settings_view);
    lv_obj_set_width(wifi_retry_btn, lv_pct(100));
    lv_obj_set_style_margin_top(wifi_retry_btn, 8, 0);
    lv_obj_add_event_cb(
        wifi_retry_btn, [](lv_event_t *e) {
            (void)e;
            wifi_request_reconnect();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *wifi_retry_label = lv_label_create(wifi_retry_btn);
    lv_label_set_text(wifi_retry_label, LV_SYMBOL_WIFI " Reconnect WiFi");
    lv_obj_center(wifi_retry_label);

    // Danger button: erases the saved WiFi network and reboots into setup
    // mode (wifi_forget_and_reboot()) - irreversible, so this only opens a
    // confirmation dialog (forget_button_event_cb below); the actual erase
    // only happens if the user confirms there.
    lv_obj_t *wifi_forget_btn = lv_button_create(settings_view);
    lv_obj_set_width(wifi_forget_btn, lv_pct(100));
    lv_obj_set_style_margin_top(wifi_forget_btn, 8, 0);
    lv_obj_set_style_bg_color(wifi_forget_btn, lv_color_hex(0xB3261E), 0);
    lv_obj_add_event_cb(wifi_forget_btn, forget_button_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *wifi_forget_label = lv_label_create(wifi_forget_btn);
    lv_label_set_text(wifi_forget_label, LV_SYMBOL_TRASH " Delete WiFi Setup");
    lv_obj_center(wifi_forget_label);

    // API key for whichever AI_PROVIDER_* is compiled in (transcribe.h),
    // used by transcribe.cpp for the long-press-to-transcribe flow
    // (ui.cpp's card_long_press_cb). Password mode masks it on screen;
    // it's still stored in NVS as plain text, same as WiFiManager's own
    // saved credentials. Title/placeholder name the provider dynamically
    // (ai_provider_name()) so this doesn't need editing when the compiled-
    // in provider changes.
    lv_obj_t *api_key_title = lv_label_create(settings_view);
    char titleText[48];
    snprintf(titleText, sizeof(titleText), "%s API Key", ai_provider_name());
    lv_label_set_text(api_key_title, titleText);
    lv_obj_set_style_margin_top(api_key_title, 8, 0);
    lv_obj_set_style_text_font(api_key_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(api_key_title, lv_color_white(), 0);

    api_key_textarea = lv_textarea_create(settings_view);
    lv_textarea_set_one_line(api_key_textarea, true);
    lv_textarea_set_password_mode(api_key_textarea, true);
    lv_textarea_set_placeholder_text(api_key_textarea, "sk-...");
    lv_obj_set_width(api_key_textarea, lv_pct(100));
    char existingKey[AI_API_KEY_MAX];
    ai_provider_get_api_key(existingKey, sizeof(existingKey));
    lv_textarea_set_text(api_key_textarea, existingKey);
    lv_obj_add_event_cb(api_key_textarea, api_key_textarea_event_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(api_key_textarea, api_key_textarea_event_cb, LV_EVENT_DEFOCUSED, nullptr);

    lv_obj_t *api_key_save_btn = lv_button_create(settings_view);
    lv_obj_set_width(api_key_save_btn, lv_pct(100));
    lv_obj_add_event_cb(api_key_save_btn, api_key_save_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *api_key_save_label = lv_label_create(api_key_save_btn);
    lv_label_set_text(api_key_save_label, LV_SYMBOL_SAVE " Save API Key");
    lv_obj_center(api_key_save_label);

    api_key_status_label = lv_label_create(settings_view);
    lv_label_set_text(api_key_status_label, "");
    lv_obj_set_style_text_font(api_key_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(api_key_status_label, lv_color_hex(0x9AA0AC), 0);
}

// Modal confirmation for wifi_forget_and_reboot() - same top-layer overlay
// pattern as ui_show_wifi_setup_dialog()/ui_show_wifi_timeout_dialog(), but
// kept in its own dialog slot (forget_confirm_dialog) since those two are
// wifi_manager.cpp's, driven by the connect flow, and can legitimately be
// showing at the same time this one isn't (Settings is only reachable once
// wifi_connect() has already resolved one way or another).
static lv_obj_t *forget_confirm_dialog = nullptr;

static void hide_forget_confirm_dialog() {
    if (!forget_confirm_dialog) return;
    // _async: called from a button inside forget_confirm_dialog itself -
    // see ui_hide_wifi_setup_dialog()'s comment for why this can't delete
    // synchronously out from under its own still-dispatching click event.
    lv_obj_delete_async(forget_confirm_dialog);
    forget_confirm_dialog = nullptr;
}

static void forget_confirm_no_cb(lv_event_t *e) {
    (void)e;
    hide_forget_confirm_dialog();
}

static void forget_confirm_yes_cb(lv_event_t *e) {
    (void)e;
    // No need to hide the dialog first - wifi_forget_and_reboot() erases
    // the saved network and reboots immediately; nothing ever comes back
    // to repaint it.
    wifi_forget_and_reboot();
}

static void show_forget_confirm_dialog() {
    if (forget_confirm_dialog) return;

    forget_confirm_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(forget_confirm_dialog);
    lv_obj_set_size(forget_confirm_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(forget_confirm_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(forget_confirm_dialog, LV_OPA_70, 0);
    lv_obj_clear_flag(forget_confirm_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(forget_confirm_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(forget_confirm_dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card = lv_obj_create(forget_confirm_dialog);
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
    lv_label_set_text(title, LV_SYMBOL_WARNING " Delete WiFi Setup?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, "This erases the saved WiFi network and reboots the device into setup mode.");
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x9AA0AC), 0);

    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_set_style_margin_top(btn_row, 8, 0);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_flex_grow(cancel_btn, 1);
    lv_obj_add_event_cb(cancel_btn, forget_confirm_no_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);

    lv_obj_t *delete_btn = lv_button_create(btn_row);
    lv_obj_set_flex_grow(delete_btn, 1);
    lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xB3261E), 0);
    lv_obj_add_event_cb(delete_btn, forget_confirm_yes_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *delete_label = lv_label_create(delete_btn);
    lv_label_set_text(delete_label, "Delete");
    lv_obj_center(delete_label);

    lv_timer_handler();
}

static void forget_button_event_cb(lv_event_t *e) {
    (void)e;
    show_forget_confirm_dialog();
}

// -----------------------------------------------------------------------
// Transcribe confirm/progress/result dialogs, triggered by long-pressing
// an audio card (card_long_press_cb, wired in render_file_list()). All
// three share the transcribe_dialog top-layer slot - only one is ever up
// at once. The confirm dialog's Yes button only calls transcribe_request()
// (transcribe.h); the actual blocking work happens later from loop() via
// transcribe_process_pending(), which is what calls
// ui_show_transcribe_progress()/ui_show_transcribe_result() below - same
// split as wifi_manager.cpp's reconnect flow, and for the same reason
// (see transcribe_request()'s comment).
// -----------------------------------------------------------------------

static void hide_transcribe_dialog_async() {
    if (!transcribe_dialog) return;
    // _async: called from a button inside transcribe_dialog itself - see
    // ui_hide_wifi_setup_dialog()'s comment for why this can't delete
    // synchronously out from under its own still-dispatching click event.
    lv_obj_delete_async(transcribe_dialog);
    transcribe_dialog = nullptr;
}

static void transcribe_confirm_no_cb(lv_event_t *e) {
    (void)e;
    hide_transcribe_dialog_async();
}

static void transcribe_confirm_yes_cb(lv_event_t *e) {
    (void)e;
    hide_transcribe_dialog_async();
    transcribe_request(transcribe_target_filename);
}

static void show_transcribe_confirm_dialog(const char *filename) {
    if (transcribe_dialog) return;
    strncpy(transcribe_target_filename, filename, sizeof(transcribe_target_filename) - 1);
    transcribe_target_filename[sizeof(transcribe_target_filename) - 1] = '\0';

    transcribe_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(transcribe_dialog);
    lv_obj_set_size(transcribe_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(transcribe_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(transcribe_dialog, LV_OPA_70, 0);
    lv_obj_clear_flag(transcribe_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(transcribe_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(transcribe_dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card = lv_obj_create(transcribe_dialog);
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
    lv_label_set_text(title, "Transcribe Audio?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *msg = lv_label_create(card);
    char text[96];
    snprintf(text, sizeof(text), "Send \"%s\" to %s for transcription?", filename, ai_provider_name());
    lv_label_set_text(msg, text);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x9AA0AC), 0);

    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_set_style_margin_top(btn_row, 8, 0);

    lv_obj_t *no_btn = lv_button_create(btn_row);
    lv_obj_set_flex_grow(no_btn, 1);
    lv_obj_add_event_cb(no_btn, transcribe_confirm_no_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *no_label = lv_label_create(no_btn);
    lv_label_set_text(no_label, "No");
    lv_obj_center(no_label);

    lv_obj_t *yes_btn = lv_button_create(btn_row);
    lv_obj_set_flex_grow(yes_btn, 1);
    lv_obj_add_event_cb(yes_btn, transcribe_confirm_yes_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *yes_label = lv_label_create(yes_btn);
    lv_label_set_text(yes_label, "Yes");
    lv_obj_center(yes_label);

    lv_timer_handler();
}

static void transcribe_offline_close_cb(lv_event_t *e) {
    (void)e;
    hide_transcribe_dialog_async();
}

// Shown instead of show_transcribe_confirm_dialog() when WiFi is down -
// no point asking "Transcribe?" for something guaranteed to fail
// (ai_transcribe_file() would just bounce it back with the same reason).
// Checking live WiFi.status() here, right at long-press time, catches a
// drop that happened after the screen was last painted - same reasoning
// as ui.cpp's own refresh_wifi_connection_ui().
static void show_transcribe_offline_dialog() {
    if (transcribe_dialog) return;

    transcribe_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(transcribe_dialog);
    lv_obj_set_size(transcribe_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(transcribe_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(transcribe_dialog, LV_OPA_70, 0);
    lv_obj_clear_flag(transcribe_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(transcribe_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(transcribe_dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card = lv_obj_create(transcribe_dialog);
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
    lv_label_set_text(title, LV_SYMBOL_WARNING " Device Offline");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *msg = lv_label_create(card);
    char offlineText[112];
    snprintf(offlineText, sizeof(offlineText), "Connect to WiFi first - transcription needs to reach %s over the internet.",
             ai_provider_name());
    lv_label_set_text(msg, offlineText);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x9AA0AC), 0);

    lv_obj_t *close_btn = lv_button_create(card);
    lv_obj_set_width(close_btn, lv_pct(100));
    lv_obj_add_event_cb(close_btn, transcribe_offline_close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    lv_timer_handler();
}

static void card_long_press_cb(lv_event_t *e) {
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= mp3FileCount) return;
    if (WiFi.status() != WL_CONNECTED) {
        show_transcribe_offline_dialog();
        return;
    }
    show_transcribe_confirm_dialog(mp3Files[idx].filename);
}

// Formats a byte count as e.g. "29.72 GB" (decimal GB, matching how SD
// cards are marketed/labeled).
static void format_gb(uint64_t bytes, char *out, size_t outLen) {
    snprintf(out, outLen, "%.2f GB", bytes / 1000000000.0);
}

// Re-checks live WiFi status and reflects it: disables wifi_retry_btn
// while already connected (retrying would be a no-op) and enables it
// otherwise, and syncs the top status line to match either way - not just
// on drop, but also on a recovery this module didn't see itself (e.g. the
// background auto-reconnect wifi_connect() listens for succeeding, or the
// underlying esp_wifi/lwIP station reassociating on its own) - otherwise
// the label can go stale showing "offline" while the button, reading live
// WiFi.status() right here, has already correctly re-disabled itself,
// which looks like the button is stuck rather than the label being out of
// date. The device keeps working offline the whole time regardless, this
// is purely informational. Shared by update_settings_info() (so opening
// Settings always catches a drop or recovery wifi_manager.cpp's own
// connect/retry flow didn't) and ui_refresh_wifi_retry_button() (called by
// wifi_manager.cpp after every connect/retry attempt, so neither the
// button nor the label goes stale if Settings happens to already be open
// when one finishes).
static void refresh_wifi_connection_ui() {
    bool connected = WiFi.status() == WL_CONNECTED;

    if (wifi_retry_btn) {
        if (connected) {
            lv_obj_add_state(wifi_retry_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(wifi_retry_btn, LV_STATE_DISABLED);
        }
    }

    if (wifi_status_label) {
        if (connected) {
            char msg[64];
            snprintf(msg, sizeof(msg), LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
            lv_label_set_text(wifi_status_label, msg);
        } else {
            lv_label_set_text(wifi_status_label, LV_SYMBOL_WARNING " working offline");
        }
    }
}

// Re-reads SD capacity/usage and refreshes sd_info_label. SD access here
// shares the same SPI peripheral as touch (see storage.h / display.h) -
// pause touch, query, resume, same dance refresh_button_event_cb does.
static void update_settings_info() {
    refresh_wifi_connection_ui();

    display_suspend_touch();
    SdInfo info;
    bool ok = get_sd_info(info);
    display_resume_touch();

    char text[160];
    if (ok && info.totalBytes > 0) {
        char cardStr[24];
        format_gb(info.cardBytes, cardStr, sizeof(cardStr));
        double usedPct = 100.0 * (double)info.usedBytes / (double)info.totalBytes;
        snprintf(text, sizeof(text), "SD card: %s\nSpace used: %.1f%%\nAudio files: %u\nText files: %u", cardStr,
                 usedPct, (unsigned)info.audioFileCount, (unsigned)info.textFileCount);
    } else {
        snprintf(text, sizeof(text), "SD card info unavailable");
    }
    lv_label_set_text(sd_info_label, text);
}

// Toggles between the file list and the settings view in the same flex
// slot, swapping the settings button's own icon between LV_SYMBOL_SETTINGS
// and LV_SYMBOL_CLOSE to match - same pattern as file_button_event_cb's
// own-icon toggle. The bottom button row stays visible either way (it's a
// separate sibling), but the Refresh and audio/text buttons are disabled
// while settings are open, since they act on the now-hidden file list.
static void settings_button_event_cb(lv_event_t *e) {
    lv_obj_t *label = static_cast<lv_obj_t *>(lv_event_get_user_data(e));

    bool showing_settings = !lv_obj_has_flag(settings_view, LV_OBJ_FLAG_HIDDEN);
    if (showing_settings) {
        lv_timer_pause(clock_timer);
        lv_obj_add_flag(settings_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(file_list_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(file_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(refresh_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(file_btn, LV_STATE_DISABLED);
        lv_label_set_text(label, LV_SYMBOL_SETTINGS);
    } else {
        update_settings_info();
        clock_timer_cb(nullptr);
        lv_timer_resume(clock_timer);
        lv_obj_add_flag(file_list_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(file_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(refresh_btn, LV_STATE_DISABLED);
        lv_obj_add_state(file_btn, LV_STATE_DISABLED);
        lv_label_set_text(label, LV_SYMBOL_CLOSE);
    }
}

static void add_bottom_buttons(lv_obj_t *scr) {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);

    refresh_btn = lv_button_create(row);
    lv_obj_set_flex_grow(refresh_btn, 1);
    lv_obj_add_event_cb(refresh_btn, refresh_button_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH);
    lv_obj_center(refresh_label);

    file_btn = lv_button_create(row);
    lv_obj_set_flex_grow(file_btn, 1);

    lv_obj_t *file_label = lv_label_create(file_btn);
    lv_label_set_text(file_label, LV_SYMBOL_FILE);
    lv_obj_add_event_cb(file_btn, file_button_event_cb, LV_EVENT_CLICKED, file_label);
    lv_obj_center(file_label);

    lv_obj_t *settings_btn = lv_button_create(row);
    lv_obj_set_flex_grow(settings_btn, 1);

    lv_obj_t *settings_label = lv_label_create(settings_btn);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS);
    lv_obj_add_event_cb(settings_btn, settings_button_event_cb, LV_EVENT_CLICKED, settings_label);
    lv_obj_center(settings_label);
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
    build_settings_view(scr);
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
    // _async: this may be called from a button inside wifi_dialog itself
    // (its own LV_EVENT_CLICKED handler, e.g. the timeout dialog's Close
    // button) - deleting wifi_dialog synchronously there would free the
    // button out from under its own still-dispatching click event.
    // Deferring the delete is the same fix the *button's* click handler
    // already needs for wifi_manager.cpp's retry request; see the
    // comment on wifi_request_reconnect().
    lv_obj_delete_async(wifi_dialog);
    wifi_dialog = nullptr;
    lv_timer_handler();
}

void ui_show_wifi_timeout_dialog(lv_event_cb_t close_cb) {
    if (wifi_dialog) return;

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
    lv_label_set_text(title, "WiFi Connection Failed");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, "Could not connect within 30 seconds. Continuing offline.");
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x9AA0AC), 0);

    lv_obj_t *close_btn = lv_button_create(card);
    lv_obj_set_width(close_btn, lv_pct(100));
    lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    lv_timer_handler();
}

void ui_refresh_wifi_retry_button() {
    refresh_wifi_connection_ui();
    lv_timer_handler();
}

void ui_show_transcribe_progress(const char *filename) {
    // Whatever's in transcribe_dialog here is the confirm dialog the Yes
    // button already scheduled an async delete for (see
    // transcribe_confirm_yes_cb) - that pointer was already cleared
    // there, so this is a plain create, not a hide-then-create.
    transcribe_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(transcribe_dialog);
    lv_obj_set_size(transcribe_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(transcribe_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(transcribe_dialog, LV_OPA_70, 0);
    lv_obj_clear_flag(transcribe_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(transcribe_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(transcribe_dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card = lv_obj_create(transcribe_dialog);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2E3A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_width(card, lv_pct(85));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *msg = lv_label_create(card);
    char text[96];
    snprintf(text, sizeof(text), "Transcribing \"%s\"...", filename);
    lv_label_set_text(msg, text);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);

    // Safe here (unlike the confirm dialog's own buttons): this only ever
    // runs from transcribe_process_pending(), called from loop() after
    // lv_timer_handler() has already returned, never nested inside it.
    lv_timer_handler();
}

static void transcribe_result_close_cb(lv_event_t *e) {
    (void)e;
    hide_transcribe_dialog_async();
}

void ui_show_transcribe_result(bool ok, const char *message) {
    // Not _async: called from transcribe_process_pending() (loop()), not
    // from a click handler nested inside transcribe_dialog itself - see
    // ui_show_transcribe_progress()'s comment.
    if (transcribe_dialog) {
        lv_obj_delete(transcribe_dialog);
        transcribe_dialog = nullptr;
    }

    transcribe_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(transcribe_dialog);
    lv_obj_set_size(transcribe_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(transcribe_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(transcribe_dialog, LV_OPA_70, 0);
    lv_obj_clear_flag(transcribe_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(transcribe_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(transcribe_dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card = lv_obj_create(transcribe_dialog);
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
    lv_label_set_text(title, ok ? LV_SYMBOL_OK " Transcription Complete" : LV_SYMBOL_WARNING " Transcription Failed");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, message);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x9AA0AC), 0);

    lv_obj_t *close_btn = lv_button_create(card);
    lv_obj_set_width(close_btn, lv_pct(100));
    lv_obj_add_event_cb(close_btn, transcribe_result_close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    lv_timer_handler();
}

#include "ui.h"

#include <cstdio>
#include <cstring>
#include <lvgl.h>

#include "display.h"
#include "speaker.h"
#include "storage.h"
#include "transcribe.h"

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
};

static const int16_t HEADER_H = 16;
static const int16_t ROW_H = 18;
static const int16_t HINT_H = 30; // fits add_hint()'s two wrapped lines
// kList reserves its own top row (below) for the Audio/Text mode label, on
// top of HEADER_H/HINT_H.
static const int VISIBLE_ROWS = (SCREEN_H - HEADER_H - HINT_H - ROW_H) / ROW_H;

static lv_obj_t *header_label = nullptr;
static lv_obj_t *body = nullptr;
static bool sd_present = false;
static Screen state = Screen::kNoCard;

// kList. showing_audio_files: true while mp3Files/mp3FileCount hold
// AUDIO_EXTS, false while showing .txt - toggled by a long Next press
// (see ui_process_input()'s kList case).
static bool showing_audio_files = true;
static size_t selected_index = 0;
static size_t top_index = 0;

// kActionMenu / kDeleteConfirm - the file the menu/confirm was opened for,
// and which option is currently highlighted.
static char active_filename[64];
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

// kList shows a synthetic "+ Record new" row pinned above the real files
// - but only while showing_audio_files (a recording is itself an audio
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

// One row of centered text, optionally drawn inverted (black bg, white
// text) to show it's the current selection - the only "focus" indicator
// this UI has, in place of touch highlighting.
static void add_row(int16_t y, const char *text, bool selected) {
    lv_obj_t *row = lv_label_create(body);
    lv_obj_set_size(row, SCREEN_W, ROW_H);
    lv_obj_set_pos(row, 0, y);
    lv_label_set_text(row, text);
    lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(row, 2, 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_black(), 0);
    lv_obj_set_style_text_color(row, selected ? lv_color_white() : lv_color_black(), 0);
}

static void add_centered_message(const char *text) {
    lv_obj_t *msg = lv_label_create(body);
    lv_label_set_text(msg, text);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, SCREEN_W - 20);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_black(), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 30);
}

// Wraps onto up to two lines instead of running off the 200px panel edge -
// callers keep hint text short enough to fit HINT_H at that wrap width.
static void add_hint(const char *text) {
    lv_obj_t *hint = lv_label_create(body);
    lv_label_set_text(hint, text);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, SCREEN_W - 4);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// Shared by kActionMenu and kDeleteConfirm - a title line plus a
// cycle-and-confirm option list.
static void render_option_menu(const char *title, const char *const *options, int count) {
    add_centered_message(title);
    int16_t y = 70;
    for (int i = 0; i < count; i++) {
        add_row(y, options[i], i == menu_index);
        y += ROW_H;
    }
    add_hint("Next: cycle   Select: choose, hold: back");
}

static void render_body() {
    lv_obj_clean(body);

    switch (state) {
        case Screen::kNoCard:
            add_centered_message("Insert an SD card to see your audio files");
            break;

        case Screen::kList: {
            add_row(0, showing_audio_files ? "Audio Files" : "Text Files", false);
            size_t count = list_item_count();
            if (count == 0) {
                add_centered_message(showing_audio_files ? "No audio files on the SD card" : "No text files on the SD card");
                add_hint("Sel(hold): rescan   Next(hold): switch");
                break;
            }
            clamp_selection();
            bool recordOption = has_record_option();
            int16_t y = ROW_H;
            for (size_t i = top_index; i < count && (i - top_index) < (size_t)VISIBLE_ROWS; i++) {
                const char *label = (recordOption && i == 0) ? "+ Record new" : mp3Files[recordOption ? i - 1 : i].filename;
                add_row(y, label, i == selected_index);
                y += ROW_H;
            }
            add_hint("Next: move, hold: switch   Sel: open, hold: rescan");
            break;
        }

        case Screen::kActionMenu: {
            // Play/Transcription only make sense for audio files, not the
            // .txt transcripts this same list shows when toggled.
            if (showing_audio_files) {
                static const char *options[] = {"Play", "Transcribe", "Delete", "Cancel"};
                render_option_menu(active_filename, options, 4);
            } else {
                static const char *options[] = {"Delete", "Cancel"};
                render_option_menu(active_filename, options, 2);
            }
            break;
        }

        case Screen::kDeleteConfirm: {
            char title[96];
            snprintf(title, sizeof(title), "Delete %s?", active_filename);
            static const char *options[] = {"Confirm delete", "Cancel"};
            render_option_menu(title, options, 2);
            break;
        }

        case Screen::kPlaying: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Playing %s...", active_filename);
            add_centered_message(msg);
            add_hint("Select: stop");
            break;
        }

        case Screen::kRecording: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Recording %s...", active_filename);
            add_centered_message(msg);
            add_hint("Select: stop");
            break;
        }

        case Screen::kMicError:
            add_centered_message(mic_last_error());
            add_hint("Select: close");
            break;

        case Screen::kWifiSetup: {
            char msg[128];
            snprintf(msg, sizeof(msg), "Join WiFi network \"%s\" from your phone or laptop to set up this device's WiFi.", wifi_setup_ssid);
            add_centered_message(msg);
            break;
        }

        case Screen::kWifiTimeoutDialog:
            add_centered_message("Couldn't connect to WiFi. The device is running offline.");
            add_hint("Select: close");
            break;

        case Screen::kTranscribeProgress: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Transcribing %s...", transcribe_filename);
            add_centered_message(msg);
            break;
        }

        case Screen::kTranscribeResult:
            add_centered_message(transcribe_message);
            add_hint("Select: close");
            break;
    }
}

void build_main_screen(bool sdPresent) {
    sd_present = sdPresent;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    header_label = lv_label_create(scr);
    lv_obj_set_style_text_font(header_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(header_label, lv_color_black(), 0);
    lv_obj_align(header_label, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(header_label, "");

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

    DisplayButtonEvent nextEv = display_button_poll(DisplayButton::kNext);
    DisplayButtonEvent selEv = display_button_poll(DisplayButton::kSelect);
    if (nextEv == DisplayButtonEvent::kNone && selEv == DisplayButtonEvent::kNone) return;

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
            // {Play, Transcribe, Delete, Cancel} for audio, {Delete,
            // Cancel} for .txt (no Play/Transcribe there - see that
            // comment).
            int optionCount = showing_audio_files ? 4 : 2;
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
    }
}

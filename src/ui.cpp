#include "ui.h"

#include <lvgl.h>

#include <cstdio>
#include <cstring>

#include "audio.h"
#include "storage.h"

// -----------------------------------------------------------------------
// Main screen: title + scrollable list of rounded file cards. Tapping a
// card opens a dialog with the file's detail and Play / Delete / Close.
// -----------------------------------------------------------------------

static lv_style_t style_card;
static char cached_source_label[32];

static void populate_screen();

static void build_file_path(size_t index, char *out, size_t outLen) {
    snprintf(out, outLen, "/%s", mp3Files[index].filename);
}

// Only one dialog can be open at a time (single-touch screen), so plain
// statics are enough - no need to smuggle state through event user_data.
static size_t dialog_index = 0;
static lv_obj_t *dialog_mbox = nullptr;
static lv_obj_t *dialog_play_label = nullptr;

static void close_dialog() {
    if (dialog_mbox) {
        audio_stop(); // don't leave something playing behind a closed dialog
        lv_msgbox_close(dialog_mbox);
        dialog_mbox = nullptr;
        dialog_play_label = nullptr;
    }
}

// Play/Pause is one button: starts playback on first tap, then toggles
// pause/resume on the same file for as long as the dialog stays open.
static void on_play_clicked(lv_event_t *e) {
    LV_UNUSED(e);

    if (audio_is_playing()) {
        if (audio_is_paused()) {
            audio_resume();
            lv_label_set_text(dialog_play_label, "Pause");
        } else {
            audio_pause();
            lv_label_set_text(dialog_play_label, "Play");
        }
        return;
    }

    char path[80];
    build_file_path(dialog_index, path, sizeof(path));
    if (audio_play(active_fs(), path)) {
        lv_label_set_text(dialog_play_label, "Pause");
    }
}

static lv_obj_t *confirm_mbox = nullptr;

static void close_confirm_dialog() {
    if (confirm_mbox) {
        lv_msgbox_close(confirm_mbox);
        confirm_mbox = nullptr;
    }
}

static void on_confirm_delete_clicked(lv_event_t *e) {
    LV_UNUSED(e);
    close_confirm_dialog();
    delete_mp3_file(dialog_index);
    close_dialog();
    populate_screen(); // reflect the removal (or the no-op if delete failed)
}

static void on_cancel_delete_clicked(lv_event_t *e) {
    LV_UNUSED(e);
    close_confirm_dialog(); // back to the file detail dialog, untouched
}

static void on_delete_clicked(lv_event_t *e) {
    LV_UNUSED(e);

    lv_obj_t *mbox = lv_msgbox_create(NULL);
    confirm_mbox = mbox;
    lv_obj_set_width(mbox, lv_pct(85));

    lv_msgbox_add_title(mbox, "Delete file?");
    lv_msgbox_add_text(mbox, mp3Files[dialog_index].filename);

    lv_obj_t *delete_btn = lv_msgbox_add_footer_button(mbox, "Delete");
    lv_obj_add_event_cb(delete_btn, on_confirm_delete_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_add_event_cb(cancel_btn, on_cancel_delete_clicked, LV_EVENT_CLICKED, NULL);
}

static void on_close_clicked(lv_event_t *e) {
    LV_UNUSED(e);
    close_dialog();
}

static void show_file_dialog(size_t index) {
    dialog_index = index;

    lv_obj_t *mbox = lv_msgbox_create(NULL); // NULL parent -> modal, own top layer
    dialog_mbox = mbox;
    lv_obj_set_width(mbox, lv_pct(85));

    lv_msgbox_add_title(mbox, mp3Files[index].filename);
    lv_msgbox_add_text(mbox, mp3Files[index].created);

    lv_obj_t *play_btn = lv_msgbox_add_footer_button(mbox, "Play");
    dialog_play_label = lv_obj_get_child(play_btn, 0);
    lv_obj_add_event_cb(play_btn, on_play_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *delete_btn = lv_msgbox_add_footer_button(mbox, "Delete");
    lv_obj_add_event_cb(delete_btn, on_delete_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_btn = lv_msgbox_add_footer_button(mbox, "Close");
    lv_obj_add_event_cb(close_btn, on_close_clicked, LV_EVENT_CLICKED, NULL);
}

static void on_card_clicked(lv_event_t *e) {
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);
    show_file_dialog(index);
}

static void populate_screen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr); // drop whatever's there from a previous build (e.g. after a delete)

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14161C), 0);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 8, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "MP3 Files (%s)", cached_source_label);
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

    if (mp3FileCount == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "No MP3 files found");
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
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

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

void build_main_screen(const char *source_label) {
    lv_style_init(&style_card);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_bg_color(&style_card, lv_color_hex(0x2A2E3A));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 10);
    lv_style_set_pad_row(&style_card, 2);

    strncpy(cached_source_label, source_label, sizeof(cached_source_label) - 1);
    cached_source_label[sizeof(cached_source_label) - 1] = '\0';

    populate_screen();
}

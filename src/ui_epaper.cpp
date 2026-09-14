#include "ui.h"

#include <Arduino.h>  // ESP.restart() - kRebootConfirm
#include <cstdio>
#include <cstring>

#include <Fonts/FreeSans9pt7b.h>

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
// Every screen is redrawn from scratch on each state change (fillScreen()
// + repopulate) rather than kept as a tree of show/hide-toggled widgets -
// simpler to keep correct, and cheap next to the e-paper refresh itself
// dominating either way. There's no retained widget tree at all here -
// GxEPD2/Adafruit_GFX only offers immediate-mode drawing into a shared
// framebuffer (see display.h's display_epd()/display_present()), so
// "rebuilding" a screen just means drawing over the same buffer again.
//
// Visual language: a black status bar pinned across the top (redrawn every
// call, since there's no persistent-widget concept to spare it from a
// clean), plain square-bordered rows for every list/menu row (inverted
// black-on-white when selected - the only focus indicator this UI has, in
// place of touch highlighting), and a bordered info-card for every
// message-only screen. Every row/card gets a short plain-text token in
// place of LVGL's old built-in symbol-font icons (no icon font exists in
// Adafruit_GFX) so screens still read at a glance instead of as walls of
// plain text.
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
    kDetails,
    kSleeping,
    kForgetWifiConfirm,
    kRebootConfirm,
};

static const int16_t HEADER_H = 20;
static const int16_t ROW_H = 20;
static const int16_t HINT_H = 30; // fits add_hint()'s two wrapped lines
static const int16_t BODY_TOP = HEADER_H;
// kList reserves its own top row (below) for the Audio/Text mode header,
// on top of HEADER_H/HINT_H.
static const int VISIBLE_ROWS = (SCREEN_H - HEADER_H - HINT_H - ROW_H) / ROW_H;

// Nominal line heights used for layout math (card sizing, hint-bar
// centering) - a fixed value rather than each line's actual measured ink
// height, so spacing stays consistent whether or not a given line has
// ascenders/descenders. Tuned for FreeSans9pt7b (body) / the built-in 6x8
// font (small) - LVGL's old Montserrat metrics don't carry over, expect to
// retune these once seen on real hardware.
static const int16_t BODY_LINE_H = 18;
static const int16_t SMALL_LINE_H = 10;

// Returns the shared GxEPD2 display object - a thin local wrapper so every
// draw call below reads as Epd().something() instead of the longer
// display_epd().something().
static EpdDisplay &Epd() { return display_epd(); }

static char header_text[64] = "";
static uint8_t battery_percent_val = 255; // sentinel - forces the first ui_set_battery_percent() paint
static bool sd_present = false;
static Screen state = Screen::kNoCard;
static bool screen_ready = false; // true once build_main_screen() has run - guards the no-op-before-that contract several ui.h functions document

// kList. showing_audio_files: true while mp3Files/mp3FileCount hold
// AUDIO_EXTS, false while showing .txt - toggled by a long Next press
// (see ui_process_input()'s kList case).
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

// kWifiSetup
static char wifi_setup_ssid[64];

// kTranscribeProgress / kTranscribeResult
static char transcribe_filename[64];
static bool transcribe_ok = false;
static char transcribe_message[128];

static void render_body();

// -----------------------------------------------------------------------
// Drawing primitives - Adafruit_GFX has no ellipsis-truncation or
// word-wrap of its own (LV_LABEL_LONG_DOT/LV_LABEL_LONG_WRAP's
// replacements), and no notion of "ink top-left" positioning (a GFXfont's
// setCursor() is baseline-relative, the built-in font's is cell-top-left-
// relative) - these wrap getTextBounds() once so every caller below can
// just say "put this text's ink at (x, y)" or "centered at x" regardless
// of which font is active.
// -----------------------------------------------------------------------

static void use_small_font() {
    Epd().setFont(nullptr);
    Epd().setTextSize(1);
}

static void use_body_font() {
    Epd().setFont(&FreeSans9pt7b);
    Epd().setTextSize(1);
}

static void text_ink_size(const char *text, uint16_t *w, uint16_t *h) {
    int16_t x1, y1;
    Epd().getTextBounds(text, 0, 0, &x1, &y1, w, h);
}

// Draws `text` with its ink's top-left corner at (x, y).
static void draw_text(int16_t x, int16_t y, const char *text) {
    int16_t x1, y1;
    uint16_t w, h;
    Epd().getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    Epd().setCursor(x - x1, y - y1);
    Epd().print(text);
}

// Draws `text` horizontally centered on `centerX`, ink's top at `y`.
static void draw_text_centered(int16_t centerX, int16_t y, const char *text) {
    int16_t x1, y1;
    uint16_t w, h;
    Epd().getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    Epd().setCursor(centerX - (int16_t)(w / 2) - x1, y - y1);
    Epd().print(text);
}

// Truncates `text` into `buf` (size `bufSize`) so it fits `maxW` px in the
// current font, appending "..." if it had to cut anything.
static void truncate_to_width(const char *text, int16_t maxW, char *buf, size_t bufSize) {
    if (bufSize == 0) return;
    uint16_t w, h;
    text_ink_size(text, &w, &h);
    if (w <= (uint16_t)maxW) {
        strncpy(buf, text, bufSize - 1);
        buf[bufSize - 1] = '\0';
        return;
    }
    size_t len = strlen(text);
    for (size_t cut = len; cut > 0; cut--) {
        char tmp[80];
        size_t n = cut < sizeof(tmp) - 4 ? cut : sizeof(tmp) - 4;
        memcpy(tmp, text, n);
        strcpy(tmp + n, "...");
        text_ink_size(tmp, &w, &h);
        if (w <= (uint16_t)maxW) {
            strncpy(buf, tmp, bufSize - 1);
            buf[bufSize - 1] = '\0';
            return;
        }
    }
    strncpy(buf, "...", bufSize - 1);
    buf[bufSize - 1] = '\0';
}

// Greedy word-wrap of `text` into up to `maxLines` lines (each up to 95
// chars), breaking on spaces so each line's rendered width fits `maxW` px
// in the current font. Recognizes embedded "\n" as an explicit line break
// too (kDetails' filename/created/size message uses one).
static int wrap_text(const char *text, int16_t maxW, char lines[][96], int maxLines) {
    int lineCount = 0;
    const char *p = text;
    while (*p && lineCount < maxLines) {
        while (*p == ' ') p++;
        if (*p == '\n') {
            p++;
            continue;
        }
        if (!*p) break;

        char *line = lines[lineCount];
        line[0] = '\0';
        while (*p && *p != '\n') {
            const char *wordEnd = p;
            while (*wordEnd && *wordEnd != ' ' && *wordEnd != '\n') wordEnd++;
            size_t wordLen = wordEnd - p;

            char candidate[96];
            if (line[0] == '\0') {
                size_t n = wordLen < sizeof(candidate) - 1 ? wordLen : sizeof(candidate) - 1;
                memcpy(candidate, p, n);
                candidate[n] = '\0';
            } else {
                snprintf(candidate, sizeof(candidate), "%s %.*s", line, (int)wordLen, p);
            }

            uint16_t w, h;
            text_ink_size(candidate, &w, &h);
            if (w > (uint16_t)maxW && line[0] != '\0') break; // this word doesn't fit - close the line out here
            strncpy(line, candidate, 95);
            line[95] = '\0';
            p = wordEnd;
            while (*p == ' ') p++;
            if (w > (uint16_t)maxW) break; // even alone it overflows - take it as-is rather than loop forever
        }
        lineCount++;
    }
    return lineCount;
}

// -----------------------------------------------------------------------
// Screen-building helpers
// -----------------------------------------------------------------------

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

// Draws the black status bar pinned across the top: left-aligned status
// text (truncated to whatever's left of the right-side icons), and
// right-aligned battery (drawn as an outline+proportional-fill bar, not a
// text token - see the icon-token map's comment in render_body()) plus an
// "SD" token if a card's present. Redrawn on every render_body() call,
// since there's no persistent-widget concept to spare it from a clean.
static void draw_header() {
    Epd().fillRect(0, 0, SCREEN_W, HEADER_H, GxEPD_BLACK);
    Epd().setTextColor(GxEPD_WHITE);
    use_small_font();

    const int16_t batteryW = 22, batteryH = 10;
    const int16_t pad = 6;
    int16_t rightX = SCREEN_W - pad - batteryW;

    char sdToken[3] = "";
    uint16_t sdW = 0, sdH = 0;
    if (sd_present) {
        strcpy(sdToken, "SD");
        text_ink_size(sdToken, &sdW, &sdH);
        rightX -= (sdW + 6);
    }

    int16_t batteryX = rightX;
    int16_t batteryY = (HEADER_H - batteryH) / 2;
    Epd().drawRect(batteryX, batteryY, batteryW - 2, batteryH, GxEPD_WHITE);
    Epd().fillRect(batteryX + batteryW - 2, batteryY + batteryH / 2 - 2, 2, 4, GxEPD_WHITE); // battery "nub"
    uint8_t pct = battery_percent_val > 100 ? 100 : battery_percent_val;
    int16_t fillW = ((batteryW - 4 - 2) * pct) / 100;
    if (fillW > 0) Epd().fillRect(batteryX + 2, batteryY + 2, fillW, batteryH - 4, GxEPD_WHITE);

    if (sd_present) {
        draw_text(SCREEN_W - pad - sdW, (HEADER_H - sdH) / 2, sdToken);
    }

    int16_t statusMaxW = rightX - 6 - 6;
    char truncated[80];
    truncate_to_width(header_text, statusMaxW, truncated, sizeof(truncated));
    uint16_t tw, th;
    text_ink_size(truncated, &tw, &th);
    draw_text(6, (HEADER_H - th) / 2, truncated);
}

// One square-bordered row: a leading icon token plus label text,
// left-aligned, inverted (black bg, white text) when selected - the only
// "focus" indicator this UI has. x/y/w let this serve both the full-width
// list (x=4, w=SCREEN_W-8) and menu rows indented inside a bordered panel
// (see render_option_menu()).
static void add_row(int16_t x, int16_t y, int16_t w, const char *icon, const char *text, bool selected) {
    int16_t h = ROW_H - 2;
    Epd().fillRect(x, y, w, h, selected ? GxEPD_BLACK : GxEPD_WHITE);
    Epd().drawRect(x, y, w, h, GxEPD_BLACK);
    Epd().setTextColor(selected ? GxEPD_WHITE : GxEPD_BLACK);
    use_body_font();

    char label[80];
    snprintf(label, sizeof(label), "%s  %s", icon, text);
    char truncated[80];
    truncate_to_width(label, w - 12, truncated, sizeof(truncated));
    uint16_t tw, th;
    text_ink_size(truncated, &tw, &th);
    draw_text(x + 6, y + (h - th) / 2, truncated);
}

// kList's own top row: an icon token, the Audio/Text mode label and file
// count, with a bottom border separating it from the rows below - not a
// row itself (never selectable), so it's built directly rather than
// through add_row(). `scrollable` - true once the list has more rows than
// VISIBLE_ROWS can show at once - draws a small "v" at the row's right
// edge as the only hint that Next still reveals more: this list has no
// scrollbar, and Next wraps around rather than stopping at the last item
// (see ui_process_input()'s kList case), so it stays fixed rather than
// tracking top_index/whether the view is currently at the bottom - there's
// always "more" to scroll to either way.
static void render_list_header(size_t count, bool scrollable) {
    Epd().drawFastHLine(0, BODY_TOP + ROW_H - 1, SCREEN_W, GxEPD_BLACK);
    Epd().setTextColor(GxEPD_BLACK);
    use_body_font();

    char label[48];
    snprintf(label, sizeof(label), "%s  %s (%u)", showing_audio_files ? "AUD" : "TXT",
              showing_audio_files ? "Audio Files" : "Text Files", (unsigned)count);
    uint16_t tw, th;
    text_ink_size(label, &tw, &th);
    draw_text(6, BODY_TOP + (ROW_H - th) / 2, label);

    if (scrollable) {
        const char *more = "v";
        text_ink_size(more, &tw, &th);
        draw_text(SCREEN_W - 6 - tw, BODY_TOP + (ROW_H - th) / 2, more);
    }
}

// A square-bordered card centered in the body area, with an optional big
// icon token above a wrapped message - the info/dialog counterpart to
// add_row()'s list rows, used by every message-only screen below.
static void add_info_card(const char *icon, const char *text) {
    const int16_t pad = 10;
    const int16_t rowGap = 6;
    const int16_t card_w = SCREEN_W - 24;
    const int16_t msgMaxW = card_w - pad * 2;

    char lines[6][96];
    int lineCount = wrap_text(text, msgMaxW, lines, 6);
    if (lineCount == 0) lineCount = 1; // always leave room for at least one (possibly blank) line

    use_body_font();
    bool hasIcon = icon && icon[0];
    uint16_t iconW = 0, iconH = 0;
    if (hasIcon) {
        Epd().setTextSize(2);
        text_ink_size(icon, &iconW, &iconH);
        Epd().setTextSize(1);
    }

    int16_t contentH = (hasIcon ? iconH + rowGap : 0) + lineCount * BODY_LINE_H;
    int16_t card_h = pad * 2 + contentH;
    int16_t card_x = (SCREEN_W - card_w) / 2;
    int16_t card_y = BODY_TOP + (SCREEN_H - BODY_TOP - card_h) / 2 - 8; // slightly above center, to balance against the hint bar below
    if (card_y < BODY_TOP + 2) card_y = BODY_TOP + 2;

    Epd().fillRect(card_x, card_y, card_w, card_h, GxEPD_WHITE);
    Epd().drawRect(card_x, card_y, card_w, card_h, GxEPD_BLACK);
    Epd().drawRect(card_x + 1, card_y + 1, card_w - 2, card_h - 2, GxEPD_BLACK); // 2px border

    Epd().setTextColor(GxEPD_BLACK);
    int16_t y = card_y + pad;
    if (hasIcon) {
        Epd().setTextSize(2);
        draw_text_centered(card_x + card_w / 2, y, icon);
        Epd().setTextSize(1);
        y += iconH + rowGap;
    }
    for (int i = 0; i < lineCount; i++) {
        uint16_t lw, lh;
        text_ink_size(lines[i], &lw, &lh);
        draw_text_centered(card_x + card_w / 2, y + (BODY_LINE_H - lh) / 2, lines[i]);
        y += BODY_LINE_H;
    }
}

// Wraps onto up to two lines instead of running off the 200px panel edge -
// callers keep hint text short enough to fit HINT_H at that wrap width.
// The top border marks it off as a distinct status strip rather than
// trailing text.
static void add_hint(const char *text) {
    int16_t barY = SCREEN_H - HINT_H;
    Epd().drawFastHLine(0, barY, SCREEN_W, GxEPD_BLACK);
    Epd().setTextColor(GxEPD_BLACK);
    use_small_font();

    char lines[2][96];
    int lineCount = wrap_text(text, SCREEN_W - 8, lines, 2);
    int16_t blockH = lineCount * SMALL_LINE_H;
    int16_t y = barY + (HINT_H - blockH) / 2;
    for (int i = 0; i < lineCount; i++) {
        uint16_t lw, lh;
        text_ink_size(lines[i], &lw, &lh);
        draw_text_centered(SCREEN_W / 2, y + (SMALL_LINE_H - lh) / 2, lines[i]);
        y += SMALL_LINE_H;
    }
}

// Shared by kMainMenu/kActionMenu/kDeleteConfirm/kForgetWifiConfirm/
// kRebootConfirm - a bordered panel holding a title line plus a
// cycle-and-confirm option list, one icon+label row per option (see
// add_row()).
static void render_option_menu(const char *title, const char *const *icons, const char *const *options, int count) {
    const int16_t pad = 6;
    const int16_t title_h = 20;
    const int16_t panel_w = SCREEN_W - 16;
    const int16_t panel_h = pad * 2 + title_h + count * ROW_H;
    const int16_t panel_x = (SCREEN_W - panel_w) / 2;
    const int16_t panel_y = BODY_TOP + 12;

    Epd().fillRect(panel_x, panel_y, panel_w, panel_h, GxEPD_WHITE);
    Epd().drawRect(panel_x, panel_y, panel_w, panel_h, GxEPD_BLACK);
    Epd().drawRect(panel_x + 1, panel_y + 1, panel_w - 2, panel_h - 2, GxEPD_BLACK);

    use_body_font();
    Epd().setTextColor(GxEPD_BLACK);
    char titleTrunc[64];
    truncate_to_width(title, panel_w - 12, titleTrunc, sizeof(titleTrunc));
    uint16_t tw, th;
    text_ink_size(titleTrunc, &tw, &th);
    draw_text_centered(panel_x + panel_w / 2, panel_y + (title_h - th) / 2, titleTrunc);

    Epd().drawFastHLine(panel_x + 6, panel_y + title_h - 6, panel_w - 12, GxEPD_BLACK);

    int16_t y = panel_y + title_h;
    for (int i = 0; i < count; i++) {
        add_row(panel_x + 6, y, panel_w - 12, icons[i], options[i], i == menu_index);
        y += ROW_H;
    }

    add_hint("Next: cycle   Select: choose, hold: back");
}

// Icon tokens below replace LVGL's old built-in symbol-font glyphs
// (LV_SYMBOL_*) - Adafruit_GFX has no icon font, so every icon is a short
// plain-text token instead (battery is the one exception - see
// draw_header()'s drawn outline+fill bar).
static void render_body() {
    Epd().fillScreen(GxEPD_WHITE);
    draw_header();

    switch (state) {
        case Screen::kNoCard:
            add_info_card("SD", "Insert an SD card to see your audio files");
            break;

        case Screen::kList: {
            size_t count = list_item_count();
            render_list_header(mp3FileCount, count > (size_t)VISIBLE_ROWS);
            if (count == 0) {
                add_info_card(showing_audio_files ? "AUD" : "TXT",
                               showing_audio_files ? "No audio files on the SD card" : "No text files on the SD card");
                add_hint("Sel(hold): menu   Next(hold): switch");
                break;
            }
            clamp_selection();
            bool recordOption = has_record_option();
            int16_t y = BODY_TOP + ROW_H;
            for (size_t i = top_index; i < count && (i - top_index) < (size_t)VISIBLE_ROWS; i++) {
                const char *label = (recordOption && i == 0) ? "Record new" : mp3Files[recordOption ? i - 1 : i].filename;
                const char *icon = (recordOption && i == 0) ? "+" : (showing_audio_files ? "AUD" : "TXT");
                add_row(4, y, SCREEN_W - 8, icon, label, i == selected_index);
                y += ROW_H;
            }
            add_hint("Next: move, hold: switch   Sel: open, hold: menu");
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
            const char *icons[] = {"R", "WiFi", "PWR", "X"};
            const char *options[] = {"Refresh", online ? "Offline" : "Online", "Reboot", "Close"};
            render_option_menu("Menu", icons, options, 4);
            break;
        }

        case Screen::kRebootConfirm: {
            static const char *icons[] = {"PWR", "X"};
            static const char *options[] = {"Confirm reboot", "Cancel"};
            render_option_menu("Reboot device?", icons, options, 2);
            break;
        }

        case Screen::kActionMenu: {
            // Play/Transcription only make sense for audio files, not the
            // .txt transcripts this same list shows when toggled.
            if (showing_audio_files) {
                static const char *icons[] = {">", "T", "i", "Del", "X"};
                static const char *options[] = {"Play", "Transcribe", "Details", "Delete", "Cancel"};
                render_option_menu(active_filename, icons, options, 5);
            } else {
                static const char *icons[] = {"i", "Del", "X"};
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
            add_info_card("i", msg);
            add_hint("Select: close");
            break;
        }

        case Screen::kDeleteConfirm: {
            char title[96];
            snprintf(title, sizeof(title), "Delete %s?", active_filename);
            static const char *icons[] = {"Del", "X"};
            static const char *options[] = {"Confirm delete", "Cancel"};
            render_option_menu(title, icons, options, 2);
            break;
        }

        case Screen::kForgetWifiConfirm: {
            static const char *icons[] = {"Del", "X"};
            static const char *options[] = {"Forget & reboot", "Cancel"};
            render_option_menu("Forget saved WiFi?", icons, options, 2);
            break;
        }

        case Screen::kPlaying: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Playing %s...", active_filename);
            add_info_card(">", msg);
            add_hint("Select: stop");
            break;
        }

        case Screen::kRecording: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Recording %s...", active_filename);
            add_info_card("AUD", msg);
            add_hint("Select: stop");
            break;
        }

        case Screen::kMicError:
            add_info_card("!", mic_last_error());
            add_hint("Select: close");
            break;

        case Screen::kWifiSetup: {
            char msg[128];
            snprintf(msg, sizeof(msg), "Join WiFi network \"%s\" from your phone or laptop to set up this device's WiFi.", wifi_setup_ssid);
            add_info_card("WiFi", msg);
            break;
        }

        case Screen::kTranscribeProgress: {
            char msg[96];
            snprintf(msg, sizeof(msg), "Transcribing %s...", transcribe_filename);
            add_info_card("R", msg);
            break;
        }

        case Screen::kTranscribeResult:
            add_info_card(transcribe_ok ? "OK" : "!", transcribe_message);
            add_hint("Select: close");
            break;

        case Screen::kSleeping:
            add_info_card("PWR", "Sleeping...\nHold Select to wake");
            break;
    }
}

void build_main_screen(bool sdPresent) {
    sd_present = sdPresent;
    header_text[0] = '\0';
    battery_percent_val = 255; // sentinel - force the next ui_set_battery_percent() paint

    selected_index = 0;
    top_index = 0;
    state = sd_present ? Screen::kList : Screen::kNoCard;
    screen_ready = true;
    render_body();
    display_present(false); // first paint of the session: full refresh
}

void ui_set_wifi_status(const char *text) {
    if (!screen_ready) return;
    strncpy(header_text, text, sizeof(header_text) - 1);
    header_text[sizeof(header_text) - 1] = '\0';
    render_body();
    display_present(true);
}

void ui_set_battery_percent(uint8_t percent) {
    if (!screen_ready) return;
    if (percent > 100) percent = 100;
    if (percent == battery_percent_val) return; // unchanged - skip the full e-paper repaint
    battery_percent_val = percent;
    render_body();
    display_present(true);
}

void ui_show_wifi_setup_dialog(const char *setup_ssid) {
    if (state == Screen::kWifiSetup) return; // already shown - see ui.h's contract
    strncpy(wifi_setup_ssid, setup_ssid, sizeof(wifi_setup_ssid) - 1);
    wifi_setup_ssid[sizeof(wifi_setup_ssid) - 1] = '\0';
    state = Screen::kWifiSetup;
    render_body();
    display_present(true);
}

void ui_hide_wifi_setup_dialog() {
    if (state != Screen::kWifiSetup) return;
    state = sd_present ? Screen::kList : Screen::kNoCard;
    render_body();
    display_present(true);
}

// No Settings view here to refresh a retry button on - see ui.h's comment.
void ui_refresh_wifi_retry_button() {}

void ui_show_transcribe_progress(const char *filename) {
    strncpy(transcribe_filename, filename, sizeof(transcribe_filename) - 1);
    transcribe_filename[sizeof(transcribe_filename) - 1] = '\0';
    state = Screen::kTranscribeProgress;
    render_body();
    display_present(true);
}

void ui_show_transcribe_result(bool ok, const char *message) {
    transcribe_ok = ok;
    strncpy(transcribe_message, message, sizeof(transcribe_message) - 1);
    transcribe_message[sizeof(transcribe_message) - 1] = '\0';
    state = Screen::kTranscribeResult;
    render_body();
    display_present(true);
}

bool ui_is_sleep_blocked() {
    return state == Screen::kRecording || state == Screen::kPlaying || state == Screen::kTranscribeProgress;
}

void ui_show_sleep_screen() {
    state = Screen::kSleeping;
    render_body();
    display_present(true);
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
        display_present(true);
    }
    if (state == Screen::kRecording && !mic_is_recording()) {
        // mic_process() force-stopped on its own (I2S read error) - same
        // idea as the speaker_is_playing() check above, but recording
        // failing mid-way is worth surfacing rather than just dropping
        // back to the list silently.
        state = Screen::kMicError;
        render_body();
        display_present(true);
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
        display_present(true);
        return;
    }
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
        display_present(true);
    }

    if (nextEv == DisplayButtonEvent::kNone && selEv == DisplayButtonEvent::kNone) return;
    sleep_reset_activity(); // any button edge counts as activity - see sleep.h

    bool redraw = false;
    switch (state) {
        case Screen::kNoCard:
            break; // nothing to navigate - insert a card and reboot

        case Screen::kList:
            if (nextEv == DisplayButtonEvent::kLong) {
                select_press_pending = false; // leaving kList's row set - drop any held-back press
                showing_audio_files = !showing_audio_files;
                load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
                selected_index = 0;
                top_index = 0;
                redraw = true;
                break;
            }
            if (selEv == DisplayButtonEvent::kLong) {
                // Covers both the empty- and non-empty-list case (Refresh
                // is one of the menu's own options), so no separate
                // rescan-on-long-press branch is needed below anymore.
                select_press_pending = false; // leaving kList - drop any held-back press
                state = Screen::kMainMenu;
                menu_index = 0;
                redraw = true;
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
                        redraw = true;
                        break;
                    }
                    selected_index = (selected_index + 1) % count;
                    redraw = true;
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
                        redraw = true;
                    } else if (select_press_pending) {
                        // Second Select short-press within the window -
                        // double-press gesture: jump to the Record row
                        // instead of opening this row's action menu.
                        select_press_pending = false;
                        selected_index = 0;
                        redraw = true;
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
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                redraw = true;
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
                redraw = true;
            }
            break;
        }

        // Reboot needs its own confirm - unlike Refresh/Offline-Online,
        // it's disruptive enough (drops whatever's on screen, same as a
        // power cycle) to warrant the same guard as Delete/Forget-WiFi
        // below rather than firing straight off the menu row.
        case Screen::kRebootConfirm:
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    // Never returns - no state/render needed after.
                    ESP.restart();
                }
                state = sd_present ? Screen::kList : Screen::kNoCard;
                redraw = true;
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
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (showing_audio_files && menu_index == 0) {
                    speaker_play(active_filename);
                    state = Screen::kPlaying;
                    redraw = true;
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
                    redraw = true;
                } else if (menu_index == (showing_audio_files ? 3 : 1)) {
                    state = Screen::kDeleteConfirm;
                    menu_index = 0;
                    redraw = true;
                } else {
                    state = Screen::kList;
                    redraw = true;
                }
            }
            break;
        }

        case Screen::kDeleteConfirm:
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = Screen::kList;
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    delete_file(active_filename);
                    load_file_catalog(showing_audio_files ? AUDIO_EXTS : ".txt");
                    selected_index = 0;
                    top_index = 0;
                }
                state = Screen::kList;
                redraw = true;
            }
            break;

        case Screen::kPlaying:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                speaker_stop();
                state = sd_present ? Screen::kList : Screen::kNoCard;
                redraw = true;
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
                redraw = true;
            }
            break;

        case Screen::kMicError:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                redraw = true;
            }
            break;

        case Screen::kWifiSetup:
            break; // informational only - see ui.h's contract

        case Screen::kTranscribeProgress:
            break; // informational only, until transcribe_process_pending() replaces it

        case Screen::kTranscribeResult:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                redraw = true;
            }
            break;

        case Screen::kDetails:
            if (selEv == DisplayButtonEvent::kShort || selEv == DisplayButtonEvent::kLong) {
                state = Screen::kActionMenu;
                redraw = true;
            }
            break;

        case Screen::kSleeping:
            break; // device deep-sleeps right after showing this - never reached

        case Screen::kForgetWifiConfirm:
            if (nextEv == DisplayButtonEvent::kShort) {
                menu_index = (menu_index + 1) % 2;
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kLong) {
                state = sd_present ? Screen::kList : Screen::kNoCard;
                redraw = true;
            } else if (selEv == DisplayButtonEvent::kShort) {
                if (menu_index == 0) {
                    // Never returns (ESP.restart()) - no state/render
                    // needed after.
                    wifi_forget_and_reboot();
                }
                state = sd_present ? Screen::kList : Screen::kNoCard;
                redraw = true;
            }
            break;
    }

    if (redraw) {
        render_body();
        display_present(true);
    }
}

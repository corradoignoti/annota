#pragma once

#include <lvgl.h>

// Builds the main screen: with sd_present, a title plus a scrollable list
// of rounded cards, one per entry in mp3Files/mp3FileCount (see
// storage.h); without it, a message inviting the user to insert an SD
// card. Call once, after display_init() and load_mp3_catalog().
void build_main_screen(bool sd_present);

// Updates the WiFi status line at the top of the main screen and forces
// one LVGL repaint, so the text is visible even if called while something
// else (e.g. WiFiManager's captive portal) is blocking loop(). No-op if
// called before build_main_screen(). Pass "" to clear it.
void ui_set_wifi_status(const char *text);

// Shows a modal dialog, floated above whatever's on screen, inviting the
// user to join the given setup-AP SSID and configure WiFi from there.
// Forces one LVGL repaint. Call ui_hide_wifi_setup_dialog() once
// configuration succeeds; calling this again while already shown is a
// no-op.
void ui_show_wifi_setup_dialog(const char *setup_ssid);

// Removes the dialog shown by ui_show_wifi_setup_dialog() or
// ui_show_wifi_timeout_dialog() and repaints. No-op if neither is
// currently shown.
void ui_hide_wifi_setup_dialog();

// Shows a modal dialog warning that WiFi connection attempts gave up
// after timing out, with a Close button wired to close_cb (fires on
// LV_EVENT_CLICKED) - dismissing it is all this dialog does; reconnecting
// is a separate, explicit action via the Settings "Reconnect WiFi"
// button (see wifi_request_reconnect()). Forces one LVGL repaint. Same
// single-dialog slot as ui_show_wifi_setup_dialog() - showing one while
// the other is up is not supported; hide with ui_hide_wifi_setup_dialog().
void ui_show_wifi_timeout_dialog(lv_event_cb_t close_cb);

// Re-checks live WiFi status and reflects it: disables the Settings
// view's "Reconnect WiFi" button while already connected, and replaces
// a stale top-of-screen status line with an explicit offline notice if
// the connection has actually dropped (the device just keeps working
// offline either way - this is purely informational). No-op before
// build_main_screen(), or if it was built without an SD card present
// (no Settings view/button exists in that case, though the status line
// still refreshes). Called by wifi_manager.cpp after every connect/retry
// attempt, in case Settings is already open when one finishes, and by
// the Settings view itself every time it's opened.
void ui_refresh_wifi_retry_button();

// Shows a modal "Transcribing <filename>..." status, floated above
// whatever's on screen (no buttons), and forces one LVGL repaint. Call
// only from loop() (via transcribe.h's transcribe_process_pending()),
// never from inside an LVGL event/timer callback - same reentrant-
// lv_timer_handler() reason as ui_show_wifi_setup_dialog(). Call
// ui_show_transcribe_result() once the attempt finishes.
void ui_show_transcribe_progress(const char *filename);

// Replaces the progress dialog with a result dialog (message plus a
// Close button) and forces one repaint. Same calling constraints as
// ui_show_transcribe_progress().
void ui_show_transcribe_result(bool ok, const char *message);

// esp32-s3-epaper154 only: polls the two onboard buttons and drives that
// board's list/action-menu state machine (see ui_epaper.cpp) - selecting a
// file's Transcribe action calls transcribe.h's transcribe_request() (safe
// here since, unlike ui.cpp's touch click handlers, this runs at loop()'s
// top level, not nested inside lv_timer_handler()); selecting Delete calls
// storage.h's delete_file() directly, same reasoning. Call once per loop()
// iteration, after lv_timer_handler(). No-op stub on esp32-cyd, where
// input flows through LVGL's touch indev instead - so callers don't need
// their own board #ifdef.
void ui_process_input();

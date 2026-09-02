#pragma once

#include <cstdint>
#include <lvgl.h>

// Builds the main screen: with sd_present, a title plus a scrollable list
// of rounded cards, one per entry in mp3Files/mp3FileCount (see
// storage.h); without it, a message inviting the user to insert an SD
// card. Call once, after display_init_panel()/display_init_input() and
// load_mp3_catalog().
void build_main_screen(bool sd_present);

// Updates the WiFi status line at the top of the main screen and forces
// one LVGL repaint, so the text is visible even if called while something
// else (e.g. WiFiManager's captive portal) is blocking loop(). No-op if
// called before build_main_screen(). Pass "" to clear it.
void ui_set_wifi_status(const char *text);

// Updates the battery percentage (icon + "NN%") shown in the header, and
// forces one repaint only if the displayed value actually changed. No-op
// before build_main_screen(). Call periodically from loop() (see
// main.cpp) - not every iteration: committing a full e-paper refresh for
// each 1% wobble would burn the panel's limited refresh life for no
// visible benefit. Reading itself comes from battery.h's
// battery_read_percent().
void ui_set_battery_percent(uint8_t percent);

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
// is a separate, explicit action via the web UI's "Reconnect WiFi" button
// (see wifi_request_reconnect()) - there's no on-screen Settings on this
// board. Forces one LVGL repaint. Same single-dialog slot as
// ui_show_wifi_setup_dialog() - showing one while the other is up is not
// supported; hide with ui_hide_wifi_setup_dialog().
void ui_show_wifi_timeout_dialog(lv_event_cb_t close_cb);

// Re-checks live WiFi status and replaces a stale top-of-screen status
// line with an explicit offline notice if the connection has actually
// dropped (the device just keeps working offline either way - this is
// purely informational). No-op before build_main_screen(). Called by
// wifi_manager.cpp after every connect/retry attempt. A no-op stub on
// this board - there's no on-screen "Reconnect WiFi" button to disable
// while connected (that lives only on the web UI's /settings page); kept
// so wifi_manager.cpp doesn't need a special case.
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

// Polls the two onboard buttons and drives the list/action-menu state
// machine (see ui_epaper.cpp) - selecting a file's Transcribe action calls
// transcribe.h's transcribe_request() (safe here since this runs at
// loop()'s top level, not nested inside lv_timer_handler()); selecting
// Delete calls storage.h's delete_file() directly, same reasoning. Call
// once per loop() iteration, after lv_timer_handler(). Also resets
// sleep.h's idle clock on any button edge.
void ui_process_input();

// True while a foreground operation that must not be interrupted by
// sleep.h's idle deep-sleep is in progress (recording, playing back, or
// transcribing). Checked by sleep_process_idle() every loop() iteration.
bool ui_is_sleep_blocked();

// Shows a plain "Sleeping..." message (no buttons - the device is about to
// deep-sleep) and forces one repaint. Called by sleep.cpp right before it
// tears down WiFi and calls esp_deep_sleep_start(); nothing clears this
// screen since the device never returns to loop() afterwards - waking is a
// full MCU reset that rebuilds the screen from scratch.
void ui_show_sleep_screen();

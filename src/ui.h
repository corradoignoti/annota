#pragma once

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

// Removes the dialog shown by ui_show_wifi_setup_dialog() and repaints.
// No-op if it isn't currently shown.
void ui_hide_wifi_setup_dialog();

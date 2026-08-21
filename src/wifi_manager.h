#pragma once

// Connects to the WiFi network saved in NVS (via WiFiManager). If none is
// saved, or the saved one can't be reached, opens a captive portal AP
// ("Annota-Setup", no password) and shows an on-screen dialog inviting the
// user to join it and pick a network from a phone/laptop; credentials
// entered there are saved for next boot. Blocks until a network is
// configured and connected - there's no timeout, so this only returns
// once WiFi is up. Call once, after build_main_screen() (the dialog is
// drawn onto whatever's already on screen) and Serial.begin().
bool wifi_connect();

// True once sync_clock_via_ntp() has successfully set the system clock from
// an NTP server after wifi_connect() succeeded; false before that (no WiFi
// yet, or the NTP request timed out) - the system clock may still be
// running from whatever it was at power-on in that case.
bool wifi_clock_synced();

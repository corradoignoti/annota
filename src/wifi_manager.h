#pragma once

// Connects to the WiFi network saved in NVS (via WiFiManager). If none is
// saved, or the saved one can't be reached, opens a captive portal AP
// ("Annota-Setup", no password) and shows an on-screen dialog inviting the
// user to join it and pick a network from a phone/laptop; credentials
// entered there are saved for next boot. Gives up after 5 minutes total
// (saved-network retry plus however long the portal stayed open) and
// shows a warning dialog with a Retry button instead of the IP - tapping
// it restarts the same 5-minute attempt. Blocks until either WiFi comes
// up or the 5 minutes run out. Call once, after build_main_screen() (the
// dialog is drawn onto whatever's already on screen) and Serial.begin().
bool wifi_connect();

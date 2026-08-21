#pragma once

// HTTP file manager for the SD card root (list / download / upload /
// delete), served over WiFi on port 80. Each request that touches the SD
// card borrows the shared SPI peripheral from touch for the duration of
// that one request (see storage.h's sd_begin() and display.h's
// display_suspend_touch()) - the screen stops responding to taps while a
// request is in flight, then touch resumes. Uploads/deletes only affect
// the SD card; the on-screen MP3 list isn't refreshed until reboot.
//
// Call web_server_start() once WiFi is up (after wifi_connect() returns
// true), and web_server_handle() every loop() iteration alongside
// lv_timer_handler().
void web_server_start();
void web_server_handle();

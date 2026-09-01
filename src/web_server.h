#pragma once

// HTTP file manager for the SD card root (list / download / upload /
// delete) at "/", plus the device's only Settings UI (WiFi/clock status,
// SD info, Reconnect WiFi, Delete WiFi Setup, AI provider API key) at
// "/settings", served over WiFi on port 80. Each request that touches the
// SD card brackets it with storage.h's sd_begin()/sd_end() and
// display.h's display_suspend_touch()/display_resume_touch() (no-ops on
// this board - there's no shared SPI peripheral to hand off - kept so a
// future board with one wouldn't need new call sites). Uploads/deletes
// only affect the SD card; the on-screen MP3 list isn't refreshed until
// reboot.
//
// Call web_server_start() once WiFi is up (after wifi_connect() returns
// true), and web_server_handle() every loop() iteration alongside
// lv_timer_handler().
void web_server_start();
void web_server_handle();

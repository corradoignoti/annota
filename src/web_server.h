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
// Call web_server_start() once WiFi is up (after wifi_start_boot_connect()
// resolves synchronously, or wifi_process_boot_connect() reports
// kConnected - see wifi_manager.h), and web_server_handle() every loop()
// iteration alongside lv_timer_handler().
void web_server_start();
void web_server_handle();

// True while a browser-initiated transcription is in flight: the window
// from GET /api/transcript-key (the browser fetching the API key to start)
// to POST /api/transcript (writing the result back) - see
// handle_get_transcript_key()'s comment. The actual upload/transcription
// happens entirely between the browser and the AI provider, not through
// this device, so no request lands here for the whole duration; ordinary
// with_activity()-driven resets can't cover that gap. Checked by sleep.cpp
// so idle deep-sleep doesn't cut WiFi out from under the browser mid-
// transcription. Self-clears after a safety-net timeout in case the
// browser never calls back (tab closed, network drop).
bool web_transcribe_in_progress();

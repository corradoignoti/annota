#pragma once

#include <cstddef>

// -----------------------------------------------------------------------
// OpenAI transcription: saves/reads the API key (Settings view) and
// uploads an SD-root .mp3 to OpenAI's audio transcription API, writing
// the result to a sibling .txt file - triggered by long-pressing a file
// card in ui.cpp.
// -----------------------------------------------------------------------

// Max length (including the trailing nul) accepted for a saved API key.
constexpr size_t OPENAI_API_KEY_MAX = 200;

// True once a non-empty API key has been saved via openai_set_api_key().
bool openai_has_api_key();

// Copies the saved API key into out (empty string if none saved yet).
void openai_get_api_key(char *out, size_t outLen);

// Saves the OpenAI API key to NVS (same "annota" Preferences namespace
// display.cpp uses for touch calibration, different key). An empty
// string clears it. Called by the Settings view's Save button.
void openai_set_api_key(const char *key);

// Requests that transcribe_process_pending() transcribe `filename` (an
// .mp3 on the SD root) the next time it's called from loop() - mirrors
// wifi_manager.h's wifi_request_reconnect()/wifi_process_pending_reconnect()
// pair: the actual work blocks and repaints the screen, which can't
// happen safely from inside the LVGL click handler (ui.cpp's long-press
// confirm dialog) that calls this, since that handler runs nested inside
// the very lv_timer_handler() call dispatching it, which refuses to run
// itself again while it's running. filename is copied, so the caller's
// buffer can be reused or go out of scope immediately after this returns.
void transcribe_request(const char *filename);

// Runs the transcription requested by transcribe_request(), if any - a
// no-op otherwise. Call once per loop() iteration, after
// lv_timer_handler() has returned, never nested inside an LVGL event or
// timer callback - same placement and reasoning as
// wifi_process_pending_reconnect(). Shows progress and the result via
// ui.h's ui_show_transcribe_progress()/ui_show_transcribe_result(), and
// pauses/resumes touch (display.h) around the SD+network work, same
// dance web_server.cpp's handlers do for their own SD access.
void transcribe_process_pending();

// The actual worker transcribe_process_pending() calls: uploads
// /`filename` to OpenAI's /v1/audio/transcriptions endpoint and writes
// the returned text to a sibling <basename>.txt file on the SD root,
// overwriting any existing one there. Requires WiFi already connected
// and an API key already saved - both are checked internally, and a
// short reason is copied into errOut/errOutLen on any failure (e.g.
// "WiFi not connected", "No OpenAI API key set (see Settings)", an SD
// error, or the API's own error message). Claims the SD card itself
// (storage.h's sd_begin()/sd_end()) for the duration of the upload, so
// callers running after boot (i.e. always, here) must pause/resume touch
// around it themselves - see storage.h's shared-SPI comment. Blocks for
// the duration of the SD read plus the OpenAI request/response.
bool transcribe_file(const char *filename, char *errOut, size_t errOutLen);

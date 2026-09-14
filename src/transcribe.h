#pragma once

#include <cstddef>

// -----------------------------------------------------------------------
// AI transcription: saves/reads a provider API key (web_server.cpp's
// /settings page) and uploads an SD-root audio file to that provider for
// transcription, writing the result to a sibling .txt file - triggered by
// selecting a file's Transcribe action in ui_epaper.cpp.
//
// Which provider is compiled in is a build-time choice, not a runtime
// one: platformio.ini's build_flags define exactly one `AI_PROVIDER_*`
// macro (e.g. `-D AI_PROVIDER_OPENAI=1`), and exactly one
// `transcribe_<provider>.cpp` file (guarded by `#ifdef AI_PROVIDER_*` so
// the others compile to nothing) implements ai_provider_name() and the
// functions below. transcribe.cpp itself is provider-agnostic - see its
// top for the compile-time check that exactly one provider is selected -
// and everything else in this codebase (ui_epaper.cpp, web_server.cpp)
// only ever calls the generic names here, never anything OpenAI-specific,
// so adding a new provider file plus a new build flag is the only change
// needed to switch.
// -----------------------------------------------------------------------

// Max length (including the trailing nul) accepted for a saved API key.
constexpr size_t AI_API_KEY_MAX = 200;

// Short display name of the compiled-in provider (e.g. "OpenAI"), used
// to label the API key field on the web UI's /settings page without it
// needing to know which provider is actually active.
const char *ai_provider_name();

// True once a non-empty API key has been saved via ai_provider_set_api_key().
bool ai_provider_has_api_key();

// Copies the saved API key into out (empty string if none saved yet).
void ai_provider_get_api_key(char *out, size_t outLen);

// Saves the provider API key to NVS. An empty string clears it. Called by
// web_server.cpp's /api/settings/ai-key handler.
void ai_provider_set_api_key(const char *key);

// Requests that transcribe_process_pending() transcribe `filename` (an
// audio file on the SD root) the next time it's called from loop() -
// mirrors wifi_manager.h's wifi_request_reconnect()/
// wifi_process_pending_reconnect() pair, keeping the actual (blocking,
// screen-repainting) work out of the button handler that calls this.
// filename is copied, so the caller's buffer can be reused or go out of
// scope immediately after this returns.
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

// The actual worker transcribe_process_pending() calls, implemented by
// whichever provider file is compiled in: uploads /`filename` to the
// provider's transcription API and writes the returned text to a
// sibling <basename>.txt file on the SD root, overwriting any existing
// one there. Requires WiFi already connected and an API key already
// saved - both are checked internally, and a short reason is copied
// into errOut/errOutLen on any failure (e.g. "WiFi not connected", "No
// <provider> API key set (see Settings)", an SD error, or the
// provider's own error message). Claims the SD card itself (storage.h's
// sd_begin()/sd_end()) for the duration of the upload, so callers
// running after boot (i.e. always, here) must pause/resume touch around
// it themselves - see storage.h's shared-SPI comment. Blocks for the
// duration of the SD read plus the request/response.
bool ai_transcribe_file(const char *filename, char *errOut, size_t errOutLen);

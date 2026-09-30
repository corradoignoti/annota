#pragma once

#include <cstddef>
#include <cstdint>

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

// Largest audio file the on-device Transcribe action will upload. The
// ESP32's own TLS upload is slow enough that bigger files risk running
// into the provider's request-body timeout, so ui_epaper.cpp refuses
// them up front and points the user at the web file manager's Transcribe
// button instead (that path uploads from the browser, not the ESP32).
static const uint32_t TRANSCRIBE_MAX_FILE_BYTES = 1024UL * 1024UL;

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

// When transcribe_process_pending() can't reach any saved network, it
// doesn't fail (and writes no error log) - it parks the request and sends
// the user to the on-device scan-and-join list (ui.h's
// ui_show_wifi_join_for_transcribe()) so they can pick an access point.
// These let wifi_manager.cpp/ui_epaper.cpp finish or abandon that parked
// request:
//
// True while a transcription is parked waiting for the user to join a
// network.
bool transcribe_is_waiting_for_wifi();
// Called by wifi_manager.cpp's wifi_process_pending_join() once its
// blocking connect finishes, only while transcribe_is_waiting_for_wifi().
// joined: re-queues the parked transcription (runs later in this same
// loop() pass, and powers WiFi back off afterwards since it was turned on
// for it); not joined: powers the radio off and shows a plain "could not
// join" result, no error log. Either way the request is no longer parked.
void transcribe_resume_after_join(bool joined);
// Drops the parked request (user backed out of the join list) and powers
// the radio back off. No-op if nothing is parked.
void transcribe_cancel_wifi_wait();

// Progress/diagnostics hooks, implemented in transcribe.cpp and called by
// the provider's ai_transcribe_file() while it runs - so providers can
// drive the progress screen and the failure log without knowing about
// ui.h. Only valid during a transcribe_process_pending() call.
// kSummarizing is optional - only providers that generate a title/abstract
// header for the transcript (see ai_transcribe_file() below) report it.
enum class TranscribePhase { kConnecting, kUploading, kWaiting, kSummarizing, kSaving };

// Repaints the progress screen's phase line (and hides the upload bar for
// any phase but kUploading).
void transcribe_report_phase(TranscribePhase phase);

// Reports upload progress for the current attempt (1-based) - repaints
// only when the percentage crosses the next 10% step or the attempt
// changes, since every e-paper refresh blocks for a noticeable while.
void transcribe_report_upload(size_t sent, size_t total, int attempt, int maxAttempts);

// Appends one printf-formatted line to the diagnostics log (and echoes it
// to Serial). On failure transcribe_process_pending() saves the log as
// <basename>_error.txt next to the audio file. Never pass the API key.
void transcribe_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// The actual worker transcribe_process_pending() calls, implemented by
// whichever provider file is compiled in: uploads /`filename` to the
// provider's transcription API and writes the returned text to a
// sibling <basename>.txt file on the SD root, overwriting any existing
// one there. Providers that support it (currently OpenAI) prepend an
// AI-generated title and abstract, each followed by a blank line, falling
// back to the plain transcript if that extra request fails. Requires WiFi already connected and an API key already
// saved - both are checked internally, and a short reason is copied
// into errOut/errOutLen on any failure (e.g. "WiFi not connected", "No
// <provider> API key set (see Settings)", an SD error, or the
// provider's own error message). Claims the SD card itself (storage.h's
// sd_begin()/sd_end()) for the duration of the upload, so callers
// running after boot (i.e. always, here) must pause/resume touch around
// it themselves - see storage.h's shared-SPI comment. Blocks for the
// duration of the SD read plus the request/response. Reports progress and
// logs diagnostics through the transcribe_report_*()/transcribe_log()
// hooks above.
bool ai_transcribe_file(const char *filename, char *errOut, size_t errOutLen);

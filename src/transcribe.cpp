#include "transcribe.h"

#include <cstring>

#include "display.h"
#include "ui.h"

// -----------------------------------------------------------------------
// Provider-agnostic half of transcribe.h: the deferred-dispatch pair
// (mirrors wifi_manager.cpp's reconnect pair, see transcribe_request()'s
// comment) that calls into whichever provider's ai_transcribe_file() is
// compiled in. ai_provider_name()/ai_provider_*_api_key()/
// ai_transcribe_file() itself live in transcribe_<provider>.cpp instead -
// see transcribe.h's top comment for how the provider is selected.
// -----------------------------------------------------------------------

// Extend this line when adding a provider (transcribe_<provider>.cpp,
// guarded by its own #ifdef AI_PROVIDER_<PROVIDER>) so a platformio.ini
// missing (or misspelling) its build flag fails at compile time here
// instead of as a confusing "undefined reference to ai_transcribe_file()"
// link error.
#if !defined(AI_PROVIDER_OPENAI)
#error "No AI_PROVIDER_* build flag defined - add one (e.g. -D AI_PROVIDER_OPENAI=1) to platformio.ini's build_flags to select a transcription provider."
#endif

static volatile bool transcribeRequested = false;
static char transcribeTargetFilename[64];

void transcribe_request(const char *filename) {
    strncpy(transcribeTargetFilename, filename, sizeof(transcribeTargetFilename) - 1);
    transcribeTargetFilename[sizeof(transcribeTargetFilename) - 1] = '\0';
    transcribeRequested = true;
}

void transcribe_process_pending() {
    if (!transcribeRequested) return;
    transcribeRequested = false;

    ui_show_transcribe_progress(transcribeTargetFilename);

    display_suspend_touch();
    char err[96];
    bool ok = ai_transcribe_file(transcribeTargetFilename, err, sizeof(err));
    display_resume_touch();

    ui_show_transcribe_result(ok, ok ? "Transcription saved." : err);
}

#pragma once

#include <Arduino.h>
#include <stdint.h>

// -----------------------------------------------------------------------
// Translations for everything user-visible: the e-paper UI (ui_epaper.cpp
// and the error/status strings other modules hand it) and the two web
// pages (web_server.cpp). One device-wide language, picked on the web
// Settings page and persisted in NVS (namespace "annota", key "lang" -
// same namespace as sleep.cpp's idle timeout).
//
// Every string lives in i18n_strings.def, one row per string carrying all
// supported languages - a row missing a column is a macro-arity compile
// error, so a new string can't ship untranslated. See CLAUDE.md's
// "Translations" section and tools/check_i18n.py.
// -----------------------------------------------------------------------

enum class Lang : uint8_t {
    kEn,
    kIt,
    kFr,
    kCount,
};

// Device-side string ids (TR rows of i18n_strings.def). Web-side (TRW)
// rows have no id here - they're only ever looked up by key in the
// browser (see i18n_write_web_js()).
enum class Str : uint16_t {
#define TR(id, en, it, fr) id,
#define TRW(key, en, it, fr)
#include "i18n_strings.def"
#undef TR
#undef TRW
    kCount,
};

// The current language's text for `id`. Never null.
const char *tr(Str id);

// Lazily loaded from NVS on first use, cached after that. Defaults to
// English.
Lang i18n_get_language();

// Persists to NVS and takes effect on the very next tr() call - callers
// that already painted something (the e-paper UI) repaint via ui.h's
// ui_request_rerender().
void i18n_set_language(Lang lang);

// "en" / "it" / "fr" - the web API's and <html lang>'s spelling.
const char *i18n_lang_code(Lang lang);

// Inverse of i18n_lang_code(). False (out untouched) for anything else.
bool i18n_parse_lang(const char *code, Lang &out);

// Appends the /i18n.js payload for the current language to `out`:
// window.I18N_LANG, window.I18N (every TRW row as key -> text) and the
// t()/applyI18n() helpers both web pages use.
void i18n_write_web_js(String &out);

#include "i18n.h"

#include <ArduinoJson.h>
#include <Preferences.h>

// Same NVS namespace as sleep.cpp's idle timeout.
static const char *NVS_KEY = "lang";

// kStrings[lang][id] - built column by column from i18n_strings.def's TR
// rows, so every Lang value has exactly Str::kCount entries.
#define TR(id, en, it, fr) en,
#define TRW(key, en, it, fr)
static const char *const kStringsEn[] = {
#include "i18n_strings.def"
};
#undef TR
#define TR(id, en, it, fr) it,
static const char *const kStringsIt[] = {
#include "i18n_strings.def"
};
#undef TR
#define TR(id, en, it, fr) fr,
static const char *const kStringsFr[] = {
#include "i18n_strings.def"
};
#undef TR
#undef TRW

static_assert(sizeof(kStringsEn) / sizeof(kStringsEn[0]) == (size_t)Str::kCount, "i18n: EN table size mismatch");
static_assert(sizeof(kStringsIt) / sizeof(kStringsIt[0]) == (size_t)Str::kCount, "i18n: IT table size mismatch");
static_assert(sizeof(kStringsFr) / sizeof(kStringsFr[0]) == (size_t)Str::kCount, "i18n: FR table size mismatch");

static const char *const *const kStrings[] = {kStringsEn, kStringsIt, kStringsFr};
static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == (size_t)Lang::kCount, "i18n: one table per Lang");

// Web-side (TRW) rows: key plus one column per language.
struct WebString {
    const char *key;
    const char *text[(size_t)Lang::kCount];
};
#define TR(id, en, it, fr)
#define TRW(key, en, it, fr) {#key, {en, it, fr}},
static const WebString kWebStrings[] = {
#include "i18n_strings.def"
};
#undef TR
#undef TRW

static const char *const kLangCodes[] = {"en", "it", "fr"};
static_assert(sizeof(kLangCodes) / sizeof(kLangCodes[0]) == (size_t)Lang::kCount, "i18n: one code per Lang");

// kCount = not loaded from NVS yet - never a valid stored value once
// loaded (see ensure_language_loaded()), doubles as the lazy-load sentinel.
static Lang currentLang = Lang::kCount;

static void ensure_language_loaded() {
    if (currentLang != Lang::kCount) return;
    Preferences prefs;
    prefs.begin("annota", true);
    uint8_t stored = prefs.getUChar(NVS_KEY, (uint8_t)Lang::kEn);
    prefs.end();
    currentLang = stored < (uint8_t)Lang::kCount ? (Lang)stored : Lang::kEn;
}

const char *tr(Str id) {
    size_t i = (size_t)id;
    if (i >= (size_t)Str::kCount) return "";
    const char *s = kStrings[(size_t)i18n_get_language()][i];
    // English fallback for a cell left blank - tools/check_i18n.py rejects
    // those, this only keeps a slip from painting an empty label.
    return (s && s[0]) ? s : kStringsEn[i];
}

Lang i18n_get_language() {
    ensure_language_loaded();
    return currentLang;
}

void i18n_set_language(Lang lang) {
    if (lang >= Lang::kCount) return;
    currentLang = lang;
    Preferences prefs;
    prefs.begin("annota", false);
    prefs.putUChar(NVS_KEY, (uint8_t)lang);
    prefs.end();
}

const char *i18n_lang_code(Lang lang) {
    return lang < Lang::kCount ? kLangCodes[(size_t)lang] : kLangCodes[0];
}

bool i18n_parse_lang(const char *code, Lang &out) {
    if (!code) return false;
    for (size_t i = 0; i < (size_t)Lang::kCount; i++) {
        if (strcmp(code, kLangCodes[i]) == 0) {
            out = (Lang)i;
            return true;
        }
    }
    return false;
}

// Loaded synchronously from both pages' <head>, before any of their own
// script runs. t(key, ...args) fills {0}, {1}, ... and falls back to the
// key itself for anything missing (so a gap shows up as a visible key, not
// a blank). applyI18n() fills the static markup: data-i18n -> textContent,
// data-i18n-ph -> placeholder, data-i18n-title -> title + aria-label.
static const char I18N_JS_HELPERS[] PROGMEM = R"rawliteral(
function t(key) {
  const s = window.I18N[key];
  if (s === undefined) return key;
  const args = arguments;
  return s.replace(/\{(\d+)\}/g, (m, i) => (args[+i + 1] === undefined ? m : String(args[+i + 1])));
}
function applyI18n() {
  document.documentElement.lang = window.I18N_LANG;
  document.querySelectorAll("[data-i18n]").forEach((e) => { e.textContent = t(e.dataset.i18n); });
  document.querySelectorAll("[data-i18n-ph]").forEach((e) => { e.placeholder = t(e.dataset.i18nPh); });
  document.querySelectorAll("[data-i18n-title]").forEach((e) => {
    e.title = t(e.dataset.i18nTitle);
    e.setAttribute("aria-label", e.title);
  });
}
)rawliteral";

void i18n_write_web_js(String &out) {
    size_t lang = (size_t)i18n_get_language();
    JsonDocument doc;
    for (const WebString &w : kWebStrings) {
        const char *s = w.text[lang];
        doc[w.key] = (s && s[0]) ? s : w.text[(size_t)Lang::kEn];
    }
    out += "window.I18N_LANG=\"";
    out += kLangCodes[lang];
    out += "\";\nwindow.I18N=";
    // Into its own String: ArduinoJson 7's serializeJson() replaces a
    // String's contents rather than appending, which would wipe the prefix.
    String json;
    serializeJson(doc, json);
    out += json;
    out += ";\n";
    out += FPSTR(I18N_JS_HELPERS);
}

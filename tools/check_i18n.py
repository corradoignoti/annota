#!/usr/bin/env python3
"""Consistency checks for src/i18n_strings.def (see src/i18n.h).

The X-macro table already makes a missing language column a compile
error; this catches what the compiler can't:
  - empty cells, rows that don't parse (e.g. literal concatenation);
  - TR rows: printf specifiers differing between languages (would read the
    wrong vararg at runtime), characters the e-paper fonts don't carry;
  - TRW rows: {n} placeholders differing between languages;
  - web keys used in web_server.cpp (t("key"), data-i18n*="key") that
    aren't defined, and defined ones nothing uses;
  - duplicate ids/keys;
  - (warning only) string literals passed straight to the e-paper UI's
    text helpers - likely a new string that skipped the table.

Stdlib only. Exit code 1 on any error. Run from anywhere:
    python3 tools/check_i18n.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
DEF = SRC / "i18n_strings.def"
WEB = SRC / "web_server.cpp"
LANGS = ("en", "it", "fr")

STR_LIT = r'"((?:[^"\\]|\\.)*)"'
ROW_RE = re.compile(r"^(TRW?)\(\s*(\w+)\s*," + r"\s*,".join([r"\s*" + STR_LIT] * len(LANGS)) + r"\s*\)\s*$")
PRINTF_RE = re.compile(r"%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diouxXeEfgGcsp])")
BRACE_RE = re.compile(r"\{\d+\}")
WEB_KEY_RE = re.compile(r'\bt\(\s*"(\w+)"|data-i18n(?:-ph|-title)?="(\w+)"')

# Calls whose first string argument lands on the e-paper screen.
UI_TEXT_CALLS = (
    "add_hint", "add_info_card", "render_list_header", "render_option_menu",
    "ui_set_wifi_status", "ui_show_transcribe_result", "set_err", "set_mic_error",
)
UI_LITERAL_RE = re.compile(r"\b(" + "|".join(UI_TEXT_CALLS) + r")\(([^;]*?)\"([A-Za-z][^\"]*)\"")


def font_ok(ch):
    cp = ord(ch)
    return cp < 0x80 or 0xC0 <= cp <= 0xFF


def main():
    errors, warnings = [], []
    rows = {"TR": {}, "TRW": {}}

    for lineno, raw in enumerate(DEF.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        m = ROW_RE.match(line)
        if not m:
            errors.append(f"{DEF.name}:{lineno}: unparseable row (one row per line, one \"...\" literal per language)")
            continue
        kind, ident, *texts = m.groups()
        where = f"{DEF.name}:{lineno} {ident}"
        if ident in rows[kind]:
            errors.append(f"{where}: duplicate {kind} id")
        rows[kind][ident] = lineno

        for lang, text in zip(LANGS, texts):
            if not text.strip():
                errors.append(f"{where}: empty '{lang}' text")

        if kind == "TR":
            ref = PRINTF_RE.findall(texts[0])
            for lang, text in zip(LANGS[1:], texts[1:]):
                if PRINTF_RE.findall(text) != ref:
                    errors.append(f"{where}: '{lang}' printf specifiers {PRINTF_RE.findall(text)} != en {ref}")
            for lang, text in zip(LANGS, texts):
                bad = sorted({c for c in text if not font_ok(c)})
                if bad:
                    errors.append(f"{where}: '{lang}' has characters the e-paper fonts lack: "
                                  + " ".join(f"U+{ord(c):04X}({c})" for c in bad))
        else:
            ref = sorted(BRACE_RE.findall(texts[0]))
            for lang, text in zip(LANGS[1:], texts[1:]):
                if sorted(BRACE_RE.findall(text)) != ref:
                    errors.append(f"{where}: '{lang}' placeholders {sorted(BRACE_RE.findall(text))} != en {ref}")

    web_src = WEB.read_text(encoding="utf-8")
    used = {a or b for a, b in WEB_KEY_RE.findall(web_src)}
    for key in sorted(used - rows["TRW"].keys()):
        errors.append(f"{WEB.name}: web key '{key}' used but not defined in {DEF.name}")
    for key in sorted(rows["TRW"].keys() - used):
        errors.append(f"{DEF.name}:{rows['TRW'][key]} {key}: web key defined but never used in {WEB.name}")

    tr_used = set()
    for path in sorted(SRC.glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        tr_used |= set(re.findall(r"Str::(\w+)", text))
        for lineno, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("//"):
                continue
            for m in UI_LITERAL_RE.finditer(line):
                warnings.append(f"{path.name}:{lineno}: literal passed to {m.group(1)}(): \"{m.group(3)}\" "
                                f"- move it to {DEF.name}?")
    for ident in sorted(rows["TR"].keys() - tr_used):
        errors.append(f"{DEF.name}:{rows['TR'][ident]} {ident}: device string defined but never used")

    for w in warnings:
        print("warning:", w)
    for e in errors:
        print("error:", e)
    print(f"{len(rows['TR'])} device + {len(rows['TRW'])} web strings, {len(LANGS)} languages: "
          f"{len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

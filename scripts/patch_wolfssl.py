"""PlatformIO pre-build step (see platformio.ini's extra_scripts).

wolfssl/Arduino-wolfSSL's own src/wolfssl.h defines
wolfSSL_Arduino_Serial_Print() at file scope, not just declares it - a bug
in that header, not this project's (wolfssl/wolfcrypt/logging.h already
has its own `extern WOLFSSL_API` declaration for it, and logging.c calls
it, so this was clearly meant to be a single out-of-line definition
somewhere, not a copy pasted into every translation unit that includes
the header). Anything that includes <WolfSSLClient.h> (which includes
wolfssl.h) from more than one .cpp file - true here since both a
transcribe_<provider>.cpp and ESP32-EasyWolfSSL's own WolfSSLClient.cpp do
- gets two external definitions of the same symbol and fails at link time
with "multiple definition of `wolfSSL_Arduino_Serial_Print'".

Marking it `inline` (tried first) avoids the duplicate-definition error,
but backfires the other way: since neither translation unit actually
calls the function itself, GCC's vague-linkage model drops it from both
object files as dead weight, and logging.c - which does call it, compiled
as plain C from a TU that never sees this Arduino-only header at all -
links with "undefined reference" instead.

The actual fix: give the header a suppression macro
(ANNOTA_WOLFSSL_SKIP_SERIAL_PRINT_DEFINITION) around the definition, and
define that macro in src/transcribe_openai.cpp and
src/transcribe_gemini.cpp before their #include <WolfSSLClient.h> (see
those files) so only WolfSSLClient.cpp's inclusion - the one file in this
project this patch can't reach, since it's inside a downloaded lib_deps
package - still provides the single real definition logging.c links
against.

Idempotent (checked before writing) and re-applied on every build, since
PlatformIO can silently redownload a clean copy of a lib_deps package at
any time.

Second, unrelated patch in this same script: xorlent/ESP32-EasyWolfSSL's
WolfSSLClient::connect() never calls wolfSSL_UseSNI(), so every TLS
handshake it does goes out with no SNI extension in the ClientHello at
all. Most hosts tolerate that (falling back to a default cert), but a
host fronted by Cloudflare - api.openai.com included - just drops the
handshake, which surfaces here as a plain connect() failure/"connection
refused" out of HTTPClient, indistinguishable from an actual TCP-level
refusal since neither WolfSSLClient nor HTTPClient say which stage failed
(see transcribe_openai.cpp's dropped-lastError() comment). Inserting the
call right after wolfSSL_SetIOWriteCtx() - present verbatim in both of
WolfSSLClient::connect()'s (const char*, ...) overloads - fixes it, but
only compiles to anything once HAVE_SNI is defined (platformio.ini's
build_flags), which the package's own user_settings.h never turns on.
"""

import re
from pathlib import Path

Import("env")  # noqa: F821 - PlatformIO injects this

GUARD = "ANNOTA_WOLFSSL_SKIP_SERIAL_PRINT_DEFINITION"
SNI_CALL_MARKER = "wolfSSL_UseSNI"


def patch_wolfssl_header():
    project_dir = Path(env["PROJECT_DIR"])  # noqa: F821
    for header in project_dir.glob(".pio/libdeps/*/Arduino-wolfSSL/src/wolfssl.h"):
        text = header.read_text()
        if GUARD in text:
            continue
        patched, n = re.subn(
            r"(inline )?(int wolfSSL_Arduino_Serial_Print\(const char \*const s\)\s*"
            r"\{.*?\n\};?\n)",
            f"#ifndef {GUARD}\n" r"\2" f"#endif // !{GUARD}\n",
            text,
            count=1,
            flags=re.DOTALL,
        )
        if n:
            header.write_text(patched)
            print(f"patch_wolfssl.py: guarded wolfSSL_Arduino_Serial_Print() definition in {header}")


def patch_easywolfssl_sni():
    project_dir = Path(env["PROJECT_DIR"])  # noqa: F821
    for src in project_dir.glob(".pio/libdeps/*/ESP32-EasyWolfSSL/src/WolfSSLClient.cpp"):
        text = src.read_text()
        if SNI_CALL_MARKER in text:
            continue
        needle = "    wolfSSL_SetIOWriteCtx(_ssl, (void*)this);\n"
        replacement = (
            needle
            + "\n"
            + "#ifdef HAVE_SNI\n"
            + "    wolfSSL_UseSNI(_ssl, WOLFSSL_SNI_HOST_NAME, host, (word16)strlen(host));\n"
            + "#endif // HAVE_SNI\n"
        )
        n = text.count(needle)
        if n:
            src.write_text(text.replace(needle, replacement))
            print(f"patch_wolfssl.py: added wolfSSL_UseSNI() to {n} connect() overload(s) in {src}")


patch_wolfssl_header()
patch_easywolfssl_sni()

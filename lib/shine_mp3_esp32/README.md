# shine_mp3_esp32 (vendored)

Fixed-point MP3 encoder used by `src/speaker.cpp`'s mic recording
(`mic_start_recording()`), esp32-s3-epaper154 only.

Vendored in-tree (rather than pulled via `platformio.ini`'s `lib_deps`,
this project's usual way of pinning an external dependency - see e.g. its
XPT2046_Touchscreen comment) because the upstream repo isn't safe to build
as-is: PlatformIO's Library Dependency Finder compiles every `.c` file it
finds for a flat-layout library with no `library.json`, and upstream's
`example_freeRTOS_task_snippet.c` is a paste-into-your-own-file snippet,
not a real translation unit - it doesn't compile stand-alone (no
`#include`s of its own) and fails the build. Vendoring just the files
actually needed sidesteps that without patching the file-list-independent
GitHub dependency PlatformIO would otherwise re-fetch as-is on every clean
checkout.

Source: https://github.com/fknrdcls/mp3_shine_esp32, commit
`b9af39f7dac01efd31b3a4e6686c93e27a681695` (no tagged releases exist
upstream). Files carried over: every `.c`/`.h` at that repo's root except
`example_freeRTOS_task_snippet.c` (see above) and the `include/` subfolder
(a duplicate copy of some of the same headers, apparently meant for an
ESP-IDF component build via the repo's `component.mk` - unused here, since
this project's build is plain PlatformIO/Arduino, not an ESP-IDF
component). `mult_mips_gcc.h`/`mult_sarm_gcc.h` were dropped too -
`types.h` only pulls in `mult_noarch_gcc.h` on this chip (Xtensa, not
MIPS/ARM).

That repo is itself a port of the original "Shine" fixed-point MP3
encoder (Gabriel Bouvigne, later Pete Everett/Patrick Roberts - see
`layer3.c`'s own history, unchanged here) for the ESP32. No
Psychoacoustic model, so encode quality is well below a "real" encoder
like LAME's - acceptable for a voice memo headed for an AI transcription
API (`transcribe.h`), not for music.

**Patched beyond the `MALLOC_CAP_DIRAM` fix noted above**: `shine_initialise()`
never checked any `heap_caps_malloc()` call but its very first for NULL -
harmless with RAM to spare, but a real crash-and-reboot on this no-PSRAM
board running close to its limit (see `speaker.cpp`'s
`mic_start_recording()` comment): one failed sub-allocation meant either
an immediate NULL-pointer write, or a garbage pointer silently carried
into `shine_subband_initialise()`/`shine_mdct_initialise()`/
`shine_loop_initialise()` a few lines down, corrupting unrelated heap
memory instead of failing cleanly. Every allocation there is now checked,
freeing whatever already succeeded and returning NULL on the first
failure. `shine_close()` had a matching bug the other way - it only ever
freed `config` itself, permanently leaking every one of those same
allocations on every single recording (so a second recording would find
less free heap than the first, a third less still). Both now free
everything `shine_initialise()` allocated.

**Also shrunk for this board's real memory budget**: even past the two
fixes above, the encoder's ~75KB working set didn't reliably fit this
board's free heap (see `speaker.cpp`'s `mic_start_recording()` comment -
no PSRAM, 320KB total SRAM already mostly claimed before recording even
starts, and further squeezed once playback's own fragmentation fix below
claimed a static 29KB of its own). Three more changes, all scoped to this
vendored copy rather than upstream, since they're specific to how this
board's mic recording actually uses the encoder (always mono, always
MPEG-II 32kbps/16kHz): `types.h`'s `MAX_CHANNELS` dropped from 2 to 1
(saves ~18KB - every per-channel array/allocation the encoder owns was
sized for stereo it never uses here; see that constant's own comment for
the code audit this was based on - which also turned up a genuine
out-of-bounds write once channels dropped to 1, `layer3.c`'s dead
`config->wave.channels == 2` branches writing `config->buffer[1]`, a
no-longer-existent array element - removed, not just left unreachable),
`types.h`'s `MAX_GRANULES` dropped from 2 to 1 (saves ~13KB more - this
board's 16kHz recording is always MPEG-II, which only ever uses 1
granule/frame; see that constant's own comment for why `l3_sb_sample`'s
declared `MAX_GRANULES+1` depth still only needs to be 2 either way), and
`bitstream.h`'s `BUFFER_SIZE` dropped from 4096 to 512 (saves ~3.5KB -
that buffer only ever needs to hold one MP3 frame at a time, and our
frames are ~144 bytes, not the ~1044 bytes upstream's default was sized
for).

**License**: the upstream repo carries two different license files -
`COPYING` (LGPL v2, the original Shine encoder's own license, kept here
verbatim) and a GitHub-added top-level `LICENSE` (GPL v3, not carried
over here - it reads like a repo-creation default rather than a deliberate
relicensing of code whose header comments and history still say LGPL).
Treat this vendored copy as LGPL v2 - if that's a problem for how this
project is distributed, swap the encoder or drop mic recording instead of
assuming this note settles it.

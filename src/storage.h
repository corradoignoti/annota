#pragma once

#include <FS.h>

#include <cstddef>
#include <cstdint>

constexpr size_t MAX_MP3_FILES = 64;

// Extensions load_mp3_catalog() treats as audio (see has_ext() in
// storage.cpp for the '|'-separated format). .mp3 (playback: AudioGeneratorMP3) and .wav
// (playback: AudioGeneratorWAV; also what mic_start_recording() now
// writes - see speaker.cpp's top-of-recording-section comment for why
// PCM WAV instead of an MP3 encoder) - no AAC/m4a decoder anywhere in
// this codebase, and both AI providers' transcribe path only ever reads
// whatever's already on the card, so there's no path that can do
// anything useful with an .m4a file - keeping it listed (as this used
// to) just let it show up as a dead end.
#define AUDIO_EXTS ".mp3|.wav"

struct Mp3Entry {
    char filename[64];
    uint32_t size;     // bytes, for the on-device Details screen
    // Audio files only: the AI title from the sibling "<basename>.txt"
    // transcript's header line (see transcribe_openai.cpp's title/abstract
    // layout), shown under the filename on the audio list. Empty if there's
    // no transcript or it has no title header (plain-transcript fallback).
    // Folded to the Latin-1 the on-device fonts carry - see read_title().
    char title[48];
    // Audio files only: whether that sibling transcript exists, and its
    // last-write time (0 if none/unknown) - the audio list's sort key, see
    // load_mp3_catalog().
    bool hasTranscript;
    time_t transcriptTime;
};

extern Mp3Entry mp3Files[MAX_MP3_FILES];
extern size_t mp3FileCount;

// Scans the SD card's root for audio files (AUDIO_EXTS) into
// mp3Files/mp3FileCount, fills each entry's `title` from its sibling
// transcript, if any, and sorts the list: files with no transcript first,
// then by transcript date, newest first. Returns false if no SD card is
// present - the caller should invite the user to insert one instead of
// showing a file list.
bool load_mp3_catalog();

// Mounts the SD card for a one-off operation outside the boot-time
// catalog scan (web_server.cpp's file manager). Callers bracket this with
// display_suspend_touch()/display_resume_touch() (no-ops on this board -
// see display.h) for symmetry with any future board that does share the
// card's peripheral with something else. Returns false if the card can't
// be opened.
bool sd_begin();

// Unmounts the card mounted by sd_begin().
void sd_end();

// The mounted filesystem object itself (SD_MMC - see storage.cpp's top
// comment). Callers that need direct fs::FS calls (web_server.cpp's file
// manager: list/open/remove) must go through this instead of naming
// `SD_MMC` directly. Only valid between sd_begin() and sd_end().
fs::FS &sd_fs();

// Raw sector access for usb_drive.cpp's USB mass-storage mode, which
// hands the whole card to a USB host block by block rather than going
// through fs::FS. Only valid between sd_begin() and sd_end(), and only
// while nothing else touches the filesystem (the FATFS mount stays up
// underneath but must not be used - its cached view goes stale as soon as
// the host writes). Sector size is always 512 bytes (SD_SECTOR_SIZE).
constexpr uint16_t SD_SECTOR_SIZE = 512;
uint32_t sd_sector_count();
bool sd_read_sectors(uint8_t *buf, uint32_t firstSector, uint32_t count);
bool sd_write_sectors(const uint8_t *buf, uint32_t firstSector, uint32_t count);

struct SdInfo {
    uint64_t cardBytes;    // raw card capacity (SD.cardSize())
    uint64_t totalBytes;   // usable filesystem capacity (SD.totalBytes())
    uint64_t usedBytes;    // filesystem space in use (SD.usedBytes())
    size_t audioFileCount; // root-level audio files (AUDIO_EXTS)
    size_t textFileCount;  // root-level .txt files
};

// Claims the SD card via sd_begin() (same SPI-sharing rules apply - see
// sd_begin()'s comment) just long enough to read capacity/usage figures for
// the settings view, then releases it via sd_end(). Returns false if the
// card can't be opened.
bool get_sd_info(SdInfo &out);

// Claims the SD card via sd_begin() just long enough to read a root-level
// file's contents into `out` (NUL-terminated, truncated to outLen - 1 bytes
// if longer), then releases it via sd_end(). Returns false if the card or
// the file can't be opened. Caller is responsible for the same
// display_suspend_touch()/display_resume_touch() calls as every other
// after-boot SD access (no-ops on this board, kept for symmetry with
// web_server.cpp's SD handlers).
bool read_text_file_preview(const char *filename, char *out, size_t outLen);

// Claims the SD card via sd_begin() just long enough to work out a
// root-level audio file's playing time in whole seconds (for the on-device
// Details screen), then releases it via sd_end(). .wav: data chunk size /
// fmt chunk byte rate. .mp3: frame count from a Xing/Info or VBRI header
// if present, otherwise a constant-bitrate estimate from the first frame's
// bitrate. Returns false if the card or file can't be opened or the header
// isn't recognized. Same caller responsibility as read_text_file_preview()
// above.
bool get_audio_duration_seconds(const char *filename, uint32_t &secs);

// Claims the SD card via sd_begin() just long enough to read a root-level
// file's size and last-write time (formatted like Mp3Entry::created), then
// releases it via sd_end(). Used by the on-device audio Details screen for
// the sibling transcript, which isn't in mp3Files while the audio list is
// shown. Returns false if the card or file can't be opened. Same caller
// responsibility as read_text_file_preview() above.
bool get_file_info(const char *filename, uint32_t &size, char *created, size_t createdLen);

// Claims the SD card via sd_begin(), deletes a root-level file, then
// releases it via sd_end(). Returns false if the card can't be opened or
// the file doesn't exist. Same caller responsibility as
// read_text_file_preview() above. Doesn't touch mp3Files/mp3FileCount -
// the caller re-scans (load_mp3_catalog()) to refresh the on-screen list.
bool delete_file(const char *filename);

// Writes an audio file's sibling transcript name ("<basename>.txt", same
// naming as the on-device and web transcription paths) into `out`, then
// claims the SD card via sd_begin() to check whether that file exists, and
// releases it via sd_end(). Returns true only if it does. Used by the
// on-device Delete flow to warn about, then also remove, the transcript.
bool find_sibling_transcript(const char *audioFilename, char *out, size_t outLen);

// Claims the SD card via sd_begin(), writes `text` to a root-level file
// (overwriting any existing one), then releases it via sd_end(). Returns
// false if the card or file can't be opened. Same claim/release pattern
// as delete_file() above.
bool write_text_file(const char *filename, const char *text);

// Finds an unused "RECnnnn.wav" name in the SD root (nnnn zero-padded,
// starting at 0001) for a new mic recording (speaker.cpp's
// mic_start_recording()), writing it (NUL-terminated) into `out`. Unlike
// this file's other helpers, doesn't bracket its own sd_begin()/sd_end() -
// the caller already holds the card open for the whole recording that
// follows. Returns false only if every slot up to 9999 is taken (never
// happens in practice).
bool next_recording_filename(char *out, size_t outLen);

#include "storage.h"

#include <Arduino.h>
#include <algorithm>

// -----------------------------------------------------------------------
// MP3 catalog: SD card only - no card, no list, no fallback
//
// SD is on the ESP32-S3's dedicated SDMMC peripheral (1-bit mode: CLK/CMD/D0
// only, pins 39/41/40 - see Waveshare's own example repo,
// waveshareteam/ESP32-S3-ePaper-1.54), entirely separate from the e-paper
// panel's SPI2_HOST - no borrowing/sharing needed, so sd_begin()/sd_end()
// are just SD_MMC.begin()/end(). Everything below that pair (scanning,
// capacity, file read/delete) is written against fs::FS.
// -----------------------------------------------------------------------

#include <SD_MMC.h>

#include "i18n.h"

#define SDMMC_CLK_PIN 39
#define SDMMC_CMD_PIN 41
#define SDMMC_D0_PIN  40
#define SD_FS SD_MMC

Mp3Entry mp3Files[MAX_MP3_FILES];
size_t mp3FileCount = 0;

// `extList` is one extension ("*.txt") or several separated by '|'
// ("*.mp3|.m4a") - matches if `name` ends in any of them, case-insensitive.
static bool has_ext(const char *name, const char *extList) {
    size_t nameLen = strlen(name);
    const char *p = extList;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t extLen = bar ? (size_t)(bar - p) : strlen(p);
        if (nameLen > extLen && strncasecmp(name + nameLen - extLen, p, extLen) == 0) {
            return true;
        }
        p = bar ? bar + 1 : p + extLen;
    }
    return false;
}

static void format_timestamp(time_t t, char *out, size_t outLen) {
    if (t <= 0) {
        strncpy(out, tr(Str::UNKNOWN_DATE), outLen - 1);
        out[outLen - 1] = '\0';
        return;
    }
    struct tm tmInfo;
    localtime_r(&t, &tmInfo);
    strftime(out, outLen, "%Y-%m-%d %H:%M", &tmInfo);
}

static size_t scan_files(fs::FS &fs, Mp3Entry *out, size_t maxEntries, const char *ext) {
    File root = fs.open("/");
    if (!root || !root.isDirectory()) {
        return 0;
    }

    size_t count = 0;
    File entry = root.openNextFile();
    while (entry && count < maxEntries) {
        // name() can come back as a full path ("/dir/x.mp3"); keep the
        // basename only for display and for the dotfile check below (macOS
        // litters FAT volumes with "._x.mp3" / ".DS_Store" junk).
        const char *base = strrchr(entry.name(), '/');
        base = base ? base + 1 : entry.name();

        if (!entry.isDirectory() && base[0] != '.' && has_ext(base, ext)) {
            strncpy(out[count].filename, base, sizeof(out[count].filename) - 1);
            out[count].filename[sizeof(out[count].filename) - 1] = '\0';
            format_timestamp(entry.getLastWrite(), out[count].created, sizeof(out[count].created));
            out[count].size = entry.size();
            out[count].title[0] = '\0';
            out[count].hasTranscript = false;
            out[count].transcriptTime = 0;
            count++;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    return count;
}

// Counts root-level files matching `ext`, same filtering rules as
// scan_files() (skips directories and dotfiles) but without storing
// anything - used for the settings view's separate audio/text counts so it
// doesn't disturb mp3Files/mp3FileCount (whichever catalog is on screen).
static size_t count_files(fs::FS &fs, const char *ext) {
    File root = fs.open("/");
    if (!root || !root.isDirectory()) {
        return 0;
    }

    size_t count = 0;
    File entry = root.openNextFile();
    while (entry) {
        const char *base = strrchr(entry.name(), '/');
        base = base ? base + 1 : entry.name();

        if (!entry.isDirectory() && base[0] != '.' && has_ext(base, ext)) {
            count++;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    return count;
}

bool sd_begin() {
    SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
    return SD_MMC.begin("/sdcard", /*mode1bit=*/true);
}

void sd_end() {
    SD_MMC.end();
}

fs::FS &sd_fs() {
    return SD_FS;
}

uint32_t sd_sector_count() {
    return (uint32_t)SD_MMC.numSectors();
}

// SDMMCFS only exposes single-sector readRAW()/writeRAW(), so multi-sector
// requests are a loop (one card command per 512 bytes - slow next to a
// real card reader, but fine for moving a few recordings around).
bool sd_read_sectors(uint8_t *buf, uint32_t firstSector, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (!SD_MMC.readRAW(buf + i * SD_SECTOR_SIZE, firstSector + i)) return false;
    }
    return true;
}

bool sd_write_sectors(const uint8_t *buf, uint32_t firstSector, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (!SD_MMC.writeRAW(const_cast<uint8_t *>(buf) + i * SD_SECTOR_SIZE, firstSector + i)) return false;
    }
    return true;
}

bool get_sd_info(SdInfo &out) {
    bool sdOk = sd_begin();
    if (sdOk) {
        out.cardBytes = SD_FS.cardSize();
        out.totalBytes = SD_FS.totalBytes();
        out.usedBytes = SD_FS.usedBytes();
        out.audioFileCount = count_files(SD_FS, AUDIO_EXTS);
        out.textFileCount = count_files(SD_FS, ".txt");
        sd_end();
    }
    return sdOk;
}

bool read_text_file_preview(const char *filename, char *out, size_t outLen) {
    out[0] = '\0';
    bool sdOk = sd_begin();
    if (!sdOk) return false;

    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);
    File f = SD_FS.open(path, FILE_READ);
    if (!f) {
        sd_end();
        return false;
    }
    size_t n = f.readBytes(out, outLen - 1);
    out[n] = '\0';
    f.close();
    sd_end();
    return true;
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Walks the RIFF chunk list rather than assuming the canonical 44-byte
// header speaker.cpp's write_wav_header() writes, so files from elsewhere
// with extra chunks (LIST/INFO etc.) ahead of "data" still work.
static bool wav_duration(File &f, uint32_t &secs) {
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint32_t byteRate = 0;
    while (true) {
        uint8_t chunk[8];
        if (f.read(chunk, 8) != 8) return false;
        uint32_t chunkSize = read_le32(chunk + 4);
        size_t chunkStart = f.position();
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunkSize < 16 || f.read(fmt, 16) != 16) return false;
            byteRate = read_le32(fmt + 8);
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (byteRate == 0) return false;
            // A recording cut short before its header was last patched can
            // understate (or zero) the data size - trust the file instead
            // when it's clearly off.
            size_t remaining = f.size() - chunkStart;
            if (chunkSize == 0 || chunkSize > remaining) chunkSize = remaining;
            secs = chunkSize / byteRate;
            return true;
        }
        if (!f.seek(chunkStart + chunkSize + (chunkSize & 1))) return false;
    }
}

// Layer III only - the only layer AudioGeneratorMP3 plays anyway.
static bool mp3_duration(File &f, uint32_t &secs) {
    static const uint16_t BITRATES_V1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t BITRATES_V2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t SAMPLE_RATES_V1[3] = {44100, 48000, 32000};

    // Skip an ID3v2 tag (syncsafe size, plus a 10-byte footer if flagged).
    uint8_t id3[10];
    size_t audioStart = 0;
    if (f.read(id3, 10) == 10 && memcmp(id3, "ID3", 3) == 0) {
        audioStart = 10 + (((uint32_t)(id3[6] & 0x7F) << 21) | ((uint32_t)(id3[7] & 0x7F) << 14) |
                           ((uint32_t)(id3[8] & 0x7F) << 7) | (uint32_t)(id3[9] & 0x7F));
        if (id3[5] & 0x10) audioStart += 10;
    }
    if (!f.seek(audioStart)) return false;

    // Find the first valid Layer III frame header within the next 64 KB.
    const size_t SEARCH_LIMIT = 64 * 1024;
    uint8_t h[4] = {0};
    size_t frameStart = 0;
    bool found = false;
    for (size_t i = 0; i < SEARCH_LIMIT; i++) {
        int c = f.read();
        if (c < 0) return false;
        h[0] = h[1];
        h[1] = h[2];
        h[2] = h[3];
        h[3] = (uint8_t)c;
        if (i < 3) continue;
        if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) continue;
        uint8_t version = (h[1] >> 3) & 3; // 0 = 2.5, 1 = reserved, 2 = MPEG2, 3 = MPEG1
        uint8_t layer = (h[1] >> 1) & 3;   // 1 = Layer III
        uint8_t bitrateIdx = h[2] >> 4;
        uint8_t srIdx = (h[2] >> 2) & 3;
        if (version == 1 || layer != 1 || bitrateIdx == 0 || bitrateIdx == 15 || srIdx == 3) continue;
        frameStart = f.position() - 4;
        found = true;
        break;
    }
    if (!found) return false;

    uint8_t version = (h[1] >> 3) & 3;
    bool mpeg1 = version == 3;
    bool mono = (h[3] >> 6) == 3;
    uint32_t bitrateKbps = (mpeg1 ? BITRATES_V1 : BITRATES_V2)[h[2] >> 4];
    uint32_t sampleRate = SAMPLE_RATES_V1[(h[2] >> 2) & 3] >> (mpeg1 ? 0 : (version == 2 ? 1 : 2));
    uint32_t samplesPerFrame = mpeg1 ? 1152 : 576;

    // VBR files carry a total frame count in a Xing/Info tag (just past the
    // first frame's side info) or a VBRI tag (fixed offset 36).
    uint8_t frame[64];
    if (f.seek(frameStart) && f.read(frame, sizeof(frame)) == sizeof(frame)) {
        size_t xingOff = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        uint32_t frames = 0;
        if (memcmp(frame + xingOff, "Xing", 4) == 0 || memcmp(frame + xingOff, "Info", 4) == 0) {
            if (read_be32(frame + xingOff + 4) & 1) frames = read_be32(frame + xingOff + 8);
        } else if (memcmp(frame + 36, "VBRI", 4) == 0) {
            frames = read_be32(frame + 36 + 14);
        }
        if (frames > 0) {
            secs = (uint32_t)((uint64_t)frames * samplesPerFrame / sampleRate);
            return true;
        }
    }

    // No VBR tag: assume constant bitrate.
    secs = (uint32_t)((uint64_t)(f.size() - frameStart) * 8 / (bitrateKbps * 1000));
    return true;
}

bool get_audio_duration_seconds(const char *filename, uint32_t &secs) {
    secs = 0;
    bool sdOk = sd_begin();
    if (!sdOk) return false;

    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);
    File f = SD_FS.open(path, FILE_READ);
    if (!f) {
        sd_end();
        return false;
    }
    bool ok = has_ext(filename, ".wav") ? wav_duration(f, secs) : mp3_duration(f, secs);
    f.close();
    sd_end();
    return ok;
}

bool get_file_info(const char *filename, uint32_t &size, char *created, size_t createdLen) {
    bool sdOk = sd_begin();
    if (!sdOk) return false;

    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);
    File f = SD_FS.open(path, FILE_READ);
    if (!f) {
        sd_end();
        return false;
    }
    size = f.size();
    format_timestamp(f.getLastWrite(), created, createdLen);
    f.close();
    sd_end();
    return true;
}

bool delete_file(const char *filename) {
    bool sdOk = sd_begin();
    if (!sdOk) return false;

    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);
    bool ok = SD_FS.remove(path);
    sd_end();
    return ok;
}

bool find_sibling_transcript(const char *audioFilename, char *out, size_t outLen) {
    const char *dot = strrchr(audioFilename, '.');
    size_t baseLen = dot ? (size_t)(dot - audioFilename) : strlen(audioFilename);
    snprintf(out, outLen, "%.*s.txt", (int)baseLen, audioFilename);

    bool sdOk = sd_begin();
    if (!sdOk) return false;

    char path[80];
    snprintf(path, sizeof(path), "/%s", out);
    bool exists = SD_FS.exists(path);
    sd_end();
    return exists;
}

bool write_text_file(const char *filename, const char *text) {
    bool sdOk = sd_begin();
    if (!sdOk) return false;

    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);
    File f = SD_FS.open(path, FILE_WRITE);
    if (!f) {
        sd_end();
        return false;
    }
    f.print(text);
    f.close();
    sd_end();
    return true;
}

bool next_recording_filename(char *out, size_t outLen) {
    for (int n = 1; n <= 9999; n++) {
        char candidate[32];
        snprintf(candidate, sizeof(candidate), "REC%04d.wav", n);
        char path[40];
        snprintf(path, sizeof(path), "/%s", candidate);
        if (!SD_FS.exists(path)) {
            strncpy(out, candidate, outLen - 1);
            out[outLen - 1] = '\0';
            return true;
        }
    }
    return false;
}

bool load_mp3_catalog() {
    return load_file_catalog(AUDIO_EXTS);
}

// Copies UTF-8 `src` (len bytes) into `out` as UTF-8 limited to Latin-1
// (U+0000-U+00FF, what lv_font_it_* carry - see CLAUDE.md's Translations
// section): typographic quotes/dashes/ellipsis the AI title may use are
// folded to ASCII, anything else outside Latin-1 is dropped. Never splits a
// multi-byte sequence when out fills up.
static void fold_to_latin1(const char *src, size_t len, char *out, size_t outLen) {
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            n = 4;
        } else {
            i++; // stray continuation byte
            continue;
        }
        if (i + n > len) break; // sequence cut off by the read buffer
        for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)src[i + k] & 0x3F);
        i += n;

        const char *repl = nullptr;
        char enc[3] = {0};
        if (cp == 0x2018 || cp == 0x2019) {
            repl = "'";
        } else if (cp == 0x201C || cp == 0x201D || cp == 0x00AB || cp == 0x00BB) {
            repl = "\"";
        } else if (cp == 0x2013 || cp == 0x2014) {
            repl = "-";
        } else if (cp == 0x2026) {
            repl = "...";
        } else if (cp < 0x20) {
            continue;
        } else if (cp < 0x80) {
            enc[0] = (char)cp;
            repl = enc;
        } else if (cp <= 0xFF) {
            enc[0] = (char)(0xC0 | (cp >> 6));
            enc[1] = (char)(0x80 | (cp & 0x3F));
            repl = enc;
        } else {
            continue;
        }
        size_t rl = strlen(repl);
        if (o + rl >= outLen) break;
        memcpy(out + o, repl, rl);
        o += rl;
    }
    out[o] = '\0';
}

// Fills `entry.hasTranscript`/`transcriptTime` from its sibling
// "<basename>.txt" transcript and `entry.title` from the AI title header of its sibling
// "<basename>.txt" transcript: the first line, but only when a blank line
// follows it - transcribe_openai.cpp (and the web page's callProvider())
// write "title\n\nabstract\n\ntranscript", while the plain-transcript
// fallback is Whisper's single unbroken paragraph, which has no title to
// show. Card must already be mounted.
static void read_title(fs::FS &fs, Mp3Entry &entry) {
    entry.title[0] = '\0';
    const char *dot = strrchr(entry.filename, '.');
    size_t baseLen = dot ? (size_t)(dot - entry.filename) : strlen(entry.filename);
    char path[80];
    snprintf(path, sizeof(path), "/%.*s.txt", (int)baseLen, entry.filename);
    File f = fs.open(path, FILE_READ);
    if (!f) return;
    entry.hasTranscript = true;
    entry.transcriptTime = f.getLastWrite();
    char buf[192];
    size_t n = f.readBytes(buf, sizeof(buf) - 1);
    f.close();
    buf[n] = '\0';

    char *nl = strchr(buf, '\n');
    if (!nl) return;
    size_t lineLen = (size_t)(nl - buf);
    if (lineLen > 0 && buf[lineLen - 1] == '\r') lineLen--;
    const char *next = nl + 1;
    if (*next == '\r') next++;
    if (*next != '\n' || lineLen == 0) return;
    fold_to_latin1(buf, lineLen, entry.title, sizeof(entry.title));
}

// Audio list order: untranscribed files first (nothing to show under them
// yet, and they're the ones still needing work), then by transcript date,
// newest first. Ties keep scan order (stable sort).
static bool audio_before(const Mp3Entry &a, const Mp3Entry &b) {
    if (a.hasTranscript != b.hasTranscript) return !a.hasTranscript;
    return a.transcriptTime > b.transcriptTime;
}

bool load_file_catalog(const char *ext) {
    bool sdOk = sd_begin();
    if (sdOk) {
        mp3FileCount = scan_files(SD_FS, mp3Files, MAX_MP3_FILES, ext);
        for (size_t i = 0; i < mp3FileCount; i++) {
            if (!has_ext(mp3Files[i].filename, ".txt")) read_title(SD_FS, mp3Files[i]);
        }
        if (!has_ext(ext, ".txt")) std::stable_sort(mp3Files, mp3Files + mp3FileCount, audio_before);
        sd_end();
    } else {
        mp3FileCount = 0;
    }

    return sdOk;
}

#include "storage.h"

#include <Arduino.h>

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
        strncpy(out, "Unknown date", outLen - 1);
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

bool delete_file(const char *filename) {
    bool sdOk = sd_begin();
    if (!sdOk) return false;

    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);
    bool ok = SD_FS.remove(path);
    sd_end();
    return ok;
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

bool load_file_catalog(const char *ext) {
    bool sdOk = sd_begin();
    if (sdOk) {
        mp3FileCount = scan_files(SD_FS, mp3Files, MAX_MP3_FILES, ext);
        sd_end();
    } else {
        mp3FileCount = 0;
    }

    return sdOk;
}

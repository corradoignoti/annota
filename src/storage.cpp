#include "storage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// -----------------------------------------------------------------------
// MP3 catalog: SD card only - no card, no list, no fallback
//
// The CYD's SD slot is wired to its own SPI pins (18/19/23), distinct from
// both the display bus (12/13/14, see User_Setup.h) and the touch bus
// (25/32/39, see display.cpp) - same "not actually shared" gotcha as
// touch. The ESP32 only has two general-purpose SPI peripherals though,
// and the display panel keeps one busy full-time, so SD has to borrow the
// other one (the one touch normally owns) *before* touch claims it -
// that's why load_mp3_catalog() must run before display_init_input().
// -----------------------------------------------------------------------

#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS   5

static SPIClass sdSPI(VSPI);

Mp3Entry mp3Files[MAX_MP3_FILES];
size_t mp3FileCount = 0;

static bool has_mp3_ext(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".mp3") == 0;
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

static size_t scan_mp3_files(fs::FS &fs, Mp3Entry *out, size_t maxEntries) {
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

        if (!entry.isDirectory() && base[0] != '.' && has_mp3_ext(base)) {
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

bool load_mp3_catalog() {
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool sdOk = SD.begin(SD_CS, sdSPI);
    if (sdOk) {
        mp3FileCount = scan_mp3_files(SD, mp3Files, MAX_MP3_FILES);
    } else {
        mp3FileCount = 0;
    }
    SD.end();
    sdSPI.end(); // release the SPI peripheral - display_init_input() needs it next for touch

    return sdOk;
}

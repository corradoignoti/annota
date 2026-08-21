#include "storage.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include "display.h"

// -----------------------------------------------------------------------
// MP3 catalog: SD card first, internal flash (LittleFS) if no card
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
static bool usingSd = false;

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

const char *load_mp3_catalog() {
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool sdOk = SD.begin(SD_CS, sdSPI);
    if (sdOk) {
        mp3FileCount = scan_mp3_files(SD, mp3Files, MAX_MP3_FILES);
    }
    SD.end();
    sdSPI.end(); // release the SPI peripheral - display_init_input() needs it next for touch

    if (sdOk) {
        usingSd = true;
        return "SD card";
    }

    if (LittleFS.begin(true)) {
        mp3FileCount = scan_mp3_files(LittleFS, mp3Files, MAX_MP3_FILES);
        usingSd = false;
        return "internal flash";
    }

    mp3FileCount = 0;
    usingSd = false;
    return "no storage found";
}

fs::FS &active_fs() {
    return usingSd ? static_cast<fs::FS &>(SD) : static_cast<fs::FS &>(LittleFS);
}

bool active_source_is_sd() {
    return usingSd;
}

bool acquire_sd_bus() {
    if (!usingSd) {
        return true;
    }
    Serial.println("[storage] acquire_sd_bus: sdSPI.begin");
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    Serial.println("[storage] acquire_sd_bus: SD.begin");
    bool ok = SD.begin(SD_CS, sdSPI);
    Serial.printf("[storage] acquire_sd_bus: SD.begin returned %d\n", ok);
    return ok;
}

void release_sd_bus() {
    if (!usingSd) {
        return;
    }
    Serial.println("[storage] release_sd_bus: SD.end");
    SD.end();
    Serial.println("[storage] release_sd_bus: sdSPI.end");
    sdSPI.end();
    Serial.println("[storage] release_sd_bus: done");
}

bool delete_mp3_file(size_t index) {
    if (index >= mp3FileCount) {
        return false;
    }

    char path[80];
    snprintf(path, sizeof(path), "/%s", mp3Files[index].filename);

    display_set_touch_enabled(false);
    bool ok = acquire_sd_bus() && active_fs().remove(path);
    release_sd_bus();
    display_set_touch_enabled(true);

    if (!ok) {
        return false;
    }

    for (size_t i = index; i + 1 < mp3FileCount; i++) {
        mp3Files[i] = mp3Files[i + 1];
    }
    mp3FileCount--;
    return true;
}

#include "usb_drive.h"

#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>
#include <driver/gpio.h>
#include <esp_private/periph_ctrl.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "reboot_combo.h"
#include "storage.h"
#include "ui.h"

// How long the host connection must stay gone (unmounted or bus-suspended)
// before it's treated as the cable being pulled. Long enough to ride out
// a host's brief bus suspends, short enough to feel immediate on unplug.
static const uint32_t USB_DRIVE_DISCONNECT_MS = 2000;

static USBMSC msc;
static bool active = false;

// Written from TinyUSB's task / the USB event task, read from loop().
static volatile bool exit_requested = false;
static volatile bool host_seen = false;
static volatile uint32_t host_gone_since = 0; // 0 = connected (or never seen)

// MSC callbacks run in TinyUSB's own task. loop() never touches the card
// while active (see usb_drive_start()'s contract), so no locking needed.
// offset is always 0 here: block size is SD_SECTOR_SIZE and TinyUSB's
// transfer buffer (CONFIG_TINYUSB_MSC_BUFSIZE, 4096) is a whole multiple
// of it.
static int32_t on_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)offset;
    return sd_read_sectors((uint8_t *)buffer, lba, bufsize / SD_SECTOR_SIZE) ? (int32_t)bufsize : -1;
}

static int32_t on_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)offset;
    return sd_write_sectors(buffer, lba, bufsize / SD_SECTOR_SIZE) ? (int32_t)bufsize : -1;
}

static bool on_start_stop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    if (load_eject && !start) {
        exit_requested = true; // host ejected the drive
    }
    return true;
}

static void on_usb_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base != ARDUINO_USB_EVENTS) return;
    switch (id) {
        case ARDUINO_USB_STARTED_EVENT:
        case ARDUINO_USB_RESUME_EVENT:
            host_seen = true;
            host_gone_since = 0;
            break;
        case ARDUINO_USB_STOPPED_EVENT:
        case ARDUINO_USB_SUSPEND_EVENT:
            if (host_seen && host_gone_since == 0) {
                host_gone_since = millis() | 1; // never 0 - 0 means "connected"
            }
            break;
        default:
            break;
    }
}

// Hands the USB PHY back to the USB-Serial-JTAG peripheral. Registered as
// an esp_restart() shutdown handler, so it runs on every reboot out of this
// mode - including reboot_combo.h's both-buttons gesture, not just
// usb_drive_process()'s own exits. Needed because the PHY selection lives
// in RTC_CNTL_USB_CONF_REG, which a software reset doesn't touch: without
// this the board comes back up still routed to the (now idle) OTG
// controller, and the serial port/esptool upload never reappear until a
// power cycle. Trimmed-down version of arduino-esp32's own static
// usb_switch_to_cdc_jtag() (esp32-hal-tinyusb.c), minus its wait for the
// host's bus reset - the restart right after this takes care of that.
static void switch_phy_back_to_jtag() {
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_disable(PERIPH_USB_MODULE);

    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);

    // Hold D-/D+ low long enough for the host to see a detach, so it drops
    // the MSC device and re-enumerates the JTAG/serial one afterwards.
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(GPIO_NUM_19, 0);
    gpio_set_level(GPIO_NUM_20, 0);
    esp_rom_delay_us(100 * 1000);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
}

bool usb_drive_start() {
    if (active) return true;
    if (!sd_begin()) return false;
    uint32_t sectors = sd_sector_count();
    if (sectors == 0) {
        sd_end();
        return false;
    }

    msc.vendorID("Annota");
    msc.productID("SD card");
    msc.productRevision("1.0");
    msc.onRead(on_read);
    msc.onWrite(on_write);
    msc.onStartStop(on_start_stop);
    msc.mediaPresent(true);
    msc.isWritable(true);
    msc.begin(sectors, SD_SECTOR_SIZE);

    Serial.printf("usb_drive: %lu sectors, switching USB to mass storage (serial goes silent)\n",
                  (unsigned long)sectors);
    Serial.flush();

    esp_register_shutdown_handler(switch_phy_back_to_jtag);
    USB.onEvent(on_usb_event);
    USB.begin(); // switches the shared PHY from USB-Serial-JTAG to OTG
    active = true;
    return true;
}

bool usb_drive_active() {
    return active;
}

void usb_drive_request_exit() {
    exit_requested = true;
}

void usb_drive_process() {
    if (!active) return;
    uint32_t goneSince = host_gone_since;
    bool cablePulled = goneSince != 0 && millis() - goneSince >= USB_DRIVE_DISCONNECT_MS;
    if (!exit_requested && !cablePulled) return;

    msc.mediaPresent(false);
    ui_show_usb_drive_restarting();
    reboot_now();
}

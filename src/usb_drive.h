#pragma once

// USB drive mode: exposes the whole SD card to a USB host as a mass-storage
// device (TinyUSB MSC), so files can be copied to/from a computer as if the
// board were a USB stick. Entered from the Home screen's "USB drive" card
// (ui_epaper.cpp).
//
// The board's USB port normally runs the ESP32-S3's USB-Serial-JTAG
// peripheral (ARDUINO_USB_MODE=1 - Serial/flashing, see platformio.ini).
// That peripheral and the USB-OTG controller TinyUSB needs share one
// internal PHY on GPIO19/20; usb_drive_start()'s USB.begin() switches the
// PHY over to OTG at runtime, so no build-config change is needed - but
// Serial goes silent for as long as this mode is active. arduino-esp32 has
// no clean way to hand the PHY back (no USB.end()), so every exit path
// goes through reboot_now() instead, which also brings back the JTAG
// serial port and re-scans the SD card the host may have changed. The PHY
// selection survives a software reset, so usb_drive_start() also registers
// an esp_restart() shutdown handler that switches it back first (see
// switch_phy_back_to_jtag() in usb_drive.cpp).
//
// The mode ends (-> reboot) when the host ejects the drive (SCSI START
// STOP UNIT with eject), when usb_drive_request_exit() is called (Select
// long-press on the USB drive screen), or when the host connection has
// been gone/suspended for USB_DRIVE_DISCONNECT_MS (cable pulled on battery
// power) after having been seen at least once.

// Claims the SD card (storage.h's sd_begin()) for the whole session and
// starts MSC. Returns false - with nothing started and the card released -
// if no card can be opened. The caller must make sure nothing else touches
// the SD card from here on (web_server.cpp's handlers: take WiFi offline
// first; main.cpp skips the web/transcribe pumps while usb_drive_active()).
bool usb_drive_start();

// True once usb_drive_start() has succeeded (never goes false again - the
// session ends in a reboot).
bool usb_drive_active();

// Asks the session to end at the next usb_drive_process() call. Safe to
// call from anywhere.
void usb_drive_request_exit();

// Pump from loop(). Cheap no-op unless active; reboots (never returns)
// once an exit condition is met.
void usb_drive_process();

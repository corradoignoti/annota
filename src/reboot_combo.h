#pragma once

// Battery power latch (Waveshare schematic): the physical power switch only
// pulses the regulator on - the MCU must itself hold this pin high or the
// board powers back off the moment the switch is released. Driven HIGH by
// main.cpp's keepBatteryPowerOn(); pad-held across a software reset by
// reboot_now() below so the board doesn't lose power mid-reboot on battery.
#define PWR_HOLD_PIN 17

// "Hold both buttons to reboot" - watched by a dedicated FreeRTOS task
// pinned to core 0 (Arduino's loop() runs on core 1), reading the two
// button GPIOs directly rather than through display.h's
// display_button_poll(), so it keeps working even if loop() or setup() is
// stuck (blocking TLS/SD call, e-paper busy-wait, the first-boot captive
// portal...). Both held continuously for REBOOT_COMBO_HOLD_MS
// (reboot_combo.cpp) calls reboot_now(). Only armed once both buttons have
// been seen released since boot, so a user still holding them as the
// device comes back up doesn't trigger a second reboot. Call once, as
// early as possible in setup() - it configures the button pins itself.
void reboot_combo_start();

// Latches PWR_HOLD_PIN's current (HIGH) level across the reset, then
// esp_restart(). A software reset doesn't re-sample strapping pins, so
// holding BOOT/GPIO0 down through it won't land in download mode. Never
// returns.
[[noreturn]] void reboot_now();

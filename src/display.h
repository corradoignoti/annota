#pragma once

#include <cstdint>

#include "board.h"

// Panel resolution, per board (board.h): 200x200 square mono e-paper on the
// Waveshare ESP32-S3-ePaper-1.54 (panel driver: display_epaper.cpp), 480x800
// portrait on the ESP32-S3-ePaper-3.97 (its 800x480 panel rotated by
// display_epaper397.cpp). Exactly
// one of those two files compiles to anything, picked by the BOARD_* flag.
constexpr uint16_t SCREEN_W = BOARD_SCREEN_W;
constexpr uint16_t SCREEN_H = BOARD_SCREEN_H;

// Brings up just the display panel. Call first, before anything else
// touches the screen or SPI.
void display_init_panel();

// Brings up input (the onboard buttons) and LVGL's display + input
// device. Call once storage.h's SD scan (if any) is done - the call order
// is kept the same as it always was so main.cpp's setup() doesn't need to
// special-case it.
void display_init_input();

// No shared SPI peripheral to hand off on this board (the e-paper panel and
// the SD card are on separate dedicated peripherals) - no-op stubs so
// callers (web_server.cpp, transcribe.cpp) that bracket their SD access
// with these don't need a special case.
void display_suspend_touch();
void display_resume_touch();

// Puts the panel into its lowest-power state right before sleep.cpp's
// esp_deep_sleep_start(), once the sleep screen has been painted (e-paper
// keeps the image unpowered). The panel isn't usable again afterwards -
// waking is a full reset, and display_init_panel() starts it from scratch.
// 3.97: the controller's deep-sleep command (its supply rail itself is cut
// by battery_prepare_deep_sleep()). 1.54: no-op.
void display_prepare_deep_sleep();

// The board's onboard buttons, driving ui_epaper.cpp's list/menu nav.
// kNext advances the current selection/menu option, kSelect opens/confirms
// it (short press) or backs out of it (long press). See ui_epaper.cpp for
// the actual nav scheme built on top of these. kPrev (step back one option)
// and kBack (same as a long kSelect) exist only on boards with
// BOARD_HAS_KNOB (the 3.97's rotary Up and BOOT) - elsewhere they're never
// pressed. GPIOs: board.h's BTN_*_GPIO; implemented in display_buttons.cpp.
enum class DisplayButton { kNext, kSelect, kPrev, kBack };
enum class DisplayButtonEvent { kNone, kShort, kLong };

// Debounced, edge-triggered: returns kShort/kLong at most once per
// press/hold, kNone otherwise (including for the whole duration of a held
// press before it crosses the long-press threshold). Call once per button
// per loop() iteration - ui_epaper.cpp's ui_process_input() does, no other
// caller needed.
DisplayButtonEvent display_button_poll(DisplayButton b);

// Raw current pin state (active-low), no debounce/edge logic - unlike
// display_button_poll(), safe to call any number of times per loop()
// iteration without consuming/altering that function's own per-button
// press-tracking state. Used only to detect whether both buttons are
// currently held down together, ahead of calling display_button_poll() -
// ui_epaper.cpp's ui_process_input() skips the individual poll entirely
// while both are held, so the "hold both to reboot" gesture (see
// reboot_combo.h, which watches the pins itself from its own task) never
// also fires a spurious single-button kLong at this state machine's own,
// shorter long-press threshold, and a hold abandoned before the reboot
// leaves no half-consumed single-button press behind either.
bool display_button_raw_pressed(DisplayButton b);

// Configures the button GPIOs as pulled-up inputs. Called by each panel
// driver's display_init_input(); nothing else needs it.
void display_buttons_init();

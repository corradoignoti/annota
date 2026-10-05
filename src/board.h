#pragma once

// Per-board pins and capabilities, selected by exactly one BOARD_* build
// flag from platformio.ini's env. Everything board-specific that isn't a
// whole separate driver file (display_epaper*.cpp) reads its pins from
// here, so a board is never described in more than one place.
//
// Button GPIOs are -1 when the board lacks that button (display.h's
// display_button_poll() then always reports kNone for it).

#if defined(BOARD_EPAPER_154)

// Waveshare ESP32-S3-ePaper-1.54: 200x200 SSD1681-class panel, 2 buttons.
#define BOARD_SCREEN_W 200
#define BOARD_SCREEN_H 200

#define EPD_SCK_PIN  12
#define EPD_MOSI_PIN 13
#define EPD_CS_PIN   11
#define EPD_DC_PIN   10
#define EPD_RST_PIN  9
#define EPD_BUSY_PIN 8
#define EPD_PWR_PIN  6 // active-low: LOW powers the panel on

// Both active-low, with onboard pull-ups.
#define BTN_NEXT_GPIO   0  // BOOT
#define BTN_SELECT_GPIO 18 // PWR
#define BTN_PREV_GPIO   -1
#define BTN_BACK_GPIO   -1
#define BOARD_HAS_KNOB  0

// Deep-sleep ext1 wakeup source (sleep.cpp) - Select only, so the sleep
// screen's "Hold Select to wake" stays true.
#define WAKE_BUTTON_GPIO BTN_SELECT_GPIO

// Battery power latch (Waveshare schematic): the physical power switch only
// pulses the regulator on - the MCU must itself hold this pin high or the
// board powers back off the moment the switch is released. Driven HIGH by
// main.cpp's keepBatteryPowerOn(); pad-held across a software reset by
// reboot_combo.h's reboot_now() so the board doesn't lose power mid-reboot
// on battery.
#define BOARD_HAS_PWR_LATCH 1
#define PWR_HOLD_PIN 17

// GPIO4: battery cell voltage through a 200K/200K divider (battery.cpp).
#define BOARD_HAS_PMIC 0
#define BAT_ADC_PIN 4

// SDMMC, 1-bit mode.
#define SD_BUS_WIDTH  1
#define SDMMC_CLK_PIN 39
#define SDMMC_CMD_PIN 41
#define SDMMC_D0_PIN  40
#define SDMMC_D1_PIN  -1
#define SDMMC_D2_PIN  -1
#define SDMMC_D3_PIN  -1

// ES8311 codec + NS4150B amp - see speaker.cpp's top comment for where
// these come from.
#define I2S_MCLK_GPIO 14
#define I2S_BCLK_GPIO 15
#define I2S_WS_GPIO   38
#define I2S_DOUT_GPIO 45
#define I2S_DIN_GPIO  16 // mic ADC data in (I2S_ASDOUT)
#define AUDIO_RAIL_PIN 42 // codec+amp analog rail switch, active-low
#define PA_CTRL_PIN    46 // NS4150B amp enable, active-high
#define I2C_SDA_PIN 47
#define I2C_SCL_PIN 48

#elif defined(BOARD_EPAPER_397)

// Waveshare ESP32-S3-ePaper-3.97: 800x480 SSD1677-class panel, used in
// portrait - the UI sees a 480x800 screen, rotated onto the panel by
// display_epaper397.cpp's flush (EPD_PORTRAIT_FLIP picks which way). A 3-way
// rotary switch (Up/Press/Down) plus BOOT, AXP2101-class PMIC. Pins from
// Waveshare's own demo repo (waveshareteam/ESP32-S3-ePaper-3.97:
// epaper_port.h, button_bsp.c, sdcard_bsp.c, es8311_bsp.h, i2c_bsp.h).
#define BOARD_SCREEN_W 480 // portrait: the UI's width, not the panel's
#define BOARD_SCREEN_H 800

#define EPD_SCK_PIN  11
#define EPD_MOSI_PIN 12
#define EPD_CS_PIN   10
#define EPD_DC_PIN   9
#define EPD_RST_PIN  46
#define EPD_BUSY_PIN 3

// All active-low, with pull-ups. The PWR button is wired to the PMIC
// only (power on/off), not to a GPIO.
#define BTN_NEXT_GPIO   6 // rotary Down
#define BTN_SELECT_GPIO 5 // rotary Press
#define BTN_PREV_GPIO   4 // rotary Up
#define BTN_BACK_GPIO   0 // BOOT
#define BOARD_HAS_KNOB  1

#define WAKE_BUTTON_GPIO BTN_SELECT_GPIO

// No GPIO latch: the PMIC keeps the board powered. (GPIO17 - the 1.54's
// latch - is SD CMD here and must never be driven.)
#define BOARD_HAS_PWR_LATCH 0
#define BOARD_HAS_PMIC 1

// SDMMC, 4-bit mode.
#define SD_BUS_WIDTH  4
#define SDMMC_CLK_PIN 16
#define SDMMC_CMD_PIN 17
#define SDMMC_D0_PIN  15
#define SDMMC_D1_PIN  7
#define SDMMC_D2_PIN  8
#define SDMMC_D3_PIN  18

// ES8311 codec + NS4150B amp. No switchable analog rail on this board.
#define I2S_MCLK_GPIO 13
#define I2S_BCLK_GPIO 14
#define I2S_WS_GPIO   47
#define I2S_DOUT_GPIO 48
#define I2S_DIN_GPIO  21
#define AUDIO_RAIL_PIN -1
#define PA_CTRL_PIN    39
// Shared with the PMIC (0x34), RTC, IMU and temperature sensor.
#define I2C_SDA_PIN 41
#define I2C_SCL_PIN 42

#else
#error "No board selected - add -D BOARD_EPAPER_154=1 or -D BOARD_EPAPER_397=1 to the env's build_flags (platformio.ini)"
#endif

# ESP32 Skill

Expert ESP32 embedded systems guidance for GitHub Copilot — chip selection, GPIO validation with anti-bricking safety checks, code generation (Arduino/ESP-IDF), LVGL UI, and Waveshare board references.

## Origin

Adapted from [ezrover/ESP32-AI-Agent-Skill](https://github.com/ezrover/ESP32-AI-Agent-Skill), a Claude Code plugin. This version is reformatted for the GitHub Copilot agent skill harness.

## What It Covers

- **Chip selection** across 9 ESP32 variants (ESP32, S2, S3, C3, C6, C2, C5, H2, P4)
- **GPIO validation** that catches strapping pin traps, ADC2/Wi-Fi conflicts, flash pin violations, and input-only pin misuse
- **Code generation** for Arduino (`setup()`/`loop()`) and ESP-IDF (`app_main()`) with correct bus initialization
- **LVGL references** for versions 8.2 through 9.5 with API docs, widget catalogs, and migration guides
- **Waveshare board references** for 60+ dev boards and LCD displays with full pinout tables
- **Protocol references** for I2C, SPI, UART, PWM, 1-Wire, CAN, ADC, DAC
- **Electrical constraints** — current limits, voltage levels, pull-up/pull-down guidance, level shifting

## Scripts

Two Python scripts are included for automated validation and code generation:

- **`scripts/validate_pinmap.py`** — Validates a JSON pin configuration against hardware constraints
- **`scripts/generate_config.py`** — Generates boilerplate initialization code for Arduino or ESP-IDF

## Reference Files

All reference documentation is in `references/`:

| Directory | Content |
|-----------|---------|
| `references/platforms/` | ESP32 GPIO pin database, specifics (strapping pins, memory, deep sleep) |
| `references/esp32/` through `references/esp32-p4/` | Per-variant specification sheets |
| `references/lvgl/` | LVGL version-specific API references (v8.2–v9.5), migration guides |
| `references/waveshare/` | Waveshare dev boards, LCD modules, display/touch controller ICs |
| `references/protocol-quick-ref.md` | I2C, SPI, UART, PWM, 1-Wire, CAN, ADC, DAC quick reference |
| `references/electrical-constraints.md` | Voltage levels, current limits, pull resistors, level shifting |
| `references/common-devices.md` | Common sensors, displays, motor drivers, communication modules |

## Usage

Ask naturally about any ESP32 topic:

```
I need to wire a BME280 sensor and ST7789 display to an ESP32-S3 with WiFi
```

```
Generate ESP-IDF initialization code for my pin assignments on ESP32-C6
```

```
What changed between LVGL v8 and v9? Which version should I use with ESP32-S3?
```

```
Validate my GPIO pin assignments for an ESP32-S3 project
```

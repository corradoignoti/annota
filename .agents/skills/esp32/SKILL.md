---
name: esp32
description: Program the ESP32 microcontroller using ESP-IDF, Arduino, or PlatformIO. Covers chip selection across 9 variants (ESP32, S2, S3, C3, C6, C2, C5, H2, P4), GPIO validation with anti-bricking safety checks, memory management, LVGL UI development, Waveshare board references, and code generation for Arduino and ESP-IDF frameworks.
version: 1.0.0
license: MIT
author: "@ezrover"
tags:
  - esp32
  - esp-idf
  - arduino
  - platformio
  - embedded
  - gpio
  - lvgl
  - waveshare
  - i2c
  - spi
  - uart
---

## Instructions

Use this skill when writing, debugging, or explaining firmware for the **ESP32** microcontroller family, using **ESP-IDF**, **Arduino**, or **PlatformIO** frameworks.

### When to Use

- Writing or editing ESP32 firmware (C/C++, `.c`, `.cpp`, `.h`)
- Explaining ESP-IDF API (`idf.py`, `sdkconfig`, peripherals, FreeRTOS tasks)
- Explaining Arduino ESP32 API (`setup()`/`loop()`, `Wire`, `SPI`, `WiFi`)
- Configuring PlatformIO projects (`platformio.ini`, multi-environment builds)
- Chip selection across ESP32 variants (S3, C3, C6, H2, P4, etc.)
- GPIO pin assignment, validation, and conflict detection
- LVGL GUI development on ESP32 displays
- Waveshare board/display configuration
- Debugging ESP32 boot issues, flash voltage traps, ADC2/Wi-Fi conflicts
- Memory management (MMU, PSRAM, IRAM, RTC memory, heap allocation)
- Deep sleep, RTC domain, ULP coprocessor programming
- I2C, SPI, UART, PWM, CAN, 1-Wire, ADC, DAC peripheral configuration

### Reference Loading

ALWAYS load the platform pin database first. Load other files only when their trigger condition is met. Use the `read_file` tool to read these files from the skill directory.

**Skill directory:** `.agents/skills/esp32/`

| File                      | Trigger                                                                          | Path                                      |
| ------------------------- | -------------------------------------------------------------------------------- | ----------------------------------------- |
| ESP32 GPIO Pin Reference  | **Always** (Core GPIO reference)                                                 | `references/platforms/esp32-pins.md`      |
| ESP32 Specifics           | Strapping pins, deep sleep, flash/PSRAM, ADC2, boot issues, architecture, memory | `references/platforms/esp32-specifics.md` |
| Protocol Quick Reference  | Any protocol: I2C, SPI, UART, PWM, 1-Wire, CAN, ADC, DAC                         | `references/protocol-quick-ref.md`        |
| Electrical Constraints    | Current limits, voltage levels, pull-ups/pull-downs, power supply                | `references/electrical-constraints.md`    |
| Common Devices            | Specific sensor, module, display, or breakout board mentioned                    | `references/common-devices.md`            |
| ESP32 (Original) Specs    | Original ESP32 variant specifics                                                 | `references/esp32/specs.md`               |
| ESP32-S2 Specs            | ESP32-S2 variant specifics                                                       | `references/esp32-s2/specs.md`            |
| ESP32-S3 Specs            | ESP32-S3 variant specifics                                                       | `references/esp32-s3/specs.md`            |
| ESP32-C3 Specs            | ESP32-C3 variant specifics                                                       | `references/esp32-c3/specs.md`            |
| ESP32-C6 Specs            | ESP32-C6 variant specifics                                                       | `references/esp32-c6/specs.md`            |
| ESP32-H2 Specs            | ESP32-H2 variant specifics                                                       | `references/esp32-h2/specs.md`            |
| ESP32-P4 Specs            | ESP32-P4 variant specifics                                                       | `references/esp32-p4/specs.md`            |
| LVGL Reference Index      | LVGL, display GUI, or UI framework mentioned                                     | `references/lvgl/README.md`               |
| Waveshare Reference Index | Waveshare board or display mentioned                                             | `references/waveshare/README.md`          |

When LVGL is mentioned, load the version-specific folder from `references/lvgl/` based on the user's target version (see the LVGL README for version selection guidance).

When Waveshare is mentioned, load the specific board/display file from `references/waveshare/dev-boards/` or `references/waveshare/lcd-boards/`.

## 1. Hardware Architecture & Chip Families

When advising on hardware selection, apply the following matrix:

| Variant              | Core                   | Clock   | Wireless                                | Best For                                               |
| -------------------- | ---------------------- | ------- | --------------------------------------- | ------------------------------------------------------ |
| **ESP32 (Original)** | Dual-core Xtensa LX6   | 240 MHz | Wi-Fi 4, BT Classic, BLE 4.2            | Legacy projects requiring Bluetooth Classic            |
| **ESP32-S2**         | Single-core Xtensa LX7 | 240 MHz | Wi-Fi 4                                 | Ultra-low power, USB OTG/HID                           |
| **ESP32-S3**         | Dual-core Xtensa LX7   | 240 MHz | Wi-Fi 4, BLE 5.0                        | Performance, AI/ML (Vector instructions), complex GUIs |
| **ESP32-C2**         | Single-core RISC-V     | 120 MHz | Wi-Fi 4, BLE 5.0                        | Cost-sensitive ESP8266 replacements                    |
| **ESP32-C3**         | Single-core RISC-V     | 160 MHz | Wi-Fi 4, BLE 5.0                        | Standard budget IoT node                               |
| **ESP32-C5**         | Single-core RISC-V     | 240 MHz | Dual-band Wi-Fi 6, BLE 5, Zigbee/Thread | Next-gen dual-band                                     |
| **ESP32-C6**         | Single-core RISC-V     | 160 MHz | Wi-Fi 6, BLE 5.3, Zigbee/Thread         | Next-gen Matter/mesh nodes                             |
| **ESP32-H2**         | Single-core RISC-V     | 96 MHz  | BLE 5.0, Zigbee/Thread (**No Wi-Fi**)   | Hub/Home, Thread border router                         |
| **ESP32-P4**         | Dual-core RISC-V       | 400 MHz | **No wireless**                         | Multimedia powerhouse (H.264, dual MIPI)               |

## 2. Safety & "Anti-Bricking" Guardrails (CRITICAL)

Actively protect hardware from destructive configurations:

### GPIO12 Flash Voltage Trap (ESP32 Original)

- MTDI strapping pin. If driven HIGH during boot, it sets flash voltage to 1.8V, potentially **bricking 3.3V modules**.
- **Enforce a strict "Do Not Use" or "Pull-Down Only" policy.**

### ADC2/Wi-Fi Conflict

- ADC2 cannot be used simultaneously with Wi-Fi on original ESP32/S2/S3.
- Use ADC1 pins (GPIO32-39 on original ESP32) when Wi-Fi is active.

### Input-Only Pins

- GPIOs 34-39 (original ESP32) are strictly inputs and lack internal pull resistors.

### IOMUX Collision

- Clear initial IOMUX functions using `gpio_func_sel(pin, PIN_FUNC_GPIO)` when remapping.

### Flash Pins (NEVER USE)

- ESP32 Original: GPIO6-11 (SPI flash)
- ESP32-S3: GPIO26-32 (SPI flash)
- ESP32-S2: GPIO26-32 (SPI flash)

## 3. Memory & Firmware Standards

### Memory Hierarchy

- **DRAM:** Data RAM, single-byte accessible (`MALLOC_CAP_8BIT`)
- **IRAM:** Instruction RAM — ISR and flash-write code MUST reside here (`MALLOC_CAP_32BIT`)
- **D/IRAM:** Flexible RAM usable on both buses
- **RTC Memory:** ULP coprocessor + `RTC_DATA_ATTR` variables during Deep Sleep
- **PSRAM:** External/integrated RAM for heavy workloads (`MALLOC_CAP_SPIRAM`)

### Heap Allocation

Use capabilities-based allocation:

```c
// DMA-capable buffer
void *buf = heap_caps_malloc(1024, MALLOC_CAP_DMA);

// External PSRAM buffer
void *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);

// SIMD-aligned buffer (ESP32-S3/P4)
void *buf = heap_caps_malloc(256, MALLOC_CAP_SIMD);
```

### Modern C++ Standards

- Apply RAII universally. Never use raw `new`/`delete`.
- Enforce static allocation or smart pointers.
- Always include Watchdog Timers (IWDT/TWDT).
- Implement short ISRs — defer processing to tasks.

## 4. Tooling & CLI

### ESP-IDF (`idf.py`)

Use modern hyphenated syntax (v5.0+):

```bash
idf.py set-target esp32s3    # Set MCU target (clears build)
idf.py menuconfig             # Configure project
idf.py build                  # Build firmware
idf.py flash                  # Flash to device
idf.py monitor                # Serial monitor
idf.py erase-flash            # Erase flash completely
```

### PlatformIO

Manage `platformio.ini` for multi-environment builds:

```ini
[env:esp32s3]
platform = espressif32
board = esp32s3box
framework = espidf

[env:esp32c3_arduino]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
```

## 5. Core Workflow

1. **Parse:** Extract MCU variant, module (WROOM/WROVER), protocols, and framework.
2. **Detect:** Identify potential conflicts (ADC2, Strapping pins, Flash pins).
3. **Load:** Read triggered reference files from `references/` using `read_file`.
4. **Generate:** Assign pins using GPIO Matrix flexibility. Prefer conventional defaults unless conflicts exist.
5. **Validate:** Run `scripts/validate_pinmap.py` to check for electrical and boot-time conflicts.
6. **Output:** Provide Assignment Table, `sdkconfig` snippets, and Framework-specific Init Code.

## 6. Script Interface

The skill includes two Python scripts for validation and code generation. Run them using the terminal.

### Input JSON Schema

Both scripts accept the same JSON input format:

```json
{
  "platform": "esp32",
  "variant": "esp32|esp32s2|esp32s3|esp32c3|esp32c6",
  "module": "WROOM|WROVER",
  "wifi_enabled": false,
  "pins": [
    {
      "gpio": 21,
      "function": "I2C_SDA",
      "protocol_bus": "i2c|spi|uart|pwm|adc|gpio|1wire",
      "device": "BME280",
      "direction": "input|output|inout",
      "pull": "none|up|down|internal_up|internal_down|external_up|external_down",
      "speed_hz": 100000,
      "notes": "Optional notes"
    }
  ]
}
```

| Field                 | Required | Default                  | Description                               |
| --------------------- | -------- | ------------------------ | ----------------------------------------- |
| `platform`            | No       | `"esp32"`                | Platform family                           |
| `variant`             | No       | Inferred from `platform` | Chip variant                              |
| `module`              | No       | `"WROOM"`                | Module type (affects reserved pins)       |
| `wifi_enabled`        | No       | `false`                  | Enables ADC2/WiFi conflict checks         |
| `pins`                | **Yes**  | —                        | Array of pin assignments                  |
| `pins[].gpio`         | **Yes**  | —                        | GPIO number (integer)                     |
| `pins[].function`     | No       | `""`                     | Signal name (e.g., `I2C_SDA`, `SPI_MOSI`) |
| `pins[].protocol_bus` | No       | `""`                     | Protocol type for categorization          |
| `pins[].device`       | No       | `""`                     | Device name for wiring notes              |
| `pins[].direction`    | No       | `""`                     | Pin direction hint                        |
| `pins[].pull`         | No       | `"none"`                 | Pull resistor configuration               |
| `pins[].speed_hz`     | No       | `0`                      | Bus clock speed in Hz                     |
| `pins[].notes`        | No       | `""`                     | Free-text notes                           |

**Note:** Script validation and code generation support esp32, esp32s2, esp32s3, esp32c3, and esp32c6 variants. Reference documentation is available for additional variants (C2, C5, H2, P4) for advisory purposes.

### validate_pinmap.py

Validates a JSON pin configuration against hardware constraints:

```bash
python .agents/skills/esp32/scripts/validate_pinmap.py --format json < input.json
```

### generate_config.py

Generates boilerplate initialization code for the selected framework:

```bash
python .agents/skills/esp32/scripts/generate_config.py --format json --framework arduino < input.json
python .agents/skills/esp32/scripts/generate_config.py --format json --framework espidf < input.json
```

## 7. Common Patterns

### ESP-IDF I2C Initialization (v5.0+)

```c
#include "driver/i2c_master.h"

i2c_master_bus_handle_t bus_handle;
i2c_master_bus_config_t bus_config = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_21,
    .scl_io_num = GPIO_NUM_22,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

i2c_device_config_t dev_config = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = 0x76,  // BME280
    .scl_speed_hz = 100000,
};
i2c_master_dev_handle_t dev_handle;
ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));
```

### ESP-IDF SPI Initialization (v5.0+)

```c
#include "driver/spi_master.h"

spi_device_handle_t spi_dev;
spi_device_interface_config_t devcfg = {
    .clock_speed_hz = 10 * 1000 * 1000,  // 10 MHz
    .mode = 0,                             // SPI mode 0
    .spics_io_num = GPIO_NUM_5,            // CS pin
    .queue_size = 7,
    .flags = SPI_DEVICE_HALFDUPLEX,
};
spi_bus_config_t buscfg = {
    .mosi_io_num = GPIO_NUM_23,
    .miso_io_num = GPIO_NUM_19,
    .sclk_io_num = GPIO_NUM_18,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4096,
};
ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_dev));
```

### Arduino ESP32 I2C

```cpp
#include <Wire.h>

void setup() {
    Wire.begin(21, 22);  // SDA, SCL
    Wire.setClock(100000);
}

void loop() {
    Wire.beginTransmission(0x76);
    Wire.write(0xF4);  // BME280 ctrl_meas
    Wire.write(0x27);  // normal mode, x4 oversampling
    Wire.endTransmission();
}
```

### Deep Sleep with RTC Memory

```c
#include "esp_sleep.h"

RTC_DATA_ATTR int boot_count = 0;

void app_main() {
    boot_count++;
    printf("Boot count: %d\n", boot_count);

    // Configure wake-up source
    esp_sleep_enable_timer_wakeup(10 * 1000000);  // 10 seconds

    // Enter deep sleep
    esp_deep_sleep_start();
}
```

### FreeRTOS Task Pattern

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void sensor_task(void *pvParameters) {
    while (1) {
        // Read sensor
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms delay
    }
}

void app_main() {
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
}
```

### GPIO Configuration (ESP-IDF v5.0+)

```c
#include "driver/gpio.h"

gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << GPIO_NUM_4),
    .mode = GPIO_MODE_INPUT_OUTPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};
gpio_config(&io_conf);

// Set output level
gpio_set_level(GPIO_NUM_4, 1);

// Read input
int level = gpio_get_level(GPIO_NUM_4);
```

### LEDC PWM (ESP-IDF v5.0+)

```c
#include "driver/ledc.h"

ledc_timer_config_t timer_conf = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_13_BIT,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = 5000,
    .clk_cfg = LEDC_AUTO_CLK,
};
ledc_timer_config(&timer_conf);

ledc_channel_config_t channel_conf = {
    .gpio_num = GPIO_NUM_2,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = LEDC_CHANNEL_0,
    .timer_sel = LEDC_TIMER_0,
    .duty = 0,
    .hpoint = 0,
};
ledc_channel_config(&channel_conf);

// Set duty cycle (0-8191 for 13-bit)
ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4096);
ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
```

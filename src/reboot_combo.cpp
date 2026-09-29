#include "reboot_combo.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>

// Same pins as display_epaper.cpp's BOOT_BUTTON_PIN/PWR_BUTTON_PIN (both
// active-low with onboard pull-ups) - duplicated here rather than exposed
// through display.h, same reasoning as sleep.cpp's own copy.
static const gpio_num_t NEXT_BUTTON_GPIO = GPIO_NUM_0;
static const gpio_num_t SELECT_BUTTON_GPIO = GPIO_NUM_18;

static const uint32_t REBOOT_COMBO_HOLD_MS = 5000;
static const uint32_t REBOOT_COMBO_POLL_MS = 20;

static bool both_pressed() {
    return gpio_get_level(NEXT_BUTTON_GPIO) == 0 && gpio_get_level(SELECT_BUTTON_GPIO) == 0;
}

static void reboot_combo_task(void *) {
    bool armed = false;
    uint32_t heldMs = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REBOOT_COMBO_POLL_MS));
        if (!both_pressed()) {
            armed = true;
            heldMs = 0;
            continue;
        }
        if (!armed) continue; // still held from before the last reboot
        heldMs += REBOOT_COMBO_POLL_MS;
        if (heldMs >= REBOOT_COMBO_HOLD_MS) {
            Serial.println("annota: both buttons held - rebooting");
            reboot_now();
        }
    }
}

void reboot_combo_start() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << NEXT_BUTTON_GPIO) | (1ULL << SELECT_BUTTON_GPIO);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    // Core 0, just under the top priority: loop() runs on core 1, so no
    // amount of blocking or busy-waiting there can starve this task.
    xTaskCreatePinnedToCore(reboot_combo_task, "reboot_combo", 3072, nullptr,
                            configMAX_PRIORITIES - 2, nullptr, 0);
}

void reboot_now() {
    gpio_hold_en((gpio_num_t)PWR_HOLD_PIN);
    esp_restart();
}

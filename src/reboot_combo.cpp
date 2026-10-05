#include "reboot_combo.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>

// The two buttons of the gesture (both active-low with pull-ups, see
// board.h): Next+Select on the 1.54, Select (rotary Press) + Back (BOOT) on
// the 3.97 - there Next is a rotary direction, and Down+Press can't be held
// together on the one knob.
#if BOARD_HAS_KNOB
static const gpio_num_t COMBO_GPIO_A = (gpio_num_t)BTN_BACK_GPIO;
#else
static const gpio_num_t COMBO_GPIO_A = (gpio_num_t)BTN_NEXT_GPIO;
#endif
static const gpio_num_t COMBO_GPIO_B = (gpio_num_t)BTN_SELECT_GPIO;

static const uint32_t REBOOT_COMBO_HOLD_MS = 5000;
static const uint32_t REBOOT_COMBO_POLL_MS = 20;

static bool both_pressed() {
    return gpio_get_level(COMBO_GPIO_A) == 0 && gpio_get_level(COMBO_GPIO_B) == 0;
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
    cfg.pin_bit_mask = (1ULL << COMBO_GPIO_A) | (1ULL << COMBO_GPIO_B);
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
#if BOARD_HAS_PWR_LATCH
    gpio_hold_en((gpio_num_t)PWR_HOLD_PIN);
#endif
    esp_restart();
}

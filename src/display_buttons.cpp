#include "display.h"

#include <Arduino.h>

// -----------------------------------------------------------------------
// Button polling - see display.h's DisplayButton/display_button_poll().
// Shared by every panel driver; pins come from board.h, and a button the
// board doesn't have (BTN_*_GPIO -1) just never reports a press.
// -----------------------------------------------------------------------

static const uint32_t BUTTON_DEBOUNCE_MS = 30;
static const uint32_t BUTTON_LONG_PRESS_MS = 700;

struct ButtonState {
    int8_t pin;
    uint32_t pressedSinceMs = 0; // 0 while not pressed
    bool longFired = false;
};
// Indexed by DisplayButton.
static ButtonState buttonStates[4] = {{BTN_NEXT_GPIO}, {BTN_SELECT_GPIO}, {BTN_PREV_GPIO}, {BTN_BACK_GPIO}};

void display_buttons_init() {
    for (const ButtonState &s : buttonStates) {
        if (s.pin >= 0) pinMode(s.pin, INPUT_PULLUP);
    }
}

DisplayButtonEvent display_button_poll(DisplayButton b) {
    ButtonState &s = buttonStates[(int)b];
    if (s.pin < 0) return DisplayButtonEvent::kNone;
    bool pressed = digitalRead(s.pin) == LOW; // active-low
    uint32_t now = millis();

    if (pressed) {
        if (s.pressedSinceMs == 0) {
            s.pressedSinceMs = now;
            s.longFired = false;
        } else if (!s.longFired && (now - s.pressedSinceMs) >= BUTTON_LONG_PRESS_MS) {
            s.longFired = true;
            return DisplayButtonEvent::kLong;
        }
        return DisplayButtonEvent::kNone;
    }

    if (s.pressedSinceMs != 0) {
        uint32_t heldMs = now - s.pressedSinceMs;
        bool wasLong = s.longFired;
        s.pressedSinceMs = 0;
        s.longFired = false;
        if (!wasLong && heldMs >= BUTTON_DEBOUNCE_MS) {
            return DisplayButtonEvent::kShort;
        }
    }
    return DisplayButtonEvent::kNone;
}

bool display_button_raw_pressed(DisplayButton b) {
    int8_t pin = buttonStates[(int)b].pin;
    return pin >= 0 && digitalRead(pin) == LOW; // active-low
}

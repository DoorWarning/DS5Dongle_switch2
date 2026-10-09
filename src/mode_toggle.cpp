#include "mode_toggle.h"

#include <cstdio>

#include "config.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

namespace {
constexpr int64_t MODE_TOGGLE_HOLD_US = 2000000;

constexpr uint8_t NO_MODE = 0xFF;

absolute_time_t combo_since = nil_time;
bool combo_fired = false;
bool toggle_requested = false;
uint8_t requested_mode = NO_MODE;           // set by mode_request() (companion app)
absolute_time_t requested_at = nil_time;    // when to act on it
}

void mode_request(uint8_t mode, uint32_t delay_ms) {
    requested_mode = mode;
    requested_at = make_timeout_time_ms(delay_ms);
}

void mode_toggle_on_input(bool create, bool options, bool mute) {
    if (!(create && options && mute)) {
        combo_since = nil_time;
        combo_fired = false;
        return;
    }
    if (combo_fired) {
        return;
    }
    if (is_nil_time(combo_since)) {
        combo_since = get_absolute_time();
        return;
    }
    if (absolute_time_diff_us(combo_since, get_absolute_time()) >= MODE_TOGGLE_HOLD_US) {
        combo_fired = true;
        toggle_requested = true;
    }
}

void mode_toggle_task() {
    const bool requested = requested_mode != NO_MODE && time_reached(requested_at);
    if (!toggle_requested && !requested) {
        return;
    }

    Config_body next = get_config();
    if (requested) {
        next.controller_mode = requested_mode;
    } else {
        next.controller_mode = next.controller_mode == ControllerMode_SwitchPro
                                   ? ControllerMode_Auto
                                   : ControllerMode_SwitchPro;
    }
    toggle_requested = false;
    requested_mode = NO_MODE;
    printf("[Mode] set -> %u\n", next.controller_mode);
    set_config(next);
    watchdog_update();
    config_save();
    watchdog_update();

    const int blinks = get_config().controller_mode == ControllerMode_SwitchPro ? 2 : 1;
    for (int i = 0; i < blinks; i++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        sleep_ms(150);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
        sleep_ms(150);
        watchdog_update();
    }
    if (get_config().disable_pico_led) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    }

    // Reboot instead of a USB re-connect: core1 sizes its heap use (opus or the
    // macro buffer) from the mode at boot. Same SYSRESETREQ warm reset as the
    // BOOTSEL double click; a watchdog reset would drop into the bootloader.
    printf("[Mode] rebooting\n");
    *((volatile uint32_t *) 0xe000ed0c) = 0x05fa0004; // SCB AIRCR: VECTKEY | SYSRESETREQ
    __dsb();
    while (true) { tight_loop_contents(); }
}

#include "mode_toggle.h"

#include <cstdio>

#include "config.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

namespace {
constexpr int64_t MODE_TOGGLE_HOLD_US = 2000000;

absolute_time_t combo_since = nil_time;
bool combo_fired = false;
bool toggle_requested = false;
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
    if (!toggle_requested) {
        return;
    }
    toggle_requested = false;

    Config_body next = get_config();
    next.controller_mode = next.controller_mode == ControllerMode_SwitchPro
                               ? ControllerMode_Auto
                               : ControllerMode_SwitchPro;
    printf("[Mode] toggle -> %u\n", next.controller_mode);
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

// Switch Pro mode settings, changed from the controller and kept in flash.
//
// The PC web config can't reach the dongle while it sits in the dock, so these
// live in their own flash record instead of Config_body (which the web config
// reads and writes).
//
//  - Mute + D-pad Up/Down: vibration level 0-5 (0 = off, then 20 % steps;
//    default 3). The level shows on the player LEDs for PLAYER_LED_MS, with a
//    short test pulse at the new strength.
//  - Mute + D-pad Right/Left: next/previous trigger mode. The mode number shows
//    as mute LED blinks.
//
// Trigger modes use DualSense raw adaptive trigger effects, FFB[11] =
// [mode, p0, p1, p2, 0...] (the encoding ds.daidr.me's tester uses), on L2 and
// R2. Switch ZL/ZR are digital, so each mode fires them where its feel says
// "pressed":
//   1 Normal:  no effect; DS5 digital bit or analog > 32 (as before)
//   2 Click:   0x02 section, resistance that gives way; fires at the click
//   3 Short:   0x01 strong resistance near the top, so the trigger acts like a button
//   4 Auto:    0x06 vibrating trigger (automatic gun) from a start point

#include "switch_settings.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "bt.h"
#include "config.h"
#include "macro.h"
#include "switch_hd_haptics.h"
#include "utils.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/btstack_flash_bank.h"
#include "pico/flash.h"
#include "pico/time.h"

namespace {

constexpr uint8_t VIB_LEVELS = 5; // 1..5, plus 0 = off
constexpr uint8_t DEFAULT_VIB_LEVEL = 3;
constexpr uint8_t TRIGGER_MODES = 4; // 1..4
constexpr uint8_t DEFAULT_TRIGGER_MODE = 1;
constexpr uint32_t SAVE_DELAY_MS = 3000;
constexpr uint32_t PLAYER_LED_MS = 2000;
constexpr uint32_t MUTE_BLINK_MS = 250;

constexpr uint32_t RECORD_MAGIC = 0x53574354; // "SWCT"
constexpr uint8_t RECORD_VERSION = 1;
// Below the wake beacon sector (switch_wake.cpp), which is below the macro slots.
constexpr uint32_t SETTINGS_FLASH_OFFSET = PICO_FLASH_BANK_STORAGE_OFFSET - 7 * FLASH_SECTOR_SIZE;
static_assert(SETTINGS_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

struct __attribute__((packed)) SettingsRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t vib_level;
    uint8_t trigger_mode;
    uint8_t reserved;
    uint32_t crc;
};

struct TriggerMode {
    uint8_t effect[4];  // FFB mode + params; the rest of the 11 bytes stay 0
    uint8_t threshold;  // analog value where ZL/ZR fire (modes 2-4)
};

// Index = trigger mode - 1. Thresholds are starting points: effect position and
// analog value are only roughly linear.
constexpr TriggerMode TRIGGER_TABLE[TRIGGER_MODES] = {
    {{0x00, 0, 0, 0}, 32},         // 1 Normal
    {{0x02, 15, 100, 255}, 100},   // 2 Click: section start 15, end 100, force 255
    {{0x01, 40, 255, 0}, 30},      // 3 Short: resistance from 40, force 255
    {{0x06, 10, 255, 20}, 20},     // 4 Auto: frequency 10, force 255, start 20
};

uint8_t vib_level = DEFAULT_VIB_LEVEL;
uint8_t trigger_mode = DEFAULT_TRIGGER_MODE;

Direction prev_dpad = None;

bool save_pending = false;
uint32_t save_at_ms = 0;
bool trigger_apply_pending = false;
bool player_led_pending = false;
uint32_t player_led_clear_ms = 0; // 0 = nothing to clear
uint8_t mute_toggles_left = 0;
uint32_t mute_last_ms = 0;
bool mute_lit = false;

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

const SettingsRecord &stored() {
    return *reinterpret_cast<const SettingsRecord *>(XIP_BASE + SETTINGS_FLASH_OFFSET);
}

uint32_t record_crc(const SettingsRecord &r) {
    return crc32(reinterpret_cast<const uint8_t *>(&r), offsetof(SettingsRecord, crc));
}

void save_flash_op(void *param) {
    const auto *page = static_cast<const uint8_t *>(param);
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(interrupts);
}

void save() {
    SettingsRecord r{RECORD_MAGIC, RECORD_VERSION, vib_level, trigger_mode, 0, 0};
    r.crc = record_crc(r);
    if (memcmp(&stored(), &r, sizeof(r)) == 0) {
        return;
    }
    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &r, sizeof(r));
    watchdog_update();
    const int rc = flash_safe_execute(save_flash_op, page, 1000);
    watchdog_update();
    printf("[Settings] saved vib=%u trigger=%u rc=%d\n", vib_level, trigger_mode, rc);
}

void schedule_save() {
    save_pending = true;
    save_at_ms = now_ms() + SAVE_DELAY_MS;
}

void apply_trigger_effect() {
    SetStateData s{};
    s.AllowRightTriggerFFB = 1;
    s.AllowLeftTriggerFFB = 1;
    const uint8_t *effect = TRIGGER_TABLE[trigger_mode - 1].effect;
    memcpy(s.RightTriggerFFB, effect, sizeof(TRIGGER_TABLE[0].effect));
    memcpy(s.LeftTriggerFFB, effect, sizeof(TRIGGER_TABLE[0].effect));
    update_state(s);
}

void show_player_leds(uint8_t count) {
    SetStateData s{};
    s.AllowPlayerIndicators = 1;
    s.PlayerLightFade = 1; // change instantly
    s.PlayerLight1 = count >= 1;
    s.PlayerLight2 = count >= 2;
    s.PlayerLight3 = count >= 3;
    s.PlayerLight4 = count >= 4;
    s.PlayerLight5 = count >= 5;
    update_state(s);
}

void set_mute_led(bool on) {
    SetStateData s{};
    s.AllowMuteLight = 1;
    s.MuteLightMode = on ? MuteLight::On : MuteLight::Off;
    update_state(s);
}

void change_vibration(int delta) {
    const int next = static_cast<int>(vib_level) + delta;
    if (next < 0 || next > VIB_LEVELS) {
        player_led_pending = true; // at the end of the range: just show it again
        return;
    }
    vib_level = static_cast<uint8_t>(next);
    player_led_pending = true;
    if (vib_level > 0) {
        switch_hd_haptics_test_pulse();
    }
    schedule_save();
    printf("[Settings] vibration level %u\n", vib_level);
}

void change_trigger_mode(int delta) {
    trigger_mode = static_cast<uint8_t>((trigger_mode - 1 + TRIGGER_MODES + delta) % TRIGGER_MODES + 1);
    trigger_apply_pending = true;
    mute_toggles_left = static_cast<uint8_t>(trigger_mode * 2);
    mute_last_ms = 0;
    mute_lit = false;
    schedule_save();
    printf("[Settings] trigger mode %u\n", trigger_mode);
}

} // namespace

void switch_settings_init() {
    const SettingsRecord &r = stored();
    if (r.magic == RECORD_MAGIC && r.version == RECORD_VERSION && r.crc == record_crc(r) &&
        r.vib_level <= VIB_LEVELS && r.trigger_mode >= 1 && r.trigger_mode <= TRIGGER_MODES) {
        vib_level = r.vib_level;
        trigger_mode = r.trigger_mode;
    }
    printf("[Settings] vibration level %u, trigger mode %u\n", vib_level, trigger_mode);
}

void switch_settings_on_input(const USBGetStateData &ds5, uint8_t io[9]) {
    const bool mute = ds5.ButtonMute;
    const Direction dpad = mute ? ds5.DPad : None;
    if (mute && dpad != prev_dpad) {
        switch (dpad) {
            case North: change_vibration(+1); break;
            case South: change_vibration(-1); break;
            case East: change_trigger_mode(+1); break;
            case West: change_trigger_mode(-1); break;
            default: break;
        }
    }
    prev_dpad = dpad;
    if (mute) {
        io[2] &= static_cast<uint8_t>(~0x0F); // D-pad bits of Switch report byte 4
    }
}

bool switch_settings_trigger_pressed(bool digital, uint8_t analog) {
    const TriggerMode &mode = TRIGGER_TABLE[trigger_mode - 1];
    if (trigger_mode == 1) {
        return digital || analog > mode.threshold;
    }
    return analog >= mode.threshold;
}

float switch_settings_vibration_scale() {
    return static_cast<float>(vib_level) / VIB_LEVELS;
}

void switch_settings_on_connect() {
    if (!is_switch_pro_mode()) {
        return;
    }
    trigger_apply_pending = true;
}

void switch_settings_task() {
    if (!is_switch_pro_mode()) {
        return;
    }
    const uint32_t now = now_ms();
    if (bt_is_connected()) {
        if (trigger_apply_pending) {
            trigger_apply_pending = false;
            apply_trigger_effect();
        }
        if (player_led_pending) {
            player_led_pending = false;
            show_player_leds(vib_level);
            player_led_clear_ms = now + PLAYER_LED_MS;
        } else if (player_led_clear_ms != 0 && static_cast<int32_t>(now - player_led_clear_ms) >= 0) {
            player_led_clear_ms = 0;
            show_player_leds(0);
        }
        if (mute_toggles_left > 0 && (mute_last_ms == 0 || now - mute_last_ms >= MUTE_BLINK_MS)) {
            mute_last_ms = now;
            mute_lit = !mute_lit;
            set_mute_led(mute_lit);
            if (--mute_toggles_left == 0) {
                macro_restore_mute_light(); // back to the macro's Breathing / On / Off
            }
        }
    }
    if (save_pending && static_cast<int32_t>(now - save_at_ms) >= 0) {
        save_pending = false;
        save();
    }
}

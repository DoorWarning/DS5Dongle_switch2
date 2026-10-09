// Switch Pro mode settings, changed from the controller or the manager app and
// kept in flash.
//
// The PC web config can't reach the dongle while it sits in the dock, so these
// live in their own flash record instead of Config_body (which the web config
// reads and writes).
//
//  - Mute + D-pad Up/Down: vibration 0-100 % in 20 % steps (default 60 %). The
//    step shows on the player LEDs for PLAYER_LED_MS, with a short test pulse at
//    the new strength. The manager app can set any percentage.
//  - Mute + D-pad Right/Left: next/previous trigger mode. The mode number shows
//    as mute LED blinks.
//  - Turbo (macro.cpp detects the gestures): Mute + double tap on a turbo button
//    toggles it, a Mute tap alone clears all. Held turbo buttons pulse at
//    turbo_rate presses per second.
//
// Trigger modes use DualSense raw adaptive trigger effects, FFB[11] (the
// encoding ds.daidr.me's tester uses), with a separate effect and fire point for
// L2 and R2. Switch ZL/ZR are digital, so each mode fires them where its feel
// says "pressed". The defaults:
//   1 Normal:  no effect; DS5 digital bit or analog > 32 (threshold 0)
//   2 Click:   0x02 section, resistance that gives way; fires at the click
//   3 Short:   0x01 strong resistance near the top, so the trigger acts like a button
//   4 Auto:    0x06 vibrating trigger (automatic gun) from a start point
//
// The settings live on the heap, allocated only in Switch Pro mode: PC mode has
// just a few KB of heap left after core1's opus allocations.

#include "switch_settings.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
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

constexpr uint8_t VIB_STEPS = 5; // 20 % per step
constexpr uint8_t VIB_STEP_PERCENT = 100 / VIB_STEPS;
constexpr uint8_t DEFAULT_VIB_PERCENT = 60;
constexpr uint8_t DEFAULT_TRIGGER_MODE = 1;
constexpr uint8_t DEFAULT_TURBO_RATE = 10;
constexpr uint8_t NORMAL_TRIGGER_THRESHOLD = 32;
constexpr uint32_t SAVE_DELAY_MS = 3000;
constexpr uint32_t PLAYER_LED_MS = 2000;
constexpr uint32_t MUTE_BLINK_MS = 250;

constexpr uint32_t RECORD_MAGIC = 0x53574354; // "SWCT"
constexpr uint8_t RECORD_VERSION = 2;
// Below the wake beacon sector (switch_wake.cpp), which is below the macro slots.
constexpr uint32_t SETTINGS_FLASH_OFFSET = PICO_FLASH_BANK_STORAGE_OFFSET - 7 * FLASH_SECTOR_SIZE;
static_assert(SETTINGS_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

// switch-v2 record: vibration level 0-5 and trigger mode only.
struct __attribute__((packed)) SettingsRecordV1 {
    uint32_t magic;
    uint8_t version;
    uint8_t vib_level;
    uint8_t trigger_mode;
    uint8_t reserved;
    uint32_t crc;
};

struct __attribute__((packed)) SettingsRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    NsSettings settings;
    uint32_t crc;
};
static_assert(sizeof(SettingsRecord) <= FLASH_PAGE_SIZE);

NsSettings *ns = nullptr;

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

uint32_t record_crc(const uint8_t *record, size_t len) {
    return crc32(record, len);
}

void default_slot(NsTriggerSlot &slot, const uint8_t effect[4], uint8_t threshold) {
    memset(&slot, 0, sizeof(slot));
    for (int side = 0; side < 2; side++) {
        memcpy(slot.effect[side], effect, 4);
        slot.threshold[side] = threshold;
    }
}

void set_defaults(NsSettings &s) {
    // Thresholds are starting points: effect position and analog value are only roughly linear.
    constexpr uint8_t NORMAL[4] = {0x00, 0, 0, 0};
    constexpr uint8_t CLICK[4] = {0x02, 15, 100, 255}; // section start 15, end 100, force 255
    constexpr uint8_t SHORT[4] = {0x01, 40, 255, 0};   // resistance from 40, force 255
    constexpr uint8_t AUTO[4] = {0x06, 10, 255, 20};   // frequency 10, force 255, start 20
    s.vib_percent = DEFAULT_VIB_PERCENT;
    s.trigger_mode = DEFAULT_TRIGGER_MODE;
    s.turbo_mask = 0;
    s.turbo_rate = DEFAULT_TURBO_RATE;
    default_slot(s.slots[0], NORMAL, 0);
    default_slot(s.slots[1], CLICK, 100);
    default_slot(s.slots[2], SHORT, 30);
    default_slot(s.slots[3], AUTO, 20);
}

bool settings_valid(const NsSettings &s) {
    return s.vib_percent <= 100 && s.trigger_mode >= 1 && s.trigger_mode <= NS_TRIGGER_SLOTS &&
           s.turbo_rate >= TURBO_RATE_MIN && s.turbo_rate <= TURBO_RATE_MAX;
}

void load() {
    const auto *flash = reinterpret_cast<const uint8_t *>(XIP_BASE + SETTINGS_FLASH_OFFSET);
    const auto &v2 = *reinterpret_cast<const SettingsRecord *>(flash);
    const auto &v1 = *reinterpret_cast<const SettingsRecordV1 *>(flash);
    if (v2.magic == RECORD_MAGIC && v2.version == RECORD_VERSION &&
        v2.crc == record_crc(flash, offsetof(SettingsRecord, crc)) && settings_valid(v2.settings)) {
        *ns = v2.settings;
    } else if (v1.magic == RECORD_MAGIC && v1.version == 1 &&
               v1.crc == record_crc(flash, offsetof(SettingsRecordV1, crc)) &&
               v1.vib_level <= VIB_STEPS && v1.trigger_mode >= 1 && v1.trigger_mode <= NS_TRIGGER_SLOTS) {
        ns->vib_percent = v1.vib_level * VIB_STEP_PERCENT;
        ns->trigger_mode = v1.trigger_mode;
    }
}

void save_flash_op(void *param) {
    const auto *page = static_cast<const uint8_t *>(param);
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(interrupts);
}

void save() {
    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    auto &r = *reinterpret_cast<SettingsRecord *>(page);
    r.magic = RECORD_MAGIC;
    r.version = RECORD_VERSION;
    memset(r.reserved, 0, sizeof(r.reserved));
    r.settings = *ns;
    r.crc = record_crc(page, offsetof(SettingsRecord, crc));
    if (memcmp(reinterpret_cast<const void *>(XIP_BASE + SETTINGS_FLASH_OFFSET), &r, sizeof(r)) == 0) {
        return;
    }
    watchdog_update();
    const int rc = flash_safe_execute(save_flash_op, page, 1000);
    watchdog_update();
    printf("[Settings] saved vib=%u%% trigger=%u turbo=0x%02X rate=%u rc=%d\n",
           ns->vib_percent, ns->trigger_mode, ns->turbo_mask, ns->turbo_rate, rc);
}

void schedule_save() {
    save_pending = true;
    save_at_ms = now_ms() + SAVE_DELAY_MS;
}

void apply_trigger_effect() {
    SetStateData s{};
    s.AllowRightTriggerFFB = 1;
    s.AllowLeftTriggerFFB = 1;
    const NsTriggerSlot &slot = ns->slots[ns->trigger_mode - 1];
    memcpy(s.LeftTriggerFFB, slot.effect[0], NS_FFB_SIZE);
    memcpy(s.RightTriggerFFB, slot.effect[1], NS_FFB_SIZE);
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

uint8_t vib_step() {
    return static_cast<uint8_t>((ns->vib_percent + VIB_STEP_PERCENT / 2) / VIB_STEP_PERCENT);
}

void change_vibration(int delta) {
    const int next = static_cast<int>(vib_step()) + delta;
    if (next < 0 || next > VIB_STEPS) {
        player_led_pending = true; // at the end of the range: just show it again
        return;
    }
    ns->vib_percent = static_cast<uint8_t>(next * VIB_STEP_PERCENT);
    player_led_pending = true;
    if (ns->vib_percent > 0) {
        switch_hd_haptics_test_pulse();
    }
    schedule_save();
    printf("[Settings] vibration %u%%\n", ns->vib_percent);
}

void change_trigger_mode(int delta) {
    ns->trigger_mode = static_cast<uint8_t>((ns->trigger_mode - 1 + NS_TRIGGER_SLOTS + delta) % NS_TRIGGER_SLOTS + 1);
    trigger_apply_pending = true;
    switch_settings_blink_mute(ns->trigger_mode);
    schedule_save();
    printf("[Settings] trigger mode %u\n", ns->trigger_mode);
}

} // namespace

void switch_settings_init() {
    if (ns == nullptr) {
        ns = static_cast<NsSettings *>(malloc(sizeof(NsSettings)));
        if (ns == nullptr) {
            return;
        }
    }
    set_defaults(*ns);
    load();
    printf("[Settings] vibration %u%%, trigger mode %u, turbo 0x%02X at %u/s\n",
           ns->vib_percent, ns->trigger_mode, ns->turbo_mask, ns->turbo_rate);
}

void switch_settings_on_input(const USBGetStateData &ds5, uint8_t io[9]) {
    const bool mute = ds5.ButtonMute;
    const Direction dpad = mute ? ds5.DPad : None;
    if (ns != nullptr && mute && dpad != prev_dpad) {
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

bool switch_settings_trigger_pressed(bool left, bool digital, uint8_t analog) {
    const uint8_t threshold = ns != nullptr ? ns->slots[ns->trigger_mode - 1].threshold[left ? 0 : 1] : 0;
    if (threshold == 0) {
        return digital || analog > NORMAL_TRIGGER_THRESHOLD;
    }
    return analog >= threshold;
}

float switch_settings_vibration_scale() {
    return ns != nullptr ? static_cast<float>(ns->vib_percent) / 100.0f : 1.0f;
}

void switch_settings_apply_turbo(uint8_t io[9]) {
    if (ns == nullptr || ns->turbo_mask == 0) {
        return;
    }
    // Two half periods per press: buttons read released during the odd one.
    const uint32_t half = static_cast<uint32_t>(time_us_64() / (500000 / ns->turbo_rate));
    if ((half & 1) == 0) {
        return;
    }
    const uint8_t m = ns->turbo_mask;
    // Switch report byte 2: West 0x01, North 0x02, South 0x04, East 0x08, R 0x40, ZR 0x80.
    uint8_t clear0 = 0;
    if (m & TURBO_SQUARE) clear0 |= 0x01;
    if (m & TURBO_TRIANGLE) clear0 |= 0x02;
    if (m & TURBO_CROSS) clear0 |= 0x04;
    if (m & TURBO_CIRCLE) clear0 |= 0x08;
    if (m & TURBO_R1) clear0 |= 0x40;
    if (m & TURBO_R2) clear0 |= 0x80;
    // Byte 4: L 0x40, ZL 0x80.
    uint8_t clear2 = 0;
    if (m & TURBO_L1) clear2 |= 0x40;
    if (m & TURBO_L2) clear2 |= 0x80;
    io[0] &= static_cast<uint8_t>(~clear0);
    io[2] &= static_cast<uint8_t>(~clear2);
}

void switch_settings_toggle_turbo(uint8_t bit) {
    if (ns == nullptr) {
        return;
    }
    ns->turbo_mask ^= bit;
    const bool on = (ns->turbo_mask & bit) != 0;
    if (on && ns->vib_percent > 0) {
        switch_hd_haptics_test_pulse();
    }
    switch_settings_blink_mute(on ? 2 : 1);
    schedule_save();
    printf("[Settings] turbo 0x%02X\n", ns->turbo_mask);
}

void switch_settings_clear_turbo() {
    if (ns == nullptr || ns->turbo_mask == 0) {
        return;
    }
    ns->turbo_mask = 0;
    switch_settings_blink_mute(1);
    schedule_save();
    printf("[Settings] turbo off\n");
}

void switch_settings_blink_mute(uint8_t count) {
    mute_toggles_left = static_cast<uint8_t>(count * 2);
    mute_last_ms = 0;
    mute_lit = false;
}

const NsSettings *switch_settings_get() {
    return ns;
}

bool switch_settings_set(const NsSettings &next) {
    if (ns == nullptr || !settings_valid(next)) {
        return false;
    }
    *ns = next;
    trigger_apply_pending = true;
    schedule_save();
    printf("[Settings] set by app: vib=%u%% trigger=%u turbo=0x%02X rate=%u\n",
           ns->vib_percent, ns->trigger_mode, ns->turbo_mask, ns->turbo_rate);
    return true;
}

void switch_settings_on_connect() {
    if (!is_switch_pro_mode()) {
        return;
    }
    trigger_apply_pending = true;
}

void switch_settings_task() {
    if (!is_switch_pro_mode() || ns == nullptr) {
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
            show_player_leds(vib_step());
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

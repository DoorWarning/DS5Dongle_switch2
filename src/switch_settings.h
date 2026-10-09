#pragma once

#include <cstdint>

#include "ds5_input.h"

// Switch Pro mode settings: vibration strength, trigger modes and turbo. Changed
// from the controller or the manager app, kept in flash. See switch_settings.cpp.

constexpr uint8_t NS_TRIGGER_SLOTS = 4;
constexpr uint8_t NS_FFB_SIZE = 11;
constexpr uint8_t TURBO_RATE_MIN = 2;
constexpr uint8_t TURBO_RATE_MAX = 30;

// Turbo bits (NsSettings::turbo_mask).
enum TurboBit : uint8_t {
    TURBO_CIRCLE = 0x01,
    TURBO_CROSS = 0x02,
    TURBO_TRIANGLE = 0x04,
    TURBO_SQUARE = 0x08,
    TURBO_L1 = 0x10,
    TURBO_R1 = 0x20,
    TURBO_L2 = 0x40,
    TURBO_R2 = 0x80,
};

struct __attribute__((packed)) NsTriggerSlot {
    uint8_t effect[2][NS_FFB_SIZE]; // raw DualSense adaptive trigger effect: [0] L2, [1] R2
    uint8_t threshold[2];           // analog value where ZL/ZR fire; 0 = digital bit or > 32
};

// The block the manager app reads and writes (companion NS_SETTINGS_*).
struct __attribute__((packed)) NsSettings {
    uint8_t vib_percent;  // 0-100
    uint8_t trigger_mode; // 1-NS_TRIGGER_SLOTS
    uint8_t turbo_mask;   // TurboBit
    uint8_t turbo_rate;   // presses per second, TURBO_RATE_MIN-TURBO_RATE_MAX
    NsTriggerSlot slots[NS_TRIGGER_SLOTS];
};

void switch_settings_init();
// Handles Mute + D-pad and masks the D-pad from the Switch while Mute is held.
// io = Switch report bytes 2..10 (buttons + sticks), as passed to macro_process().
void switch_settings_on_input(const USBGetStateData &ds5, uint8_t io[9]);
// ZL/ZR state for the current trigger mode (left = L2).
bool switch_settings_trigger_pressed(bool left, bool digital, uint8_t analog);
// Haptics output scale, 0.0 (off) to 1.0.
float switch_settings_vibration_scale();
void switch_settings_on_connect(); // DualSense connected: apply the trigger effect
void switch_settings_task();

// Turbo: pulses the held turbo buttons in io (Switch report bytes 2..10).
void switch_settings_apply_turbo(uint8_t io[9]);
void switch_settings_toggle_turbo(uint8_t bit);
void switch_settings_clear_turbo();
// Short mute LED feedback (count blinks), then back to the macro's mute light.
void switch_settings_blink_mute(uint8_t count);

// Manager app access (companion.cpp). Null outside Switch Pro mode.
const NsSettings *switch_settings_get();
// Validates and applies a full settings block; false if out of range.
bool switch_settings_set(const NsSettings &next);

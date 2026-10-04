#pragma once

#include <cstdint>

#include "ds5_input.h"

// Switch Pro mode settings changed from the controller: vibration level and
// trigger mode. See switch_settings.cpp.
void switch_settings_init();
// Handles Mute + D-pad and masks the D-pad from the Switch while Mute is held.
// io = Switch report bytes 2..10 (buttons + sticks), as passed to macro_process().
void switch_settings_on_input(const USBGetStateData &ds5, uint8_t io[9]);
// ZL/ZR state for the current trigger mode.
bool switch_settings_trigger_pressed(bool digital, uint8_t analog);
// Haptics output scale, 0.0 (off) to 1.0.
float switch_settings_vibration_scale();
void switch_settings_on_connect(); // DualSense connected: apply the trigger effect
void switch_settings_task();

#pragma once

#include <cstdint>

void mode_toggle_on_input(bool create, bool options, bool mute);
// Switch to `mode` (ControllerMode), save and reboot after delay_ms, so a
// companion reply can still be read before the USB device goes away.
void mode_request(uint8_t mode, uint32_t delay_ms);
void mode_toggle_task();

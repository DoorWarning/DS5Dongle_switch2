#pragma once

#include <cstdint>

constexpr uint8_t MACRO_SLOTS = 4;
constexpr uint8_t MACRO_EVENT_SIZE = 12; // MacroEvent: dur_ms u16, buttons[3], sticks[6], reserved

// Raw DualSense buttons the macro and turbo gestures look at.
struct MacroInput {
    bool mute;
    bool circle, cross, triangle, square;
    bool l1, r1, l2, r2; // l2/r2: ZL/ZR as the trigger mode reports them
    bool other;          // any other button or the D-pad
};

void macro_init();
// io = Switch report bytes 2..10 (buttons + sticks).
void macro_process(uint8_t io[9], const MacroInput &in);
void macro_task();
// Re-send the mute LED state the macro wants (Breathing / On / Off), e.g. after
// switch_settings.cpp blinked it.
void macro_restore_mute_light();

// Manager app access (companion.cpp), Switch Pro mode only.
enum class MacroStatus : uint8_t { Idle = 0, Recording = 1, Playing = 2, Unavailable = 3 };
MacroStatus macro_status(int8_t &active_slot);
uint16_t macro_max_events();
uint16_t macro_event_count(int slot); // 0 for an empty slot
// Copies up to len bytes of the slot's event array from byte offset; returns the count copied.
uint16_t macro_read(int slot, uint16_t offset, uint8_t *out, uint16_t len);
// Editing: begin claims the record buffer, write fills the event array, commit saves.
bool macro_edit_begin(int slot);
bool macro_edit_write(uint16_t offset, const uint8_t *data, uint16_t len);
bool macro_edit_commit(uint16_t count);

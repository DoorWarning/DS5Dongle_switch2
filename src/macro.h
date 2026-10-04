#pragma once

#include <cstdint>

void macro_init();
void macro_process(uint8_t io[9], bool mute, bool circle, bool cross, bool triangle, bool square);
void macro_task();
// Re-send the mute LED state the macro wants (Breathing / On / Off), e.g. after
// switch_settings.cpp blinked it.
void macro_restore_mute_light();

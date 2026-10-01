#pragma once

#include <cstdint>

void macro_init();
void macro_process(uint8_t io[9], bool mute, bool circle, bool cross, bool triangle, bool square);
void macro_task();

#ifndef DS5_BRIDGE_SWITCH_HD_HAPTICS_H
#define DS5_BRIDGE_SWITCH_HD_HAPTICS_H

#include <cstdint>

void switch_hd_haptics_set_rumble(const uint8_t left[4], const uint8_t right[4]);
void switch_hd_haptics_task();
void switch_hd_haptics_stop();

#endif // DS5_BRIDGE_SWITCH_HD_HAPTICS_H

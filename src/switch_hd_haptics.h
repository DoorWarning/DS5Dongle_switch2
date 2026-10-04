#ifndef DS5_BRIDGE_SWITCH_HD_HAPTICS_H
#define DS5_BRIDGE_SWITCH_HD_HAPTICS_H

#include <cstdint>

void switch_hd_haptics_set_rumble(const uint8_t left[4], const uint8_t right[4]);
void switch_hd_haptics_task();
void switch_hd_haptics_stop();
// Short low-frequency pulse at the current vibration level (ends by the rumble timeout).
void switch_hd_haptics_test_pulse();

#endif // DS5_BRIDGE_SWITCH_HD_HAPTICS_H

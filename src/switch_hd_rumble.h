#ifndef DS5_BRIDGE_SWITCH_HD_RUMBLE_H
#define DS5_BRIDGE_SWITCH_HD_RUMBLE_H

#include <cstdint>

struct SwitchHdRumbleBand {
    float frequency_hz;
    float amplitude;
};

struct SwitchHdRumble {
    SwitchHdRumbleBand low;
    SwitchHdRumbleBand high;
};

SwitchHdRumble switch_hd_rumble_decode(const uint8_t data[4]);
bool switch_hd_rumble_has_energy(const SwitchHdRumble &rumble);

#endif // DS5_BRIDGE_SWITCH_HD_RUMBLE_H

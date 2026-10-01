#include "switch_hd_rumble.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float MIN_SWITCH_RUMBLE_FREQ_HZ = 20.0f;
constexpr float MAX_SWITCH_RUMBLE_FREQ_HZ = 1250.0f;

float clampf(float value, float min_value, float max_value) {
    return std::min(std::max(value, min_value), max_value);
}

float decode_frequency(uint16_t encoded_freq) {
    const float freq = 10.0f * std::exp2(static_cast<float>(encoded_freq) / 32.0f);
    return clampf(freq, MIN_SWITCH_RUMBLE_FREQ_HZ, MAX_SWITCH_RUMBLE_FREQ_HZ);
}

float decode_amplitude(uint8_t encoded_amp) {
    if (encoded_amp == 0) {
        return 0.0f;
    }

    float amp;
    if (encoded_amp == 1) {
        amp = 0.007843f;
    } else if (encoded_amp < 16) {
        amp = 0.011823f * std::exp2((static_cast<float>(encoded_amp) - 2.0f) / 4.0f);
    } else if (encoded_amp < 32) {
        amp = std::exp2(static_cast<float>(encoded_amp) / 16.0f) / 17.0f;
    } else {
        amp = std::exp2(static_cast<float>(encoded_amp) / 32.0f) / 8.7f;
    }

    return clampf(amp, 0.0f, 1.0f);
}

} // namespace

SwitchHdRumble switch_hd_rumble_decode(const uint8_t data[4]) {
    const uint16_t high_freq_encoded =
        (static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1] & 0x01) << 8)) / 4 + 0x60;
    const uint16_t low_freq_encoded = (data[2] & 0x7F) + 0x40;
    const uint8_t high_amp_encoded = (data[1] & 0xFE) / 2;
    const uint8_t low_amp_base = data[3] > 0x40 ? static_cast<uint8_t>(data[3] - 0x40) : 0;
    const uint8_t low_amp_encoded =
        static_cast<uint8_t>(std::min<uint16_t>(low_amp_base * 2 + ((data[2] & 0x80) ? 1 : 0), 255));

    return {
        {
            decode_frequency(low_freq_encoded),
            decode_amplitude(low_amp_encoded),
        },
        {
            decode_frequency(high_freq_encoded),
            decode_amplitude(high_amp_encoded),
        },
    };
}

bool switch_hd_rumble_has_energy(const SwitchHdRumble &rumble) {
    return rumble.low.amplitude > 0.001f || rumble.high.amplitude > 0.001f;
}

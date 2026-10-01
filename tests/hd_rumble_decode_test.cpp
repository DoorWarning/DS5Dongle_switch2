#include "switch_hd_rumble.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        std::exit(1);
    }
}

void require_near(float actual, float expected, float tolerance, const char *message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::printf("FAIL: %s: expected %.4f, got %.4f\n", message, expected, actual);
        std::exit(1);
    }
}

uint8_t encode_frequency(float freq_hz) {
    return static_cast<uint8_t>(std::round(std::log2(freq_hz / 10.0f) * 32.0f));
}

uint8_t encode_amplitude(float amp) {
    if (amp <= 0.0f) {
        return 0;
    }
    if (amp > 0.23f) {
        return static_cast<uint8_t>(std::round(std::log2(amp * 8.7f) * 32.0f));
    }
    if (amp > 0.12f) {
        return static_cast<uint8_t>(std::round(std::log2(amp * 17.0f) * 16.0f));
    }
    return static_cast<uint8_t>(std::round(amp / 0.12f * 16.0f));
}

std::array<uint8_t, 4> encode_rumble(float high_freq_hz, float high_amp,
                                     float low_freq_hz, float low_amp) {
    const uint16_t high_freq =
        static_cast<uint16_t>((encode_frequency(high_freq_hz) - 0x60) * 4);
    const uint8_t low_freq = static_cast<uint8_t>(encode_frequency(low_freq_hz) - 0x40);
    const uint8_t high_amp_encoded = static_cast<uint8_t>(encode_amplitude(high_amp) * 2);
    const uint16_t low_amp_encoded = static_cast<uint16_t>(encode_amplitude(low_amp) / 2 + 0x40);

    return {
        static_cast<uint8_t>(high_freq & 0xFF),
        static_cast<uint8_t>(high_amp_encoded + ((high_freq >> 8) & 0x01)),
        static_cast<uint8_t>(low_freq + ((low_amp_encoded >> 8) & 0x01)),
        static_cast<uint8_t>(low_amp_encoded & 0xFF),
    };
}

} // namespace

int main() {
    {
        const uint8_t neutral[] = {0x00, 0x01, 0x40, 0x40};
        const SwitchHdRumble decoded = switch_hd_rumble_decode(neutral);
        require_near(decoded.high.frequency_hz, 320.0f, 1.0f, "neutral high frequency");
        require_near(decoded.low.frequency_hz, 160.0f, 1.0f, "neutral low frequency");
        require_near(decoded.high.amplitude, 0.0f, 0.0001f, "neutral high amplitude");
        require_near(decoded.low.amplitude, 0.0f, 0.0001f, "neutral low amplitude");
        require(!switch_hd_rumble_has_energy(decoded), "neutral has no energy");
    }

    {
        const auto encoded = encode_rumble(320.0f, 0.50f, 160.0f, 0.25f);
        const SwitchHdRumble decoded = switch_hd_rumble_decode(encoded.data());
        require_near(decoded.high.frequency_hz, 320.0f, 1.0f, "round-trip high frequency");
        require_near(decoded.low.frequency_hz, 160.0f, 1.0f, "round-trip low frequency");
        require_near(decoded.high.amplitude, 0.50f, 0.05f, "round-trip high amplitude");
        require_near(decoded.low.amplitude, 0.25f, 0.05f, "round-trip low amplitude");
        require(switch_hd_rumble_has_energy(decoded), "round-trip has energy");
    }

    {
        const uint8_t low_level_1[] = {0x00, 0x03, 0x40, 0x40};
        const uint8_t low_level_8[] = {0x00, 0x11, 0x40, 0x40};
        const SwitchHdRumble decoded_1 = switch_hd_rumble_decode(low_level_1);
        const SwitchHdRumble decoded_8 = switch_hd_rumble_decode(low_level_8);
        require_near(decoded_1.high.amplitude, 0.007843f, 0.0001f, "low amplitude level 1");
        require_near(decoded_8.high.amplitude, 0.033442f, 0.0001f, "low amplitude level 8");
        require(decoded_8.high.amplitude < 0.12f, "subtle high amplitude stays subtle");
    }

    {
        const uint8_t low_no_flag[] = {0x00, 0x01, 0x40, 0x41};
        const uint8_t low_with_flag[] = {0x00, 0x01, 0xC0, 0x41};
        const SwitchHdRumble no_flag = switch_hd_rumble_decode(low_no_flag);
        const SwitchHdRumble with_flag = switch_hd_rumble_decode(low_with_flag);
        require(with_flag.low.amplitude > no_flag.low.amplitude,
                "low amplitude intermediate bit increases amplitude");
    }

    std::printf("PASS: hd_rumble_decode\n");
    return 0;
}

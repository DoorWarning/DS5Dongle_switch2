//
// Switch HD rumble to DualSense haptics synthesis.
//

#include "switch_hd_haptics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "bt.h"
#include "config.h"
#include "pico/time.h"
#include "switch_hd_rumble.h"
#include "switch_settings.h"

namespace {

constexpr uint8_t DS5_OUTPUT_REPORT_ID = 0x36;
constexpr uint16_t DS5_OUTPUT_REPORT_SIZE = 398;
constexpr uint8_t DS5_HAPTIC_SAMPLE_SIZE = 64;
constexpr uint8_t DS5_HAPTIC_CHANNELS = 2;
constexpr uint32_t DS5_HAPTIC_SAMPLE_RATE = 3000;
constexpr uint32_t DS5_HAPTIC_FRAMES_PER_PACKET = DS5_HAPTIC_SAMPLE_SIZE / DS5_HAPTIC_CHANNELS;
constexpr uint32_t DS5_HAPTIC_PACKET_INTERVAL_US =
    DS5_HAPTIC_FRAMES_PER_PACKET * 1000000u / DS5_HAPTIC_SAMPLE_RATE;
constexpr uint32_t SWITCH_RUMBLE_TIMEOUT_US = 120000;
constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float AMP_ATTACK_PER_SAMPLE = 0.18f;
constexpr float AMP_RELEASE_PER_SAMPLE = 0.075f;
constexpr float FREQ_GLIDE_PER_SAMPLE = 0.08f;
constexpr float HF_OUTPUT_GAIN = 0.40f;
constexpr float LF_OUTPUT_GAIN = 0.72f;
constexpr float TRANSIENT_FREQ_HZ = 190.0f;
constexpr float TRANSIENT_DECAY_PER_SAMPLE = 0.82f;
constexpr float TRANSIENT_MIN_DELTA = 0.16f;
constexpr float TRANSIENT_MAX_AMP = 0.62f;
constexpr float CROSSFEED = 0.06f;
constexpr float OUTPUT_DRIVE = 1.55f;
constexpr float OUTPUT_HEADROOM = 0.88f;

struct RumbleBand {
    float target_freq_hz = 160.0f;
    float freq_hz = 160.0f;
    float target_amp = 0.0f;
    float amp = 0.0f;
    float phase = 0.0f;
};

struct RumbleVoice {
    RumbleBand low;
    RumbleBand high;
    float transient_amp = 0.0f;
    float transient_phase = 0.0f;
};

RumbleVoice voices[2]{};
uint8_t report_seq = 0;
uint8_t packet_counter = 0;
uint32_t last_packet_us = 0;
uint32_t last_update_us = 0;
uint8_t zero_packets_remaining = 0;
bool packet_requested = false;
bool silenced = false; // vibration level 0: the zero packet went out

float clampf(float value, float min_value, float max_value) {
    return std::min(std::max(value, min_value), max_value);
}

float perceptual_amplitude(float amplitude) {
    if (amplitude < 0.004f) {
        return 0.0f;
    }

    const float lifted = std::pow(clampf(amplitude, 0.0f, 1.0f), 0.62f);
    return 0.015f + lifted * 0.985f;
}

float warp_frequency(float freq_hz) {
    float freq = clampf(freq_hz, 40.0f, 1280.0f);
    if (freq > 650.0f) {
        while (freq > 430.0f) {
            freq *= 0.5f;
        }
        return clampf(freq, 140.0f, 430.0f);
    }
    if (freq > 320.0f) {
        return 320.0f + (freq - 320.0f) * 0.36f;
    }
    return freq;
}

float frequency_eq(float freq_hz) {
    struct EqPoint {
        float freq_hz;
        float gain;
    };

    constexpr EqPoint points[] = {
        {40.0f, 0.78f},
        {60.0f, 0.95f},
        {80.0f, 1.10f},
        {120.0f, 1.14f},
        {160.0f, 1.00f},
        {220.0f, 0.94f},
        {320.0f, 0.82f},
        {450.0f, 0.64f},
    };

    if (freq_hz <= points[0].freq_hz) {
        return points[0].gain;
    }
    for (size_t i = 1; i < std::size(points); ++i) {
        if (freq_hz <= points[i].freq_hz) {
            const float t = (freq_hz - points[i - 1].freq_hz) /
                            (points[i].freq_hz - points[i - 1].freq_hz);
            return points[i - 1].gain + (points[i].gain - points[i - 1].gain) * t;
        }
    }
    return points[std::size(points) - 1].gain;
}

float band_target_amp(const SwitchHdRumbleBand &band, float output_gain) {
    const float freq = warp_frequency(band.frequency_hz);
    const float amp = perceptual_amplitude(band.amplitude);
    return clampf(amp * frequency_eq(freq) * output_gain, 0.0f, 1.0f);
}

float voice_target_energy(const RumbleVoice &voice) {
    return std::max(voice.low.target_amp, voice.high.target_amp);
}

float voice_dominant_frequency(const RumbleVoice &voice) {
    const float low_weight = voice.low.target_amp;
    const float high_weight = voice.high.target_amp;
    const float weight = low_weight + high_weight;
    if (weight < 0.001f) {
        return 160.0f;
    }
    return (voice.low.target_freq_hz * low_weight + voice.high.target_freq_hz * high_weight) / weight;
}

RumbleVoice voice_from_decoded(const SwitchHdRumble &decoded) {
    RumbleVoice voice{};
    voice.low.target_freq_hz = warp_frequency(decoded.low.frequency_hz);
    voice.low.freq_hz = voice.low.target_freq_hz;
    voice.low.target_amp = band_target_amp(decoded.low, LF_OUTPUT_GAIN);
    voice.high.target_freq_hz = warp_frequency(decoded.high.frequency_hz);
    voice.high.freq_hz = voice.high.target_freq_hz;
    voice.high.target_amp = band_target_amp(decoded.high, HF_OUTPUT_GAIN);
    return voice;
}

RumbleVoice voice_from_switch_rumble(const uint8_t data[4]) {
    return voice_from_decoded(switch_hd_rumble_decode(data));
}

bool voice_has_energy(const RumbleVoice &voice) {
    return voice.low.target_amp > 0.001f || voice.high.target_amp > 0.001f ||
           voice.low.amp > 0.001f || voice.high.amp > 0.001f ||
           voice.transient_amp > 0.001f;
}

bool any_voice_has_energy() {
    return voice_has_energy(voices[0]) || voice_has_energy(voices[1]);
}

void keep_phase(RumbleBand &dst, const RumbleBand &src) {
    dst.phase = src.phase;
}

void update_voice(uint8_t index, const RumbleVoice &decoded) {
    RumbleVoice next = decoded;
    keep_phase(next.low, voices[index].low);
    keep_phase(next.high, voices[index].high);
    next.low.freq_hz = voices[index].low.freq_hz;
    next.high.freq_hz = voices[index].high.freq_hz;
    next.low.amp = voices[index].low.amp;
    next.high.amp = voices[index].high.amp;
    next.transient_amp = voices[index].transient_amp;
    next.transient_phase = voices[index].transient_phase;

    const float old_energy = voice_target_energy(voices[index]);
    const float new_energy = voice_target_energy(next);
    const float energy_delta = new_energy - old_energy;
    const float old_freq = voice_dominant_frequency(voices[index]);
    const float new_freq = voice_dominant_frequency(next);
    const float freq_step = old_energy > 0.02f && new_energy > 0.02f
                                ? std::fabs(std::log2(new_freq / old_freq))
                                : 0.0f;

    if ((old_energy < 0.01f && new_energy > 0.035f) ||
        energy_delta > TRANSIENT_MIN_DELTA ||
        freq_step > 0.45f) {
        const float impulse = 0.16f +
                              std::max(energy_delta, 0.0f) * 0.70f +
                              freq_step * 0.12f;
        next.transient_amp = std::max(next.transient_amp,
                                      clampf(impulse, 0.0f, TRANSIENT_MAX_AMP));
        next.transient_phase = 0.0f;
    }

    voices[index] = next;
}

void advance_band(RumbleBand &band) {
    const float smoothing = band.target_amp > band.amp ? AMP_ATTACK_PER_SAMPLE : AMP_RELEASE_PER_SAMPLE;
    band.amp += (band.target_amp - band.amp) * smoothing;
    band.freq_hz += (band.target_freq_hz - band.freq_hz) * FREQ_GLIDE_PER_SAMPLE;
    band.phase += band.freq_hz * TWO_PI / static_cast<float>(DS5_HAPTIC_SAMPLE_RATE);
    while (band.phase >= TWO_PI) {
        band.phase -= TWO_PI;
    }
}

float shaped_sine(float phase, float harmonic_mix) {
    const float fundamental = std::sin(phase);
    const float harmonic = std::sin(phase * 2.0f) * harmonic_mix;
    return fundamental + harmonic;
}

float render_transient(RumbleVoice &voice) {
    if (voice.transient_amp <= 0.001f) {
        voice.transient_amp = 0.0f;
        return 0.0f;
    }

    voice.transient_phase += TRANSIENT_FREQ_HZ * TWO_PI / static_cast<float>(DS5_HAPTIC_SAMPLE_RATE);
    while (voice.transient_phase >= TWO_PI) {
        voice.transient_phase -= TWO_PI;
    }
    const float sample = std::sin(voice.transient_phase) * voice.transient_amp;
    voice.transient_amp *= TRANSIENT_DECAY_PER_SAMPLE;
    return sample;
}

float render_voice(RumbleVoice &voice) {
    advance_band(voice.low);
    advance_band(voice.high);

    return shaped_sine(voice.low.phase, 0.08f) * voice.low.amp +
           shaped_sine(voice.high.phase, 0.18f) * voice.high.amp +
           render_transient(voice);
}

int8_t float_to_s8(float sample, float gain) {
    const float driven = sample * gain;
    const float limited = std::tanh(driven * OUTPUT_DRIVE) / std::tanh(OUTPUT_DRIVE);
    const int value = static_cast<int>(limited * OUTPUT_HEADROOM * 127.0f);
    return static_cast<int8_t>(std::clamp(value, -128, 127));
}

void send_haptics_packet(const int8_t haptics[DS5_HAPTIC_SAMPLE_SIZE]) {
    uint8_t pkt[DS5_OUTPUT_REPORT_SIZE]{};
    pkt[0] = DS5_OUTPUT_REPORT_ID;
    pkt[1] = static_cast<uint8_t>((report_seq++ & 0x0F) << 4);
    pkt[2] = 0x11 | (1 << 7);
    pkt[3] = 7;
    pkt[4] = 0b11111110;

    const uint8_t buffer_length = get_config().audio_buffer_length;
    pkt[5] = buffer_length;
    pkt[6] = buffer_length;
    pkt[7] = buffer_length;
    pkt[8] = buffer_length;
    pkt[9] = buffer_length;
    pkt[10] = packet_counter++;
    pkt[11] = 0x12 | (1 << 7);
    pkt[12] = DS5_HAPTIC_SAMPLE_SIZE;
    memcpy(pkt + 13, haptics, DS5_HAPTIC_SAMPLE_SIZE);

    bt_write(pkt, sizeof(pkt));
}

} // namespace

void switch_hd_haptics_set_rumble(const uint8_t left[4], const uint8_t right[4]) {
    const bool was_idle = !any_voice_has_energy();
    update_voice(0, voice_from_switch_rumble(left));
    update_voice(1, voice_from_switch_rumble(right));
    last_update_us = time_us_32();
    packet_requested = was_idle && any_voice_has_energy();

    if (!voice_has_energy(voices[0]) && !voice_has_energy(voices[1])) {
        zero_packets_remaining = 3;
    }
}

void switch_hd_haptics_task() {
    const uint32_t now = time_us_32();
    if (!packet_requested &&
        last_packet_us != 0 &&
        static_cast<uint32_t>(now - last_packet_us) < DS5_HAPTIC_PACKET_INTERVAL_US) {
        return;
    }

    if (last_update_us == 0) {
        return;
    }

    if (static_cast<uint32_t>(now - last_update_us) > SWITCH_RUMBLE_TIMEOUT_US) {
        voices[0].low.target_amp = 0.0f;
        voices[0].high.target_amp = 0.0f;
        voices[1].low.target_amp = 0.0f;
        voices[1].high.target_amp = 0.0f;
        zero_packets_remaining = std::max<uint8_t>(zero_packets_remaining, 3);
    }

    if (!any_voice_has_energy() && zero_packets_remaining == 0) {
        return;
    }

    // Level 0 = off: one zero packet, then nothing (saves the controller's battery).
    const float gain = switch_settings_vibration_scale();
    if (gain <= 0.0f) {
        if (!silenced) {
            int8_t zero[DS5_HAPTIC_SAMPLE_SIZE]{};
            send_haptics_packet(zero);
            silenced = true;
        }
        last_packet_us = now;
        packet_requested = false;
        zero_packets_remaining = 0;
        return;
    }
    silenced = false;
    int8_t haptics[DS5_HAPTIC_SAMPLE_SIZE]{};
    for (uint8_t frame = 0; frame < DS5_HAPTIC_FRAMES_PER_PACKET; ++frame) {
        const float left = render_voice(voices[0]);
        const float right = render_voice(voices[1]);
        haptics[frame * 2] = float_to_s8(left + right * CROSSFEED, gain);
        haptics[frame * 2 + 1] = float_to_s8(right + left * CROSSFEED, gain);
    }

    if (!any_voice_has_energy() && zero_packets_remaining > 0) {
        --zero_packets_remaining;
    }

    send_haptics_packet(haptics);
    last_packet_us = now;
    packet_requested = false;
}

void switch_hd_haptics_test_pulse() {
    for (RumbleVoice &voice : voices) {
        voice.low.target_freq_hz = 160.0f;
        voice.low.target_amp = 0.7f;
    }
    last_update_us = time_us_32(); // SWITCH_RUMBLE_TIMEOUT_US later the task fades it out
    packet_requested = true;
}

void switch_hd_haptics_stop() {
    voices[0] = {};
    voices[1] = {};
    last_update_us = 0;
    zero_packets_remaining = 0;
    packet_requested = false;

    int8_t haptics[DS5_HAPTIC_SAMPLE_SIZE]{};
    send_haptics_packet(haptics);
    last_packet_us = time_us_32();
}

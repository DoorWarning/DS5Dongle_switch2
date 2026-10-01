//
// Created for classic Switch Pro USB emulation.
//

#include "switch_pro.h"
#include "macro.h"

#include <algorithm>
#include <cstring>

#include "bt.h"
#include "config.h"
#include "pico/critical_section.h"
#include "switch_hd_haptics.h"
#include "pico/time.h"
#include "tusb.h"

namespace {

constexpr uint32_t SWITCH_PRO_REPORT_INTERVAL_US = 15000;
constexpr uint32_t SWITCH_PRO_IMU_SAMPLE_INTERVAL_US = 5000;
constexpr uint8_t SWITCH_PRO_REPORT_ID = 0x30;
constexpr uint8_t SWITCH_PRO_REPORT_SIZE = 63;
constexpr uint8_t SWITCH_PRO_IMU_SAMPLES = 3;
constexpr uint8_t SWITCH_PRO_TIMER_STEP = SWITCH_PRO_IMU_SAMPLES;
constexpr uint8_t SWITCH_PRO_IMU_HISTORY_SIZE = 24;
constexpr uint8_t DS5_TRIGGER_THRESHOLD = 32;
constexpr uint8_t SWITCH_PRO_DEVICE_TYPE = 0x03;
constexpr uint32_t DS5_SENSOR_TIMESTAMP_TICKS_PER_US = 3;
constexpr int32_t DS5_EFFECTIVE_GYRO_RES_PER_DEG_S = 16;
constexpr int32_t SWITCH_GYRO_RES_PER_DEG_S_X1000 = 14284;
constexpr int32_t DS5_ACCEL_RES_PER_G = 8192;
constexpr int32_t SWITCH_ACCEL_RES_PER_G = 4096;
constexpr uint8_t SWITCH_PRO_CONNECTION_INFO_USB = 0x01;
constexpr uint32_t SWITCH_PRO_COLOR_FLAG_ADDR = 0x601B;
constexpr uint32_t SWITCH_PRO_COLOR_ADDR = 0x6050;

critical_section_t switch_pro_cs;
bool switch_pro_cs_ready = false;
uint32_t switch_pro_last_report_us = 0;
uint8_t switch_pro_state[SWITCH_PRO_REPORT_SIZE]{};
uint8_t switch_pro_pending_report_id = 0;
uint8_t switch_pro_pending_report[SWITCH_PRO_REPORT_SIZE]{};
bool switch_pro_pending_report_ready = false;
bool switch_pro_usb_enabled = false;
bool switch_pro_imu_enabled = false;
uint8_t switch_pro_timer = 0;
uint8_t switch_pro_mac[6] = {0x98, 0xB6, 0xE9, 0x00, 0x00, 0x01};
bool switch_pro_ds5_calibration_loaded = false;

struct Ds5Imu {
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
};

struct SwitchImu {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
};

struct Ds5Calibration {
    int16_t gyro_bias[3]{};
    int16_t gyro_denominator[3]{};
    int16_t gyro_speed_2x = DS5_EFFECTIVE_GYRO_RES_PER_DEG_S;
    int16_t accel_bias[3]{};
    int16_t accel_range_2g[3] = {DS5_ACCEL_RES_PER_G * 2, DS5_ACCEL_RES_PER_G * 2, DS5_ACCEL_RES_PER_G * 2};
};

Ds5Calibration switch_pro_ds5_calibration{};

struct TimedSwitchImu {
    SwitchImu sample{};
    uint32_t timestamp_ticks = 0;
    bool valid = false;
};

TimedSwitchImu switch_pro_imu_history[SWITCH_PRO_IMU_HISTORY_SIZE]{};
uint8_t switch_pro_imu_history_next = 0;

uint16_t u8_to_u12(uint8_t value) {
    return static_cast<uint16_t>(static_cast<uint32_t>(value) * 4095u / 255u);
}

uint16_t u8_to_u12_inverted(uint8_t value) {
    return static_cast<uint16_t>(4095u - u8_to_u12(value));
}

uint8_t switch_battery_level(uint8_t power_percent) {
    if (power_percent >= 8) {
        return 8;
    }
    if (power_percent >= 6) {
        return 6;
    }
    if (power_percent >= 3) {
        return 4;
    }
    if (power_percent >= 1) {
        return 2;
    }
    return 0;
}

bool ds5_is_charging(const USBGetStateData &ds5) {
    return ds5.Power == Charging || ds5.Power == Complete;
}

uint8_t switch_power_connection_info(const USBGetStateData &ds5) {
    uint8_t battery = ds5.Power == Complete ? 8 : switch_battery_level(ds5.PowerPercent);
    if (ds5_is_charging(ds5)) {
        battery += 1;
    }
    return static_cast<uint8_t>((battery << 4) | SWITCH_PRO_CONNECTION_INFO_USB);
}

void encode_stick(uint8_t *data, uint8_t offset, uint16_t x, uint16_t y) {
    data[offset] = x & 0xFF;
    data[offset + 1] = static_cast<uint8_t>(((y & 0x0F) << 4) | ((x >> 8) & 0x0F));
    data[offset + 2] = static_cast<uint8_t>((y >> 4) & 0xFF);
}

int16_t clamp_i16(int64_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return static_cast<int16_t>(value);
}

int32_t abs_i32(int32_t value) {
    return value < 0 ? -value : value;
}

void put_i16_le(uint8_t *dst, int16_t value) {
    dst[0] = value & 0xFF;
    dst[1] = (value >> 8) & 0xFF;
}

int16_t load_i16_le(uint8_t const *src) {
    return static_cast<int16_t>(static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8));
}

int16_t ds5_gyro_to_switch(uint8_t axis, int16_t value) {
    int64_t dps_x1000;

    if (switch_pro_ds5_calibration_loaded) {
        const int32_t denominator = switch_pro_ds5_calibration.gyro_denominator[axis];
        if (denominator == 0) {
            return 0;
        }
        dps_x1000 =
            (static_cast<int64_t>(value) - switch_pro_ds5_calibration.gyro_bias[axis]) *
            static_cast<int32_t>(switch_pro_ds5_calibration.gyro_speed_2x) *
            1000 /
            denominator;
    } else {
        dps_x1000 = static_cast<int32_t>(value) * 1000 / DS5_EFFECTIVE_GYRO_RES_PER_DEG_S;
    }

    return clamp_i16(
        dps_x1000 * SWITCH_GYRO_RES_PER_DEG_S_X1000 / (1000 * 1000)
    );
}

int16_t ds5_accel_to_switch(uint8_t axis, int16_t value) {
    if (switch_pro_ds5_calibration_loaded) {
        const int32_t range_2g = switch_pro_ds5_calibration.accel_range_2g[axis];
        if (range_2g == 0) {
            return 0;
        }
        return clamp_i16(
            (static_cast<int32_t>(value) - switch_pro_ds5_calibration.accel_bias[axis]) *
            (SWITCH_ACCEL_RES_PER_G * 2) /
            range_2g
        );
    }

    return clamp_i16(static_cast<int32_t>(value) * SWITCH_ACCEL_RES_PER_G / DS5_ACCEL_RES_PER_G);
}

Ds5Imu ds5_imu_from_report(const USBGetStateData &ds5) {
    return {
        ds5.AngularVelocityX,
        // USBGetStateData kept the old field names, but DualSense bytes
        // 15/17/19 are gyro X/Y/Z in SDL and hid-playstation.
        ds5.AngularVelocityZ,
        ds5.AngularVelocityY,
        ds5.AccelerometerX,
        ds5.AccelerometerY,
        ds5.AccelerometerZ,
    };
}

bool ds5_calibration_sane(const Ds5Calibration &calibration) {
    for (uint8_t i = 0; i < 3; ++i) {
        if (abs_i32(calibration.gyro_bias[i]) > 1024 || calibration.gyro_denominator[i] == 0) {
            return false;
        }
        const int64_t gyro_sensitivity_x1000 =
            static_cast<int64_t>(calibration.gyro_speed_2x) * 1024 * 1000 / calibration.gyro_denominator[i];
        if (gyro_sensitivity_x1000 < 32000 || gyro_sensitivity_x1000 > 96000) {
            return false;
        }

        if (abs_i32(calibration.accel_bias[i]) > 1024 || calibration.accel_range_2g[i] == 0) {
            return false;
        }
        const int64_t accel_sensitivity_x1000 =
            static_cast<int64_t>(DS5_ACCEL_RES_PER_G) * 2 * 1000 / calibration.accel_range_2g[i];
        if (accel_sensitivity_x1000 < 500 || accel_sensitivity_x1000 > 1500) {
            return false;
        }
    }

    return true;
}

bool load_ds5_calibration() {
    if (switch_pro_ds5_calibration_loaded) {
        return true;
    }

    const std::vector<uint8_t> report = get_feature_data(0x05, 41);
    if (report.size() < 35 || report[0] != 0x05) {
        return false;
    }

    Ds5Calibration calibration{};
    calibration.gyro_bias[0] = load_i16_le(report.data() + 1);
    calibration.gyro_bias[1] = load_i16_le(report.data() + 3);
    calibration.gyro_bias[2] = load_i16_le(report.data() + 5);
    calibration.gyro_denominator[0] =
        load_i16_le(report.data() + 7) - load_i16_le(report.data() + 9);
    calibration.gyro_denominator[1] =
        load_i16_le(report.data() + 11) - load_i16_le(report.data() + 13);
    calibration.gyro_denominator[2] =
        load_i16_le(report.data() + 15) - load_i16_le(report.data() + 17);
    calibration.gyro_speed_2x = load_i16_le(report.data() + 19) + load_i16_le(report.data() + 21);

    for (uint8_t i = 0; i < 3; ++i) {
        const uint8_t offset = 23 + i * 4;
        const int16_t accel_plus = load_i16_le(report.data() + offset);
        const int16_t accel_minus = load_i16_le(report.data() + offset + 2);
        const int16_t range_2g = accel_plus - accel_minus;
        calibration.accel_bias[i] = accel_plus - range_2g / 2;
        calibration.accel_range_2g[i] = range_2g;
    }

    if (!ds5_calibration_sane(calibration)) {
        return false;
    }

    switch_pro_ds5_calibration = calibration;
    switch_pro_ds5_calibration_loaded = true;
    return true;
}

uint8_t dpad_to_bits(Direction dpad) {
    switch (dpad) {
        case North:
            return 0x02;
        case NorthEast:
            return 0x02 | 0x04;
        case East:
            return 0x04;
        case SouthEast:
            return 0x01 | 0x04;
        case South:
            return 0x01;
        case SouthWest:
            return 0x01 | 0x08;
        case West:
            return 0x08;
        case NorthWest:
            return 0x02 | 0x08;
        case None:
        default:
            return 0x00;
    }
}

void reset_state() {
    memset(switch_pro_state, 0, sizeof(switch_pro_state));
    switch_pro_state[0] = 0x00;
    switch_pro_state[1] = 0x91;
    encode_stick(switch_pro_state, 5, 2048, 2048);
    encode_stick(switch_pro_state, 8, 2048, 2048);
    switch_pro_pending_report_ready = false;
    switch_pro_usb_enabled = false;
    switch_pro_imu_enabled = false;
    switch_pro_timer = 0;
    switch_pro_last_report_us = 0;
    switch_pro_ds5_calibration = {};
    switch_pro_ds5_calibration_loaded = false;
    memset(switch_pro_imu_history, 0, sizeof(switch_pro_imu_history));
    switch_pro_imu_history_next = 0;
}

void build_state_snapshot(uint8_t out[SWITCH_PRO_REPORT_SIZE]) {
    critical_section_enter_blocking(&switch_pro_cs);
    memcpy(out, switch_pro_state, SWITCH_PRO_REPORT_SIZE);
    critical_section_exit(&switch_pro_cs);
    out[0] = switch_pro_timer++;
}

void encode_imu_sample(uint8_t *imu, const SwitchImu &sample) {
    put_i16_le(imu + 0, sample.accel_x);
    put_i16_le(imu + 2, sample.accel_y);
    put_i16_le(imu + 4, sample.accel_z);
    put_i16_le(imu + 6, sample.gyro_x);
    put_i16_le(imu + 8, sample.gyro_y);
    put_i16_le(imu + 10, sample.gyro_z);
}

void reset_imu_history() {
    memset(switch_pro_imu_history, 0, sizeof(switch_pro_imu_history));
    switch_pro_imu_history_next = 0;
}

void push_imu_sample(const SwitchImu &sample, uint32_t timestamp_ticks) {
    switch_pro_imu_history[switch_pro_imu_history_next] = {sample, timestamp_ticks, true};
    switch_pro_imu_history_next = (switch_pro_imu_history_next + 1) % SWITCH_PRO_IMU_HISTORY_SIZE;
}

uint32_t tick_delta_abs(uint32_t a, uint32_t b) {
    const uint32_t forward = a - b;
    const uint32_t backward = b - a;
    return std::min(forward, backward);
}

int16_t lerp_i16(int16_t older, int16_t newer, uint32_t numerator, uint32_t denominator) {
    if (denominator == 0) {
        return older;
    }

    const int64_t delta = static_cast<int32_t>(newer) - older;
    return clamp_i16(static_cast<int64_t>(older) + delta * numerator / denominator);
}

SwitchImu lerp_imu_sample(const SwitchImu &older, const SwitchImu &newer, uint32_t numerator, uint32_t denominator) {
    return {
        lerp_i16(older.accel_x, newer.accel_x, numerator, denominator),
        lerp_i16(older.accel_y, newer.accel_y, numerator, denominator),
        lerp_i16(older.accel_z, newer.accel_z, numerator, denominator),
        lerp_i16(older.gyro_x, newer.gyro_x, numerator, denominator),
        lerp_i16(older.gyro_y, newer.gyro_y, numerator, denominator),
        lerp_i16(older.gyro_z, newer.gyro_z, numerator, denominator),
    };
}

bool latest_imu_timestamp(uint32_t *timestamp_ticks) {
    const uint8_t latest_index =
        (switch_pro_imu_history_next + SWITCH_PRO_IMU_HISTORY_SIZE - 1) % SWITCH_PRO_IMU_HISTORY_SIZE;
    if (!switch_pro_imu_history[latest_index].valid) {
        return false;
    }

    *timestamp_ticks = switch_pro_imu_history[latest_index].timestamp_ticks;
    return true;
}

bool nearest_imu_sample(uint32_t target_ticks, SwitchImu *sample) {
    bool found = false;
    uint32_t best_delta = 0;

    for (const TimedSwitchImu &candidate : switch_pro_imu_history) {
        if (!candidate.valid) {
            continue;
        }

        const uint32_t delta = tick_delta_abs(candidate.timestamp_ticks, target_ticks);
        if (!found || delta < best_delta) {
            *sample = candidate.sample;
            best_delta = delta;
            found = true;
        }
    }

    return found;
}

bool sample_at_imu_timestamp(uint32_t target_ticks, SwitchImu *sample) {
    const TimedSwitchImu *older = nullptr;
    const TimedSwitchImu *newer = nullptr;
    uint32_t older_distance = 0;
    uint32_t newer_distance = 0;

    for (const TimedSwitchImu &candidate : switch_pro_imu_history) {
        if (!candidate.valid) {
            continue;
        }

        const int32_t offset = static_cast<int32_t>(candidate.timestamp_ticks - target_ticks);
        if (offset == 0) {
            *sample = candidate.sample;
            return true;
        }

        if (offset < 0) {
            const uint32_t distance = static_cast<uint32_t>(-offset);
            if (older == nullptr || distance < older_distance) {
                older = &candidate;
                older_distance = distance;
            }
        } else {
            const uint32_t distance = static_cast<uint32_t>(offset);
            if (newer == nullptr || distance < newer_distance) {
                newer = &candidate;
                newer_distance = distance;
            }
        }
    }

    if (older != nullptr && newer != nullptr) {
        const uint32_t numerator = target_ticks - older->timestamp_ticks;
        const uint32_t denominator = newer->timestamp_ticks - older->timestamp_ticks;
        *sample = lerp_imu_sample(older->sample, newer->sample, numerator, denominator);
        return true;
    }

    return nearest_imu_sample(target_ticks, sample);
}

void encode_imu_samples(uint8_t state[SWITCH_PRO_REPORT_SIZE]) {
    uint32_t latest_ticks = 0;
    if (!latest_imu_timestamp(&latest_ticks)) {
        return;
    }

    const uint32_t sample_spacing_ticks =
        SWITCH_PRO_IMU_SAMPLE_INTERVAL_US * DS5_SENSOR_TIMESTAMP_TICKS_PER_US;

    // SDL's Switch driver gives imuState[2] the oldest timestamp and
    // imuState[0] the newest one, so slot 0 in the packet is newest.
    for (uint8_t i = 0; i < SWITCH_PRO_IMU_SAMPLES; ++i) {
        SwitchImu sample{};
        const uint32_t target_ticks = latest_ticks - sample_spacing_ticks * i;
        if (sample_at_imu_timestamp(target_ticks, &sample)) {
            encode_imu_sample(state + 12 + i * 12, sample);
        }
    }
}

void build_input_report(uint8_t out[SWITCH_PRO_REPORT_SIZE]) {
    critical_section_enter_blocking(&switch_pro_cs);
    memcpy(out, switch_pro_state, SWITCH_PRO_REPORT_SIZE);
    if (switch_pro_imu_enabled) {
        encode_imu_samples(out);
    }
    critical_section_exit(&switch_pro_cs);
    out[0] = switch_pro_timer;
    switch_pro_timer += SWITCH_PRO_TIMER_STEP;
}

void queue_input(uint8_t report_id, uint8_t const *payload, uint16_t len) {
    memset(switch_pro_pending_report, 0, sizeof(switch_pro_pending_report));
    memcpy(switch_pro_pending_report, payload, std::min<uint16_t>(len, sizeof(switch_pro_pending_report)));
    switch_pro_pending_report_id = report_id;
    switch_pro_pending_report_ready = true;
}

void queue_proprietary_ack(uint8_t command) {
    uint8_t payload[SWITCH_PRO_REPORT_SIZE]{};
    payload[0] = command;
    if (command == 0x01) {
        payload[2] = SWITCH_PRO_DEVICE_TYPE;
        for (uint8_t i = 0; i < sizeof(switch_pro_mac); ++i) {
            payload[3 + i] = switch_pro_mac[sizeof(switch_pro_mac) - 1 - i];
        }
    }
    queue_input(0x81, payload, sizeof(payload));
}

void put_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = value & 0xFF;
    dst[1] = (value >> 8) & 0xFF;
    dst[2] = (value >> 16) & 0xFF;
    dst[3] = (value >> 24) & 0xFF;
}

void put_u16_le(uint8_t *dst, uint16_t value) {
    dst[0] = value & 0xFF;
    dst[1] = (value >> 8) & 0xFF;
}

void copy_bytes(uint8_t *dst, uint8_t dst_len, const uint8_t *src, uint8_t src_len) {
    memcpy(dst, src, std::min<uint8_t>(dst_len, src_len));
}

bool copy_spi_region(uint32_t address, uint8_t *dst, uint8_t dst_len,
                     uint32_t region_address, const uint8_t *region, uint8_t region_len) {
    const uint32_t region_end = region_address + region_len;
    if (address < region_address || address >= region_end) {
        return false;
    }

    const uint32_t offset = address - region_address;
    const uint8_t copy_len = std::min<uint32_t>(dst_len, region_end - address);
    memcpy(dst, region + offset, copy_len);
    return true;
}

void encode_left_calibration(uint8_t *dst) {
    encode_stick(dst, 0, 1536, 1536); // X/Y max above center
    encode_stick(dst, 3, 2048, 2048); // X/Y center
    encode_stick(dst, 6, 1536, 1536); // X/Y min below center
}

void encode_right_calibration(uint8_t *dst) {
    encode_stick(dst, 0, 2048, 2048); // X/Y center
    encode_stick(dst, 3, 1536, 1536); // X/Y min below center
    encode_stick(dst, 6, 1536, 1536); // X/Y max above center
}

void build_spi_read(uint32_t address, uint8_t length, uint8_t *dst, uint8_t dst_len) {
    memset(dst, 0, dst_len);

    static const uint8_t color_flag[] = {0x01};
    static const uint8_t controller_color[] = {
        0xF4, 0xF4, 0xF4, // body
        0x28, 0x2D, 0x33, // buttons
        0x1F, 0x22, 0x26, // left grip
        0x1F, 0x22, 0x26, // right grip
        0xFF
    };
    if (copy_spi_region(address, dst, dst_len,
                        SWITCH_PRO_COLOR_FLAG_ADDR, color_flag, sizeof(color_flag)) ||
        copy_spi_region(address, dst, dst_len,
                        SWITCH_PRO_COLOR_ADDR, controller_color, sizeof(controller_color))) {
        return;
    }

    if (address == 0x603D) {
        encode_left_calibration(dst);
        if (dst_len >= 18) {
            encode_right_calibration(dst + 9);
        }
        if (dst_len >= 25) {
            dst[18] = 0x32;
            dst[19] = 0x32;
            dst[20] = 0x32;
            memset(dst + 21, 0xFF, 4);
        }
    } else if (address == 0x6000) {
        memset(dst, 0xFF, dst_len);
    } else if (address == 0x6080) {
        static const uint8_t factory_sensor_stick_params[] = {
            0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F, 0x0F, 0x30,
            0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41,
            0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63
        };
        copy_bytes(dst, dst_len, factory_sensor_stick_params, sizeof(factory_sensor_stick_params));
    } else if (address == 0x6098) {
        static const uint8_t factory_stick_params_2[] = {
            0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14,
            0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33,
            0x36, 0x63
        };
        copy_bytes(dst, dst_len, factory_stick_params_2, sizeof(factory_stick_params_2));
    } else if (address == 0x6020) {
        if (dst_len >= 24) {
            put_u16_le(dst + 0, 0);
            put_u16_le(dst + 2, 0);
            put_u16_le(dst + 4, 0);
            put_u16_le(dst + 6, 0x4000);
            put_u16_le(dst + 8, 0x4000);
            put_u16_le(dst + 10, 0x4000);
            put_u16_le(dst + 12, 0);
            put_u16_le(dst + 14, 0);
            put_u16_le(dst + 16, 0);
            put_u16_le(dst + 18, 0x343A);
            put_u16_le(dst + 20, 0x343A);
            put_u16_le(dst + 22, 0x343A);
        }
    } else if (address == 0x8010) {
        memset(dst, 0xFF, dst_len);
    } else if (address == 0x8026) {
        memset(dst, 0xFF, dst_len);
    } else if (address == 0x8028) {
        memset(dst, 0xFF, dst_len);
    }

    (void) length;
}

void queue_subcommand_reply(uint8_t subcommand, uint8_t const *request_data, uint16_t request_len) {
    uint8_t payload[SWITCH_PRO_REPORT_SIZE]{};
    build_state_snapshot(payload);
    payload[12] = 0x80;
    payload[13] = subcommand;

    switch (subcommand) {
        case 0x01:
            payload[12] = 0x81;
            payload[14] = SWITCH_PRO_DEVICE_TYPE;
            break;

        case 0x02:
            payload[12] = 0x82;
            payload[14] = 0x03;
            payload[15] = 0x48;
            payload[16] = SWITCH_PRO_DEVICE_TYPE;
            payload[17] = 0x02;
            memcpy(payload + 18, switch_pro_mac, sizeof(switch_pro_mac));
            payload[24] = 0x03;
            payload[25] = 0x01;
            break;

        case 0x03:
            if (request_len >= 1 && (request_data[0] == 0x30 || request_data[0] == 0x3F)) {
                switch_pro_usb_enabled = true;
            }
            break;

        case 0x04:
            payload[12] = 0x83;
            break;

        case 0x40:
            if (request_len >= 1) {
                critical_section_enter_blocking(&switch_pro_cs);
                switch_pro_imu_enabled = request_data[0] != 0;
                reset_imu_history();
                critical_section_exit(&switch_pro_cs);
            }
            break;

        case 0x10:
            payload[12] = 0x90;
            if (request_len >= 5) {
                const uint32_t address = static_cast<uint32_t>(request_data[0]) |
                                         (static_cast<uint32_t>(request_data[1]) << 8) |
                                         (static_cast<uint32_t>(request_data[2]) << 16) |
                                         (static_cast<uint32_t>(request_data[3]) << 24);
                const uint8_t length = std::min<uint8_t>(request_data[4], 29);
                put_u32_le(payload + 14, address);
                payload[18] = length;
                build_spi_read(address, length, payload + 19, length);
            }
            break;

        case 0x21:
            payload[12] = 0xA0;
            payload[14] = 0x01;
            payload[15] = 0x00;
            payload[16] = 0xFF;
            payload[17] = 0x00;
            payload[18] = 0x03;
            payload[19] = 0x00;
            payload[20] = 0x05;
            payload[21] = 0x01;
            break;

        default:
            break;
    }

    queue_input(0x21, payload, sizeof(payload));
}

void drain_pending_report() {
    if (!switch_pro_pending_report_ready || !tud_hid_ready()) {
        return;
    }
    tud_hid_report(switch_pro_pending_report_id, switch_pro_pending_report, sizeof(switch_pro_pending_report));
    switch_pro_pending_report_ready = false;
}

void send_state_if_due() {
    const uint32_t now = time_us_32();
    if (!switch_pro_usb_enabled || !tud_hid_ready()) {
        return;
    }

    if (switch_pro_last_report_us != 0 &&
        static_cast<uint32_t>(now - switch_pro_last_report_us) < SWITCH_PRO_REPORT_INTERVAL_US) {
        return;
    }

    uint8_t state[SWITCH_PRO_REPORT_SIZE]{};
    build_input_report(state);

    if (tud_hid_report(SWITCH_PRO_REPORT_ID, state, sizeof(state))) {
        switch_pro_last_report_us = now;
    }
}

SwitchImu switch_imu_from_ds5(const Ds5Imu &imu) {
    load_ds5_calibration();

    const int16_t gyro_x = ds5_gyro_to_switch(0, imu.gyro_x);
    const int16_t gyro_y = ds5_gyro_to_switch(1, imu.gyro_y);
    const int16_t gyro_z = ds5_gyro_to_switch(2, imu.gyro_z);

    const int16_t accel_x = ds5_accel_to_switch(0, imu.accel_x);
    const int16_t accel_y = ds5_accel_to_switch(1, imu.accel_y);
    const int16_t accel_z = ds5_accel_to_switch(2, imu.accel_z);

    // SDL maps Switch raw IMU to the PlayStation convention as:
    // PS_X = -Switch_Y, PS_Y = Switch_Z, PS_Z = -Switch_X.
    return {
        clamp_i16(-accel_z),
        clamp_i16(-accel_x),
        accel_y,
        clamp_i16(-gyro_z),
        clamp_i16(-gyro_x),
        gyro_y,
    };
}

} // namespace

void switch_pro_init() {
    macro_init();
    if (!switch_pro_cs_ready) {
        critical_section_init(&switch_pro_cs);
        switch_pro_cs_ready = true;
    }
    critical_section_enter_blocking(&switch_pro_cs);
    reset_state();
    critical_section_exit(&switch_pro_cs);
    switch_hd_haptics_stop();
}

void switch_pro_on_ds5_input(const USBGetStateData &ds5) {
    uint8_t state[SWITCH_PRO_REPORT_SIZE]{};
    state[0] = 0x00; // timer/counter
    state[1] = switch_power_connection_info(ds5);

    if (ds5.ButtonSquare) state[2] |= 0x01;   // West
    if (ds5.ButtonTriangle) state[2] |= 0x02; // North
    if (ds5.ButtonCross) state[2] |= 0x04;    // South
    if (ds5.ButtonCircle) state[2] |= 0x08;   // East
    if (ds5.ButtonR1) state[2] |= 0x40;
    if (ds5.ButtonR2 || ds5.TriggerRight > DS5_TRIGGER_THRESHOLD) state[2] |= 0x80;

    if (ds5.ButtonCreate) state[3] |= 0x01;  // Minus
    if (ds5.ButtonOptions) state[3] |= 0x02; // Plus
    if (ds5.ButtonR3) state[3] |= 0x04;
    if (ds5.ButtonL3) state[3] |= 0x08;
    if (ds5.ButtonHome) state[3] |= 0x10;
    if (ds5.ButtonPad) state[3] |= 0x20; // Capture

    state[4] |= dpad_to_bits(ds5.DPad);
    if (ds5.ButtonL1) state[4] |= 0x40;
    if (ds5.ButtonL2 || ds5.TriggerLeft > DS5_TRIGGER_THRESHOLD) state[4] |= 0x80;

    encode_stick(state, 5, u8_to_u12(ds5.LeftStickX), u8_to_u12_inverted(ds5.LeftStickY));
    encode_stick(state, 8, u8_to_u12(ds5.RightStickX), u8_to_u12_inverted(ds5.RightStickY));
    macro_process(state + 2, ds5.ButtonMute, ds5.ButtonCircle, ds5.ButtonCross, ds5.ButtonTriangle, ds5.ButtonSquare);

    const bool has_imu = switch_pro_imu_enabled;
    SwitchImu switch_imu{};
    if (has_imu) {
        const Ds5Imu ds5_imu = ds5_imu_from_report(ds5);
        switch_imu = switch_imu_from_ds5(ds5_imu);
    }

    critical_section_enter_blocking(&switch_pro_cs);
    if (has_imu) {
        push_imu_sample(switch_imu, ds5.SensorTimestamp);
    } else {
        reset_imu_history();
    }
    memcpy(switch_pro_state, state, sizeof(switch_pro_state));
    critical_section_exit(&switch_pro_cs);
}

void switch_pro_task() {
    if (!is_switch_pro_mode()) {
        return;
    }
    switch_hd_haptics_task();
    macro_task();
    drain_pending_report();
    send_state_if_due();
}

uint16_t switch_pro_get_report(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    if (!is_switch_pro_mode()) {
        return 0;
    }

    uint8_t payload[SWITCH_PRO_REPORT_SIZE]{};
    uint16_t len = sizeof(payload);
    switch (report_id) {
        case 0x30:
            build_input_report(payload);
            break;
        case 0x81:
            payload[0] = 0x01;
            payload[2] = SWITCH_PRO_DEVICE_TYPE;
            for (uint8_t i = 0; i < sizeof(switch_pro_mac); ++i) {
                payload[3 + i] = switch_pro_mac[sizeof(switch_pro_mac) - 1 - i];
            }
            break;
        default:
            return 0;
    }

    len = std::min<uint16_t>(len, reqlen);
    memcpy(buffer, payload, len);
    return len;
}

void switch_pro_handle_hid_out(uint8_t report_id, uint8_t const *buffer, uint16_t len) {
    if (!is_switch_pro_mode() || len == 0) {
        return;
    }

    uint8_t actual_report_id = report_id;
    uint8_t const *payload = buffer;
    uint16_t payload_len = len;
    if (actual_report_id == 0) {
        actual_report_id = buffer[0];
        payload = buffer + 1;
        payload_len = len - 1;
        if (payload_len == 0) {
            return;
        }
    }

    if (actual_report_id == 0x80) {
        switch (payload[0]) {
            case 0x01:
            case 0x02:
            case 0x03:
            case 0x06:
                queue_proprietary_ack(payload[0]);
                break;
            case 0x04:
                switch_pro_usb_enabled = true;
                break;
            case 0x05:
                switch_pro_usb_enabled = false;
                queue_proprietary_ack(payload[0]);
                break;
            default:
                break;
        }
        return;
    }

    if (actual_report_id == 0x01 && payload_len >= 10) {
        switch_hd_haptics_set_rumble(payload + 1, payload + 5);
        queue_subcommand_reply(payload[9], payload + 10, payload_len - 10);
        return;
    }

    if (actual_report_id == 0x10 && payload_len >= 9) {
        switch_hd_haptics_set_rumble(payload + 1, payload + 5);
        return;
    }

    if (actual_report_id == 0x02 && payload_len >= 8) {
        switch_hd_haptics_set_rumble(payload, payload + 4);
    }
}

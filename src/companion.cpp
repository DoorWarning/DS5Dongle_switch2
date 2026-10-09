// Companion protocol for the PC manager app (ds5_dongle_manager).
//
// One command set, two transports:
//  - DS5/DSE mode: vendor Feature report 0xFA (63 bytes). SET_REPORT carries a
//    request, GET_REPORT returns the reply to the last request. 0xF6-0xF9 (the
//    original web config protocol) are left alone.
//  - Switch Pro mode: output report 0x01 with subcommand 0xE0 (outside the range
//    the Switch uses). The request starts at the subcommand data; the reply rides
//    in the 0x21 subcommand reply (switch_pro.cpp). The HID descriptor the Switch
//    sees is unchanged.
//
// Request: [cmd, seq, len, payload...]   Reply: [cmd, seq, status, len, data...]
// The host sends one request at a time and matches the reply by cmd + seq.

#include "companion.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "bt.h"
#include "config.h"
#include "macro.h"
#include "mode_toggle.h"
#include "switch_settings.h"
#include "switch_wake.h"

extern uint8_t interrupt_in_data[63]; // main.cpp: last DualSense input report
bool ds_mode();                       // usb_descriptors.cpp

namespace {

constexpr uint8_t PROTOCOL_VERSION = 2; // 2: NS settings, macro editing, wake learn
constexpr uint32_t MODE_SWITCH_DELAY_MS = 300; // let the host read the reply first

enum Command : uint8_t {
    CMD_GET_INFO = 0x01,
    CMD_SET_MODE = 0x02,
    CMD_WAKE_LEARN = 0x03,  // payload [1 start / 0 cancel]; the Pico LED blinks slowly while learning
    CMD_WAKE_FORGET = 0x04, // erase the learned wake beacon
    // Switch Pro mode only (STATUS_WRONG_MODE otherwise). The app reads and
    // writes NsSettings in CHUNK-byte pieces, then applies it in one go.
    CMD_NS_SETTINGS_READ = 0x10,  // [offset] -> [total size, bytes...]
    CMD_NS_SETTINGS_WRITE = 0x11, // [offset, bytes...] into a staging copy
    CMD_NS_SETTINGS_APPLY = 0x12, // validate, apply and save the staging copy
    CMD_MACRO_INFO = 0x20,        // -> [status, active slot, max events u16, count u16 x MACRO_SLOTS]
    CMD_MACRO_READ = 0x21,        // [slot, offset u16] -> event bytes
    CMD_MACRO_EDIT_BEGIN = 0x22,  // [slot]
    CMD_MACRO_EDIT_WRITE = 0x23,  // [offset u16, event bytes...]
    CMD_MACRO_EDIT_COMMIT = 0x24, // [count u16]; 0 clears the slot
};

enum Status : uint8_t {
    STATUS_OK = 0,
    STATUS_UNKNOWN_COMMAND = 1,
    STATUS_BAD_ARGS = 2,
    STATUS_WRONG_MODE = 3, // needs Switch Pro mode
    STATUS_BUSY = 4,       // a macro is recording or still being saved
};

constexpr uint8_t CHUNK = 32; // fits both transports (Switch reply data is 45 bytes)

enum InfoFlags : uint8_t {
    INFO_DS5_CONNECTED = 0x01,
    INFO_WAKE_BEACON = 0x02,
    INFO_SERIAL_BUILD = 0x04,
    INFO_WAKE_LEARNING = 0x08,
};

constexpr uint8_t REPLY_HEADER = 4;
constexpr uint8_t FEATURE_SIZE = 63;
uint8_t feature_reply[FEATURE_SIZE]{};
uint8_t feature_reply_len = 0;

// Staging copy for CMD_NS_SETTINGS_WRITE. Heap, Switch Pro mode only.
NsSettings *ns_staging = nullptr;

uint16_t u16_at(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void put_u16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

uint8_t active_mode() {
    if (is_switch_pro_mode()) {
        return ControllerMode_SwitchPro;
    }
    return ds_mode() ? ControllerMode_DS5 : ControllerMode_DSE;
}

// Returns the data length written to `out`.
uint16_t get_info(uint8_t *out, uint16_t max) {
    if (max < 5) {
        return 0;
    }
    uint8_t flags = 0;
    if (bt_is_connected()) flags |= INFO_DS5_CONNECTED;
    if (switch_wake_beacon_learned()) flags |= INFO_WAKE_BEACON;
    if (switch_wake_learning()) flags |= INFO_WAKE_LEARNING;
#if ENABLE_SERIAL
    flags |= INFO_SERIAL_BUILD;
#endif
    out[0] = PROTOCOL_VERSION;
    out[1] = get_config().controller_mode; // stored setting (2 = Auto)
    out[2] = active_mode();               // what the dongle is right now
    out[3] = flags;
    out[4] = bt_is_connected() ? interrupt_in_data[52] : 0xFF; // battery: low nibble level, high nibble state
    const uint16_t ver_len = std::min<uint16_t>(strlen(PICO_PROGRAM_VERSION_STRING), max - 5);
    memcpy(out + 5, PICO_PROGRAM_VERSION_STRING, ver_len);
    return 5 + ver_len;
}

} // namespace

uint16_t companion_handle(const uint8_t *request, uint16_t request_len, uint8_t *reply, uint16_t reply_max) {
    if (request_len < 3 || reply_max < REPLY_HEADER) {
        return 0;
    }
    const uint8_t cmd = request[0];
    const uint8_t seq = request[1];
    const uint8_t len = std::min<uint16_t>(request[2], request_len - 3);
    const uint8_t *payload = request + 3;
    uint8_t *data = reply + REPLY_HEADER;
    const uint16_t data_max = reply_max - REPLY_HEADER;

    uint8_t status = STATUS_OK;
    uint16_t data_len = 0;
    switch (cmd) {
        case CMD_GET_INFO:
            data_len = get_info(data, data_max);
            break;
        case CMD_SET_MODE:
            if (len < 1 || payload[0] > ControllerMode_SwitchPro) {
                status = STATUS_BAD_ARGS;
            } else {
                mode_request(payload[0], MODE_SWITCH_DELAY_MS);
            }
            break;
        case CMD_WAKE_LEARN:
            if (len < 1 || payload[0] > 1) {
                status = STATUS_BAD_ARGS;
            } else {
                switch_wake_set_learning(payload[0] == 1);
            }
            break;
        case CMD_WAKE_FORGET:
            switch_wake_forget();
            break;
        case CMD_NS_SETTINGS_READ: {
            const NsSettings *current = switch_settings_get();
            if (current == nullptr) {
                status = STATUS_WRONG_MODE;
            } else if (len < 1 || payload[0] >= sizeof(NsSettings) || data_max < CHUNK + 1) {
                status = STATUS_BAD_ARGS;
            } else {
                const uint8_t offset = payload[0];
                const uint8_t n = std::min<uint8_t>(CHUNK, sizeof(NsSettings) - offset);
                data[0] = sizeof(NsSettings);
                memcpy(data + 1, reinterpret_cast<const uint8_t *>(current) + offset, n);
                data_len = 1 + n;
            }
            break;
        }
        case CMD_NS_SETTINGS_WRITE: {
            const NsSettings *current = switch_settings_get();
            if (current == nullptr) {
                status = STATUS_WRONG_MODE;
                break;
            }
            if (ns_staging == nullptr) {
                ns_staging = static_cast<NsSettings *>(malloc(sizeof(NsSettings)));
                if (ns_staging == nullptr) {
                    status = STATUS_BUSY;
                    break;
                }
                *ns_staging = *current;
            }
            if (len < 1 || payload[0] + (len - 1) > sizeof(NsSettings)) {
                status = STATUS_BAD_ARGS;
            } else {
                memcpy(reinterpret_cast<uint8_t *>(ns_staging) + payload[0], payload + 1, len - 1);
            }
            break;
        }
        case CMD_NS_SETTINGS_APPLY:
            if (switch_settings_get() == nullptr) {
                status = STATUS_WRONG_MODE;
            } else if (ns_staging == nullptr || !switch_settings_set(*ns_staging)) {
                status = STATUS_BAD_ARGS;
            }
            if (ns_staging != nullptr) {
                // Next edit starts from what is live again.
                *ns_staging = *switch_settings_get();
            }
            break;
        case CMD_MACRO_INFO: {
            int8_t slot = -1;
            const MacroStatus st = macro_status(slot);
            if (st == MacroStatus::Unavailable) {
                status = STATUS_WRONG_MODE;
                break;
            }
            data[0] = static_cast<uint8_t>(st);
            data[1] = static_cast<uint8_t>(slot);
            put_u16(data + 2, macro_max_events());
            for (int i = 0; i < MACRO_SLOTS; i++) {
                put_u16(data + 4 + i * 2, macro_event_count(i));
            }
            data_len = 4 + MACRO_SLOTS * 2;
            break;
        }
        case CMD_MACRO_READ: {
            int8_t unused = -1;
            if (macro_status(unused) == MacroStatus::Unavailable) {
                status = STATUS_WRONG_MODE;
            } else if (len < 3 || payload[0] >= MACRO_SLOTS) {
                status = STATUS_BAD_ARGS;
            } else {
                data_len = macro_read(payload[0], u16_at(payload + 1), data, std::min<uint16_t>(CHUNK, data_max));
            }
            break;
        }
        case CMD_MACRO_EDIT_BEGIN: {
            int8_t unused = -1;
            if (macro_status(unused) == MacroStatus::Unavailable) {
                status = STATUS_WRONG_MODE;
            } else if (len < 1 || payload[0] >= MACRO_SLOTS) {
                status = STATUS_BAD_ARGS;
            } else if (!macro_edit_begin(payload[0])) {
                status = STATUS_BUSY;
            }
            break;
        }
        case CMD_MACRO_EDIT_WRITE:
            if (len < 2 || !macro_edit_write(u16_at(payload), payload + 2, len - 2)) {
                status = STATUS_BAD_ARGS;
            }
            break;
        case CMD_MACRO_EDIT_COMMIT:
            if (len < 2 || !macro_edit_commit(u16_at(payload))) {
                status = STATUS_BAD_ARGS;
            }
            break;
        default:
            status = STATUS_UNKNOWN_COMMAND;
            break;
    }

    reply[0] = cmd;
    reply[1] = seq;
    reply[2] = status;
    reply[3] = static_cast<uint8_t>(data_len);
    return REPLY_HEADER + data_len;
}

void companion_feature_set(const uint8_t *buffer, uint16_t len) {
    feature_reply_len = static_cast<uint8_t>(companion_handle(buffer, len, feature_reply, sizeof(feature_reply)));
}

uint16_t companion_feature_get(uint8_t *buffer, uint16_t reqlen) {
    // Always the full report size; unused bytes are zero.
    const uint16_t len = std::min<uint16_t>(FEATURE_SIZE, reqlen);
    memset(buffer, 0, len);
    memcpy(buffer, feature_reply, std::min<uint16_t>(feature_reply_len, len));
    return len;
}

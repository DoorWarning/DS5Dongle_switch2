// BLE experiment for the Switch 2 wake beacon.
//
// BTstack is built without ENABLE_BLE (the extra RAM would starve core1's opus
// allocations), so the LE commands are defined here and sent raw, and the LE
// Meta event is enabled in the event mask by hand.
//
// Scan: logs every advertisement that carries Nintendo manufacturer data
// (company ID 0x0553) or the Nintendo USB VID (7E 05), plus anything very close
// to the dongle (RSSI >= CLOSE_RSSI). The last Nintendo beacon with the wake
// flag (0x81) is kept in RAM.
//
// Replay (type a key in the serial terminal):
//   'w' - send the kept beacon for REPLAY_MS from the Joy-Con's address
//         (public BD_ADDR temporarily changed with the Broadcom vendor command),
//         non-connectable, 30-50 ms interval: the settings of the ESP32 wake
//         firmware that is confirmed to work (alexvnesta/switch2controller)
//   'c' - like 'w' but connectable ADV_IND at 20 ms (what the Joy-Con sends)
//   'n' - like 'w' but from the dongle's own address (control test)
//   'l' - like 'w' but for LONG_REPLAY_MS, to check the sender address with a
//         phone BLE scanner (e.g. nRF Connect on Android)
// After each address change the controller's BD_ADDR is read back and logged.
// LE events other than advertising reports (e.g. the console connecting) are
// logged raw.

#include "ble_scan.h"

#include <cstdio>
#include <cstring>

#include "btstack.h"
#include "switch_wake.h"
#include "pico/stdio.h"
#include "pico/time.h"

namespace {

constexpr uint16_t NINTENDO_COMPANY_ID = 0x0553;
constexpr int8_t CLOSE_RSSI = -45;
constexpr uint32_t REPEAT_LOG_MS = 1000;
constexpr uint32_t HEARTBEAT_MS = 10000;
constexpr uint32_t REPLAY_MS = 3000;
constexpr uint32_t LONG_REPLAY_MS = 20000;
constexpr uint8_t ADV_LEN = 31;
// Offset of the state flag in the Joy-Con 2 beacon (0x00 idle, 0x81 wake):
// 02 01 06 | 1B FF 53 05 01 00 03 7E 05 66 20 00 01 [81] [console addr, reversed] 0F 00 ..
constexpr uint8_t WAKE_FLAG_OFFSET = 16;
constexpr uint8_t WAKE_FLAG = 0x81;

const hci_cmd_t le_set_scan_parameters = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x0B), "12211"};
const hci_cmd_t le_set_scan_enable = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x0C), "11"};
const hci_cmd_t le_set_adv_parameters = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x06), "22111B11"};
// Length + 31 data bytes. The 'A' (31-byte advertising data) format char only
// exists with ENABLE_BLE; without it the data was silently dropped and the
// controller advertised an empty payload. 3 x 'D' (8 bytes) + 7 x '1' = 31.
const hci_cmd_t le_set_adv_data = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x08), "1DDD1111111"};
const hci_cmd_t le_set_adv_enable = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x0A), "1"};
// Broadcom Write BD_ADDR, same command btstack_chipset_bcm uses at init.
const hci_cmd_t bcm_write_bd_addr = {0xFC01, "B"};

enum class Step : uint8_t {
    Off,
    SetEventMask,
    SetScanParameters,
    EnableScan,
    Running,
    ReplayScanOff,
    ReplaySetAddr,
    ReplayCheckAddr,
    ReplayAdvParameters,
    ReplayAdvData,
    ReplayAdvOn,
    ReplayWait,
    ReplayAdvOff,
    ReplayRestoreAddr,
    ReplayCheckRestored,
    Failed,
};

Step step = Step::Off;
uint16_t waiting_opcode = 0;

uint32_t adv_total = 0;
uint32_t adv_logged = 0;
uint32_t last_heartbeat_ms = 0;

// Only the last logged advertisement is remembered, to drop repeats.
uint8_t last_addr[6]{};
uint32_t last_hash = 0;
uint32_t last_log_ms = 0;

bool beacon_valid = false;
bool store_requested = false; // save the kept beacon for switch_wake.cpp
bd_addr_t beacon_addr{};
uint8_t beacon_data[ADV_LEN]{};

bool replay_spoof = false;
bool replay_connectable = false;
bd_addr_t own_addr{};
uint32_t replay_end_ms = 0;
uint32_t replay_len_ms = REPLAY_MS;

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

uint32_t fnv1a(const uint8_t *data, uint8_t len) {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < len; i++) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h;
}

const char *addr_type_str(uint8_t addr_type, const bd_addr_t addr) {
    switch (addr_type) {
        case 0: return "public";
        case 2: return "public-id";
        case 1:
        case 3:
            switch (addr[0] >> 6) {
                case 3: return "random-static";
                case 1: return "random-resolvable";
                case 0: return "random-nonresolvable";
                default: return "random-reserved";
            }
        default: return "unknown";
    }
}

bool is_nintendo(const uint8_t *data, uint8_t len) {
    for (uint8_t i = 0; i + 1 < len;) {
        const uint8_t ad_len = data[i];
        if (ad_len == 0 || i + 1 + ad_len > len) {
            break;
        }
        const uint8_t ad_type = data[i + 1];
        if (ad_type == 0xFF && ad_len >= 3 &&
            little_endian_read_16(data, i + 2) == NINTENDO_COMPANY_ID) {
            return true;
        }
        i += 1 + ad_len;
    }
    for (uint8_t i = 0; i + 1 < len; i++) {
        if (data[i] == 0x7E && data[i + 1] == 0x05) {
            return true;
        }
    }
    return false;
}

void print_hex(const uint8_t *data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        printf("%02X", data[i]);
    }
}

void handle_adv_report(const uint8_t *packet, uint16_t size) {
    // [0]=0x3E [1]=len [2]=subevent [3]=num_reports, then one report:
    // event_type(1) addr_type(1) addr(6, LE) data_len(1) data(n) rssi(1)
    if (size < 13) {
        return;
    }
    const uint8_t num_reports = packet[3];
    const uint8_t event_type = packet[4];
    const uint8_t addr_type = packet[5];
    const uint8_t data_len = packet[12];
    if (13u + data_len + 1u > size) {
        return;
    }
    const uint8_t *data = packet + 13;
    const auto rssi = static_cast<int8_t>(packet[13 + data_len]);
    adv_total++;

    const bool nintendo = is_nintendo(data, data_len);
    if (!nintendo && rssi < CLOSE_RSSI) {
        return;
    }

    bd_addr_t addr;
    reverse_bd_addr(packet + 6, addr);

    if (nintendo && data_len == ADV_LEN && data[WAKE_FLAG_OFFSET] == WAKE_FLAG && addr_type == 0) {
        if (!beacon_valid || bd_addr_cmp(addr, beacon_addr) != 0 || memcmp(data, beacon_data, ADV_LEN) != 0) {
            printf("[BLE] wake beacon kept for replay\n");
            store_requested = true; // flash write happens in ble_scan_task(), not in this callback
        }
        bd_addr_copy(beacon_addr, addr);
        memcpy(beacon_data, data, ADV_LEN);
        beacon_valid = true;
    }

    const uint32_t hash = fnv1a(data, data_len) ^ addr_type;
    const uint32_t now = now_ms();
    if (hash == last_hash && bd_addr_cmp(addr, last_addr) == 0 && now - last_log_ms < REPEAT_LOG_MS) {
        return;
    }
    last_hash = hash;
    bd_addr_copy(last_addr, addr);
    last_log_ms = now;
    adv_logged++;

    printf("[BLE] %s adv_type=%u addr=%s addr_type=%u(%s) rssi=%d len=%u reports=%u\n",
           nintendo ? "NINTENDO" : "close", event_type, bd_addr_to_str(addr), addr_type,
           addr_type_str(addr_type, addr), rssi, data_len, num_reports);
    printf("[BLE]   data=");
    print_hex(data, data_len);
    printf("\n");
}

void expect(uint16_t opcode, Step next) {
    waiting_opcode = opcode;
    step = next;
}

void start_replay(bool spoof, bool connectable, uint32_t len_ms) {
    if (!beacon_valid) {
        printf("[BLE] no wake beacon yet: sleep the console and press HOME on the Joy-Con 2 first\n");
        return;
    }
    replay_spoof = spoof;
    replay_connectable = connectable;
    replay_len_ms = len_ms;
    gap_local_bd_addr(own_addr);
    // bd_addr_to_str() returns a shared buffer: one call per printf.
    printf("[BLE] replay %lu ms %s as %s, ", static_cast<unsigned long>(len_ms),
           connectable ? "ADV_IND 20ms" : "NONCONN 30-50ms", spoof ? "Joy-Con" : "dongle");
    printf("sender %s, ", bd_addr_to_str(spoof ? beacon_addr : own_addr));
    printf("dongle %s\n", bd_addr_to_str(own_addr));
    printf("[BLE]   data=");
    print_hex(beacon_data, ADV_LEN);
    printf("\n");
    step = Step::ReplayScanOff;
}

void poll_serial_key() {
    const int c = getchar_timeout_us(0);
    if (c == 'w' || c == 'W') {
        start_replay(true, false, REPLAY_MS);
    } else if (c == 'c' || c == 'C') {
        start_replay(true, true, REPLAY_MS);
    } else if (c == 'n' || c == 'N') {
        start_replay(false, false, REPLAY_MS);
    } else if (c == 'l' || c == 'L') {
        start_replay(true, false, LONG_REPLAY_MS);
    }
}

} // namespace

void ble_scan_on_hci_event(uint8_t packet_type, const uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET || size < 2) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                step = Step::SetEventMask;
                waiting_opcode = 0;
                printf("[BLE] scan experiment: starting\n");
            } else {
                step = Step::Off;
            }
            break;

        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            if (waiting_opcode == 0 || opcode != waiting_opcode) {
                break;
            }
            const uint8_t *ret = hci_event_command_complete_get_return_parameters(packet);
            const uint8_t status = ret[0];
            printf("[BLE] opcode 0x%04X status=0x%02X\n", opcode, status);
            if (opcode == hci_read_bd_addr.opcode && status == ERROR_CODE_SUCCESS) {
                bd_addr_t now_addr;
                reverse_bd_addr(ret + 1, now_addr);
                printf("[BLE] controller BD_ADDR now %s\n", bd_addr_to_str(now_addr));
            }
            waiting_opcode = 0;
            if (status != ERROR_CODE_SUCCESS) {
                printf("[BLE] experiment: FAILED\n");
                if (step >= Step::ReplayScanOff && step <= Step::ReplayRestoreAddr && replay_spoof) {
                    // Always try to put the real address back.
                    step = Step::ReplayRestoreAddr;
                } else {
                    step = Step::Failed;
                }
            } else if (step == Step::Running) {
                printf("[BLE] scanning (passive). Keys: w = Joy-Con NONCONN, c = Joy-Con ADV_IND, "
                       "n = dongle NONCONN, l = long w\n");
                last_heartbeat_ms = now_ms();
            } else if (step == Step::ReplayWait) {
                replay_end_ms = now_ms() + replay_len_ms;
                printf("[BLE] advertising...\n");
            }
            break;
        }

        case HCI_EVENT_LE_META:
            if (size >= 3 && packet[2] == HCI_SUBEVENT_LE_ADVERTISING_REPORT) {
                handle_adv_report(packet, size);
            } else if (size >= 3) {
                // e.g. 0x01 LE Connection Complete: the console connecting back
                printf("[BLE] LE event subevent=0x%02X raw=", packet[2]);
                print_hex(packet, size > 40 ? 40 : static_cast<uint8_t>(size));
                printf("\n");
            }
            break;

        default:
            break;
    }
}

void ble_scan_task() {
    if (store_requested) {
        store_requested = false;
        const bool ok = switch_wake_store_beacon(beacon_addr, beacon_data);
        printf("[BLE] wake beacon %s for Switch mode\n", ok ? "SAVED to flash" : "save FAILED");
    }
    if (step == Step::Running && waiting_opcode == 0) {
        poll_serial_key();
        const uint32_t now = now_ms();
        if (step == Step::Running && now - last_heartbeat_ms >= HEARTBEAT_MS) {
            last_heartbeat_ms = now;
            printf("[BLE] heartbeat: adv_total=%lu logged=%lu beacon=%d\n",
                   static_cast<unsigned long>(adv_total), static_cast<unsigned long>(adv_logged), beacon_valid);
        }
        if (step == Step::Running) {
            return;
        }
    }
    if (waiting_opcode != 0 || !hci_can_send_command_packet_now()) {
        return;
    }
    switch (step) {
        case Step::SetEventMask:
            // BTstack leaves the LE Meta bit (61) off when built without ENABLE_BLE.
            hci_send_cmd(&hci_set_event_mask, 0xFFFFFFFFU, 0x3FFFFFFFU);
            expect(hci_set_event_mask.opcode, Step::SetScanParameters);
            break;
        case Step::SetScanParameters:
            // passive, interval 60 ms, window 30 ms (leave air time for the DualSense link),
            // own address public, accept all
            hci_send_cmd(&le_set_scan_parameters, 0, 0x0060, 0x0030, 0, 0);
            expect(le_set_scan_parameters.opcode, Step::EnableScan);
            break;
        case Step::EnableScan:
            // enable, no duplicate filtering (we want every beacon burst)
            hci_send_cmd(&le_set_scan_enable, 1, 0);
            expect(le_set_scan_enable.opcode, Step::Running);
            break;

        case Step::ReplayScanOff:
            hci_send_cmd(&le_set_scan_enable, 0, 0);
            expect(le_set_scan_enable.opcode, replay_spoof ? Step::ReplaySetAddr : Step::ReplayAdvParameters);
            break;
        case Step::ReplaySetAddr:
            hci_send_cmd(&bcm_write_bd_addr, beacon_addr);
            expect(bcm_write_bd_addr.opcode, Step::ReplayCheckAddr);
            break;
        case Step::ReplayCheckAddr:
            hci_send_cmd(&hci_read_bd_addr);
            expect(hci_read_bd_addr.opcode, Step::ReplayAdvParameters);
            break;
        case Step::ReplayAdvParameters: {
            // public address, all channels. Connectable: ADV_IND (0) at 20 ms like the
            // Joy-Con. Otherwise ADV_NONCONN_IND (3) at 30-50 ms like the ESP32 firmware.
            bd_addr_t no_peer{};
            if (replay_connectable) {
                hci_send_cmd(&le_set_adv_parameters, 0x0020, 0x0020, 0, 0, 0, no_peer, 0x07, 0);
            } else {
                hci_send_cmd(&le_set_adv_parameters, 0x0030, 0x0050, 3, 0, 0, no_peer, 0x07, 0);
            }
            expect(le_set_adv_parameters.opcode, Step::ReplayAdvData);
            break;
        }
        case Step::ReplayAdvData:
            static_assert(ADV_LEN == 3 * 8 + 7);
            hci_send_cmd(&le_set_adv_data, ADV_LEN, beacon_data, beacon_data + 8, beacon_data + 16,
                         beacon_data[24], beacon_data[25], beacon_data[26], beacon_data[27],
                         beacon_data[28], beacon_data[29], beacon_data[30]);
            expect(le_set_adv_data.opcode, Step::ReplayAdvOn);
            break;
        case Step::ReplayAdvOn:
            hci_send_cmd(&le_set_adv_enable, 1);
            expect(le_set_adv_enable.opcode, Step::ReplayWait);
            break;
        case Step::ReplayWait:
            if (static_cast<int32_t>(now_ms() - replay_end_ms) >= 0) {
                step = Step::ReplayAdvOff;
            }
            break;
        case Step::ReplayAdvOff:
            hci_send_cmd(&le_set_adv_enable, 0);
            expect(le_set_adv_enable.opcode, replay_spoof ? Step::ReplayRestoreAddr : Step::EnableScan);
            break;
        case Step::ReplayRestoreAddr:
            hci_send_cmd(&bcm_write_bd_addr, own_addr);
            expect(bcm_write_bd_addr.opcode, Step::ReplayCheckRestored);
            printf("[BLE] restoring dongle address %s\n", bd_addr_to_str(own_addr));
            break;
        case Step::ReplayCheckRestored:
            hci_send_cmd(&hci_read_bd_addr);
            expect(hci_read_bd_addr.opcode, Step::EnableScan);
            break;
        default:
            break;
    }
}

// Switch 2 wake from sleep.
//
// A wired Pro Controller cannot wake a Switch 2, but the console wakes on the
// BLE beacon a Joy-Con 2 sends when HOME is pressed. The console only accepts it
// from the controller's own (public) address, so the dongle:
//  - learns (any mode, learn mode started with Create + Options + Triangle held
//    LEARN_HOLD_US): scans for that beacon for LEARN_MS and stores the sender
//    address + payload in flash. Do it with the dongle on a PC or charger: the
//    dock limits USB power while the console sleeps.
//  - wakes (Switch Pro mode): when the DualSense reconnects while the console
//    sleeps (PS press), it changes its BD_ADDR to the stored address (Broadcom
//    vendor command), advertises the beacon for WAKE_ADV_MS (connectable, 20 ms,
//    like the Joy-Con) and restores its address. The DualSense link survives.
//
// How the Switch 2 dock behaves (seen on 2026-10-02): going to sleep it detaches
// the dongle and cycles USB power, then never enumerates it; waking up it cycles
// power again. So "asleep" = not enumerated for UNMOUNTED_ASLEEP_MS (an awake
// console enumerates a Pro Controller within a second or two), or USB suspended
// for ASLEEP_AFTER_MS on hosts that do suspend. "Awake" = the console is reading
// input reports again.
//
// BTstack is built without ENABLE_BLE (its RAM would starve core1's opus
// allocations), so the LE commands are defined here and sent raw. Only format
// chars that exist without ENABLE_BLE may be used: 'A' (31-byte advertising
// data) does not, so the advertising data goes out as "1DDD1111111".
//
// Pico LED, unless disabled in the config:
//  - slow blinking: learn mode, waiting for a Joy-Con 2 beacon
//  - 5 fast blinks: beacon learned and saved
//  - fast blinking for WAKE_ADV_MS: sending the wake beacon
//  - 2 fast blinks on a reconnect while the console sleeps: no beacon learned yet

#include "switch_wake.h"

#include <cstdio>
#include <cstring>

#include "btstack.h"
#include "bt.h"
#include "config.h"
#include "usb.h" // usb_mounted(); tusb.h itself clashes with btstack.h's HID names
#include "utils.h"
#include "wake.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/btstack_flash_bank.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/time.h"

namespace {

constexpr uint8_t ADV_LEN = 31;
constexpr uint16_t NINTENDO_COMPANY_ID = 0x0553;
// 02 01 06 | 1B FF 53 05 01 00 03 7E 05 [PID] 00 01 [81] [console addr, reversed] 0F 00..
constexpr uint8_t WAKE_FLAG_OFFSET = 16;
constexpr uint8_t WAKE_FLAG = 0x81;
constexpr uint32_t WAKE_ADV_MS = 3000;

constexpr uint32_t ASLEEP_AFTER_MS = 3000;
constexpr uint32_t UNMOUNTED_ASLEEP_MS = 5000;
constexpr uint32_t AWAKE_WINDOW_MS = 1000;
constexpr uint16_t AWAKE_REPORTS = 10;

constexpr int64_t LEARN_HOLD_US = 3000000;
constexpr uint32_t LEARN_MS = 60000;

constexpr uint32_t RECORD_MAGIC = 0x57414B45; // "WAKE"
// Below the macro slots, which sit below the config sector.
constexpr uint32_t WAKE_FLASH_OFFSET = PICO_FLASH_BANK_STORAGE_OFFSET - 6 * FLASH_SECTOR_SIZE;
static_assert(WAKE_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

struct __attribute__((packed)) WakeRecord {
    uint32_t magic;
    uint8_t addr[6]; // big-endian, as printed
    uint8_t data[ADV_LEN];
    uint8_t reserved;
    uint32_t crc;
};
static_assert(sizeof(WakeRecord) <= FLASH_PAGE_SIZE);

const hci_cmd_t le_set_scan_parameters = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x0B), "12211"};
const hci_cmd_t le_set_scan_enable = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x0C), "11"};
const hci_cmd_t le_set_adv_parameters = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x06), "22111B11"};
const hci_cmd_t le_set_adv_data = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x08), "1DDD1111111"};
const hci_cmd_t le_set_adv_enable = {HCI_OPCODE(OGF_LE_CONTROLLER, 0x0A), "1"};
const hci_cmd_t bcm_write_bd_addr = {0xFC01, "B"}; // same command btstack_chipset_bcm uses at init

enum class Step : uint8_t {
    Off,           // BLE not set up (yet)
    SetEventMask,
    SetScanParameters,
    Ready,
    ScanOn,
    ScanOff,
    WakeScanOff,
    WakeSetAddr,
    WakeAdvParameters,
    WakeAdvData,
    WakeAdvOn,
    WakeAdvWait,
    WakeAdvOff,
    WakeRestoreAddr,
};

bool hci_working = false;
Step step = Step::Off;
uint16_t waiting_opcode = 0;
uint32_t retry_after_ms = 0; // back-off after a failed command
bool scanning = false;

bool console_asleep = false;
uint32_t suspended_since_ms = 0; // 0 = not suspended
uint32_t unmounted_since_ms = 0; // 0 = enumerated
uint16_t reports_sent = 0;       // input reports the console read in this window (tud_task context)
uint32_t report_window_ms = 0;

bool wake_requested = false;
uint32_t wake_end_ms = 0;
bd_addr_t own_addr{};

absolute_time_t learn_combo_since = nil_time;
bool learn_combo_fired = false;
bool learn_requested = false;
bool learning = false;
uint32_t learn_until_ms = 0;
bool save_pending = false;
WakeRecord pending{};

uint8_t led_blinks_left = 0;
uint32_t led_last_ms = 0;
bool led_on = false;
bool led_owned = false; // this module is driving the LED

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

// PC mode only runs this module for learn mode, until the scan is off again.
bool active() {
    return is_switch_pro_mode() || learning || scanning || waiting_opcode != 0;
}

const WakeRecord &stored_record() {
    return *reinterpret_cast<const WakeRecord *>(XIP_BASE + WAKE_FLASH_OFFSET);
}

uint32_t record_crc(const WakeRecord &r) {
    return crc32(r.addr, sizeof(r.addr) + sizeof(r.data));
}

bool record_valid(const WakeRecord &r) {
    return r.magic == RECORD_MAGIC && r.crc == record_crc(r);
}

WakeRecord make_record(const uint8_t addr[6], const uint8_t data[ADV_LEN]) {
    WakeRecord r{};
    r.magic = RECORD_MAGIC;
    memcpy(r.addr, addr, sizeof(r.addr));
    memcpy(r.data, data, ADV_LEN);
    r.data[WAKE_FLAG_OFFSET] = WAKE_FLAG;
    r.crc = record_crc(r);
    return r;
}

// A Joy-Con 2 advertisement naming its console: flag 0x81 is the wake beacon,
// 0x00 the same frame while not waking. Both carry the console address.
bool is_joycon_beacon(uint8_t addr_type, const uint8_t *data, uint8_t len) {
    return addr_type == 0 && len == ADV_LEN &&
           data[3] == 0x1B && data[4] == 0xFF &&
           little_endian_read_16(data, 5) == NINTENDO_COMPANY_ID &&
           data[10] == 0x7E && data[11] == 0x05 &&
           (data[WAKE_FLAG_OFFSET] == WAKE_FLAG || data[WAKE_FLAG_OFFSET] == 0x00);
}

void handle_adv_report(const uint8_t *packet, uint16_t size) {
    // [3]=num_reports, then: event_type(1) addr_type(1) addr(6, LE) data_len(1) data(n) rssi(1)
    if (size < 13 || !learning || save_pending) {
        return;
    }
    const uint8_t addr_type = packet[5];
    const uint8_t data_len = packet[12];
    if (13u + data_len > size || !is_joycon_beacon(addr_type, packet + 13, data_len)) {
        return;
    }
    bd_addr_t addr;
    reverse_bd_addr(packet + 6, addr);
    pending = make_record(addr, packet + 13);
    save_pending = true;
    printf("[Wake] learned beacon from %s\n", bd_addr_to_str(addr));
}

void save_flash_op(void *param) {
    const auto *page = static_cast<const uint8_t *>(param);
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(WAKE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WAKE_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(interrupts);
}

// Main loop only (flash_safe_execute).
bool save_record(const WakeRecord &r) {
    if (record_valid(stored_record()) && memcmp(&stored_record(), &r, sizeof(r)) == 0) {
        return true;
    }
    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &r, sizeof(r));
    watchdog_update();
    const int rc = flash_safe_execute(save_flash_op, page, 1000);
    watchdog_update();
    const bool ok = rc == PICO_OK && memcmp(&stored_record(), &r, sizeof(r)) == 0;
    printf("[Wake] beacon saved rc=%d ok=%d\n", rc, ok);
    return ok;
}

void expect(uint16_t opcode, Step next) {
    waiting_opcode = opcode;
    step = next;
}

// Only writes on change: every write is a CYW43 SPI transaction.
void led_set(bool on) {
    if (led_on != on) {
        led_on = on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    }
}

void led_task() {
    if (get_config().disable_pico_led) {
        return;
    }
    const uint32_t now = now_ms();
    const bool advertising = step >= Step::WakeAdvOn && step <= Step::WakeAdvOff;
    if (advertising || led_blinks_left > 0) {
        if (!led_owned || now - led_last_ms >= 100) {
            led_owned = true;
            led_last_ms = now;
            led_set(!led_on);
            if (led_blinks_left > 0) {
                led_blinks_left--;
            }
        }
        return;
    }
    if (learning) {
        led_owned = true;
        led_set((now % 1000) < 500);
        return;
    }
    if (led_owned) {
        // Hand the LED back in bt.cpp's state: on while the DualSense is connected.
        led_owned = false;
        led_on = bt_is_connected();
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
    }
}

void update_console_state(uint32_t now) {
    if (!wake_host_suspended()) {
        suspended_since_ms = 0;
    } else if (suspended_since_ms == 0) {
        suspended_since_ms = now;
    }
    if (usb_mounted()) {
        unmounted_since_ms = 0;
    } else if (unmounted_since_ms == 0) {
        unmounted_since_ms = now;
    }
    if (!console_asleep &&
        ((suspended_since_ms != 0 && now - suspended_since_ms >= ASLEEP_AFTER_MS) ||
         (unmounted_since_ms != 0 && now - unmounted_since_ms >= UNMOUNTED_ASLEEP_MS))) {
        console_asleep = true;
        printf("[Wake] console asleep\n");
    }
    if (now - report_window_ms >= AWAKE_WINDOW_MS) {
        if (console_asleep && reports_sent >= AWAKE_REPORTS) {
            console_asleep = false;
            printf("[Wake] console awake\n");
        }
        reports_sent = 0;
        report_window_ms = now;
    }
}

void send_next_command() {
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
            // passive, interval 60 ms, window 30 ms, own address public, accept all
            hci_send_cmd(&le_set_scan_parameters, 0, 0x0060, 0x0030, 0, 0);
            expect(le_set_scan_parameters.opcode, Step::Ready);
            break;
        case Step::ScanOn:
            hci_send_cmd(&le_set_scan_enable, 1, 1); // filter duplicates
            scanning = true;
            expect(le_set_scan_enable.opcode, Step::Ready);
            break;
        case Step::ScanOff:
            hci_send_cmd(&le_set_scan_enable, 0, 0);
            scanning = false;
            expect(le_set_scan_enable.opcode, Step::Ready);
            break;

        case Step::WakeScanOff:
            if (!scanning) {
                step = Step::WakeSetAddr;
                send_next_command();
                return;
            }
            hci_send_cmd(&le_set_scan_enable, 0, 0);
            scanning = false;
            expect(le_set_scan_enable.opcode, Step::WakeSetAddr);
            break;
        case Step::WakeSetAddr: {
            gap_local_bd_addr(own_addr);
            bd_addr_t addr;
            memcpy(addr, stored_record().addr, sizeof(addr));
            hci_send_cmd(&bcm_write_bd_addr, addr);
            expect(bcm_write_bd_addr.opcode, Step::WakeAdvParameters);
            break;
        }
        case Step::WakeAdvParameters: {
            // ADV_IND at 20 ms on all channels, public address. Non-connectable at 30-50 ms
            // was tested too: it wakes less reliably and later.
            bd_addr_t no_peer{};
            hci_send_cmd(&le_set_adv_parameters, 0x0020, 0x0020, 0, 0, 0, no_peer, 0x07, 0);
            expect(le_set_adv_parameters.opcode, Step::WakeAdvData);
            break;
        }
        case Step::WakeAdvData: {
            static_assert(ADV_LEN == 3 * 8 + 7);
            const uint8_t *d = stored_record().data;
            hci_send_cmd(&le_set_adv_data, ADV_LEN, d, d + 8, d + 16,
                         d[24], d[25], d[26], d[27], d[28], d[29], d[30]);
            expect(le_set_adv_data.opcode, Step::WakeAdvOn);
            break;
        }
        case Step::WakeAdvOn:
            hci_send_cmd(&le_set_adv_enable, 1);
            expect(le_set_adv_enable.opcode, Step::WakeAdvWait);
            break;
        case Step::WakeAdvWait:
            if (static_cast<int32_t>(now_ms() - wake_end_ms) >= 0) {
                step = Step::WakeAdvOff;
            }
            break;
        case Step::WakeAdvOff:
            hci_send_cmd(&le_set_adv_enable, 0);
            expect(le_set_adv_enable.opcode, Step::WakeRestoreAddr);
            break;
        case Step::WakeRestoreAddr:
            hci_send_cmd(&bcm_write_bd_addr, own_addr);
            expect(bcm_write_bd_addr.opcode, Step::Ready);
            printf("[Wake] beacon sent, address restored\n");
            break;
        default:
            break;
    }
}

} // namespace

void switch_wake_on_input(bool create, bool options, bool triangle) {
    if (!(create && options && triangle)) {
        learn_combo_since = nil_time;
        learn_combo_fired = false;
        return;
    }
    if (learn_combo_fired) {
        return;
    }
    if (is_nil_time(learn_combo_since)) {
        learn_combo_since = get_absolute_time();
    } else if (absolute_time_diff_us(learn_combo_since, get_absolute_time()) >= LEARN_HOLD_US) {
        learn_combo_fired = true;
        learn_requested = true;
    }
}

bool switch_wake_store_beacon(const uint8_t addr[6], const uint8_t data[31]) {
    return save_record(make_record(addr, data));
}

void switch_wake_note_report_sent() {
    reports_sent++;
}

void switch_wake_on_hci_event(uint8_t packet_type, const uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET || size < 2) {
        return;
    }
    if (hci_event_packet_get_type(packet) == BTSTACK_EVENT_STATE) {
        hci_working = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
        step = hci_working && is_switch_pro_mode() ? Step::SetEventMask : Step::Off;
        waiting_opcode = 0;
        scanning = false;
        return;
    }
    if (!active()) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            if (waiting_opcode == 0 || opcode != waiting_opcode) {
                break;
            }
            waiting_opcode = 0;
            const uint8_t status = hci_event_command_complete_get_return_parameters(packet)[0];
            if (status != ERROR_CODE_SUCCESS) {
                printf("[Wake] opcode 0x%04X failed status=0x%02X\n", opcode, status);
                if (opcode == le_set_scan_enable.opcode) {
                    scanning = false;
                }
                retry_after_ms = now_ms() + 1000;
                if (step >= Step::WakeAdvParameters && step <= Step::WakeRestoreAddr) {
                    step = Step::WakeAdvOff; // stop advertising and put the real address back
                } else if (step == Step::Ready && opcode == bcm_write_bd_addr.opcode) {
                    step = Step::WakeRestoreAddr; // retry the restore
                } else if (step < Step::Ready) {
                    step = Step::Off;
                }
            } else if (step == Step::WakeAdvWait) {
                wake_end_ms = now_ms() + WAKE_ADV_MS;
            }
            break;
        }

        case HCI_EVENT_LE_META:
            if (size >= 3 && packet[2] == HCI_SUBEVENT_LE_ADVERTISING_REPORT) {
                handle_adv_report(packet, size);
            }
            break;

        default:
            break;
    }
}

void switch_wake_on_bt_connect() {
    if (!is_switch_pro_mode() || !console_asleep) {
        return;
    }
    if (!record_valid(stored_record())) {
        printf("[Wake] console asleep but no beacon learned yet\n");
        led_blinks_left = 4; // 2 fast blinks: nothing to send
        return;
    }
    wake_requested = true;
}

void switch_wake_task() {
    const uint32_t now = now_ms();
    if (learn_requested) {
        learn_requested = false;
        learning = true;
        learn_until_ms = now + LEARN_MS;
        printf("[Wake] learn mode: sleep the console and press HOME on a Joy-Con 2\n");
        if (step == Step::Off && hci_working) {
            step = Step::SetEventMask;
        }
    }
    if (learning && static_cast<int32_t>(now - learn_until_ms) >= 0) {
        learning = false;
        printf("[Wake] learn mode timed out\n");
    }
    if (!active() && led_blinks_left == 0 && !led_owned) {
        return;
    }
    led_task();
    if (is_switch_pro_mode()) {
        update_console_state(now);
    }

    if (save_pending) {
        save_pending = false;
        learning = false;
        if (save_record(pending)) {
            led_blinks_left = 10; // 5 blinks
        }
        return;
    }
    if (step == Step::Off || waiting_opcode != 0) {
        return;
    }
    if (step == Step::Ready) {
        if (wake_requested) {
            wake_requested = false;
            printf("[Wake] DualSense reconnected while the console sleeps: sending wake beacon\n");
            step = Step::WakeScanOff;
        } else if (static_cast<int32_t>(now - retry_after_ms) < 0) {
            return;
        } else if (learning != scanning) {
            step = learning ? Step::ScanOn : Step::ScanOff;
        } else {
            return;
        }
    }
    send_next_command();
}

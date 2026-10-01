#pragma once

#include <cstdint>

// BLE scan experiment (DS5_BLE_SCAN build option): logs the Switch 2 wake
// beacon that a Joy-Con 2 sends when HOME is pressed on a sleeping console.
void ble_scan_on_hci_event(uint8_t packet_type, const uint8_t *packet, uint16_t size);
void ble_scan_task();

#pragma once

#include <cstdint>

// Switch 2 wake from sleep. See switch_wake.cpp.
void switch_wake_on_hci_event(uint8_t packet_type, const uint8_t *packet, uint16_t size);
void switch_wake_on_bt_connect();
// BTstack classic connection filter hook: true = decline this incoming connection
// (a paired controller reconnecting while the console sleeps; the beacon goes out).
bool switch_wake_defer_connection(const uint8_t addr[6]);
// True once after a declined connection request, so bt.cpp doesn't accept it.
bool switch_wake_take_deferred();
void switch_wake_on_input(bool create, bool options, bool triangle); // learn-mode combo
void switch_wake_note_report_sent(); // an input report reached the console (USB IN complete)
void switch_wake_task();
// Store a wake beacon (sender address big-endian, 31-byte payload) in flash. Works in
// any mode; the BLE scan experiment uses it too. Main loop only.
bool switch_wake_store_beacon(const uint8_t addr[6], const uint8_t data[31]);

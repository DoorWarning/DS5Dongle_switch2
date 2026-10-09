#pragma once

#include <cstdint>

// Switch 2 wake from sleep. See switch_wake.cpp.
void switch_wake_on_hci_event(uint8_t packet_type, const uint8_t *packet, uint16_t size);
void switch_wake_on_bt_connect();
void switch_wake_on_input(bool create, bool options, bool triangle); // learn-mode combo
void switch_wake_note_report_sent(); // an input report reached the console (USB IN complete)
void switch_wake_task();
// Store a wake beacon (sender address big-endian, 31-byte payload) in flash. Works in
// any mode; the BLE scan experiment uses it too. Main loop only.
bool switch_wake_store_beacon(const uint8_t addr[6], const uint8_t data[31]);
bool switch_wake_beacon_learned();
// Manager app control (USB callback context: only sets requests for switch_wake_task).
void switch_wake_set_learning(bool on);
bool switch_wake_learning();
void switch_wake_forget();

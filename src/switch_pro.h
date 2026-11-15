//
// Created for classic Switch Pro USB emulation.
//

#ifndef DS5_BRIDGE_SWITCH_PRO_H
#define DS5_BRIDGE_SWITCH_PRO_H

#include <cstdint>

#include "ds5_input.h"

void switch_pro_init();
void switch_pro_on_ds5_input(const USBGetStateData &ds5);
void switch_pro_task();
uint16_t switch_pro_get_report(uint8_t report_id, uint8_t *buffer, uint16_t reqlen);
void switch_pro_handle_hid_out(uint8_t report_id, uint8_t const *buffer, uint16_t len);
// Last second's DualSense input rate, report rate to the host, and how old the
// newest DualSense input was when a report went out (average, microseconds).
void switch_pro_latency(uint16_t &ds5_hz, uint16_t &report_hz, uint16_t &age_us);

#endif //DS5_BRIDGE_SWITCH_PRO_H

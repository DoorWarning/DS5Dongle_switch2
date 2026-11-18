#pragma once

#include <cstdint>

// Companion protocol for the PC manager app (ds5_dongle_manager). See companion.cpp.

// DS5/DSE mode: vendor Feature report. Upstream uses 0xFA/0xFB for button
// remapping, so the companion report lives at 0xFE and every request starts
// with COMPANION_MAGIC; anything else written to it is ignored.
constexpr uint8_t COMPANION_REPORT_ID = 0xFE;
constexpr uint8_t COMPANION_MAGIC[2] = {'D', 'W'};
constexpr uint8_t COMPANION_SUBCOMMAND = 0xE0; // Switch Pro mode: output 0x01 subcommand

// Handles one request [cmd, seq, len, payload...] and writes the reply
// [cmd, seq, status, len, data...] into `reply`. Returns the reply length.
uint16_t companion_handle(const uint8_t *request, uint16_t request_len, uint8_t *reply, uint16_t reply_max);

// DS5/DSE mode transport (Feature report COMPANION_REPORT_ID).
void companion_feature_set(const uint8_t *buffer, uint16_t len);
uint16_t companion_feature_get(uint8_t *buffer, uint16_t reqlen);

// Main loop: work a request could not do inside the USB callback (flash saves).
void companion_task();

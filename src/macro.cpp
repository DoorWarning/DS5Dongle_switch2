#include "macro.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bt.h"
#include "config.h"
#include "utils.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/btstack_flash_bank.h"
#include "pico/flash.h"
#include "pico/time.h"

namespace {

constexpr uint8_t MACRO_SLOTS = 4;
constexpr uint32_t MACRO_MAGIC = 0x4D43524F;
constexpr uint32_t MACRO_FLASH_OFFSET =
    PICO_FLASH_BANK_STORAGE_OFFSET - FLASH_SECTOR_SIZE - MACRO_SLOTS * FLASH_SECTOR_SIZE;
constexpr uint64_t PLAY_LOOP_HOLD_US = 1000000;
constexpr uint64_t RECORD_HOLD_US = 3000000;
constexpr uint32_t MAX_EVENT_MS = 60000;
constexpr int STICK_EVENT_DELTA = 64;
constexpr int STICK_DEADZONE = 200;
constexpr int STICK_CENTER = 2048;

struct __attribute__((packed)) MacroEvent {
    uint16_t dur_ms;
    uint8_t buttons[3];
    uint8_t sticks[6];
    uint8_t reserved;
};

struct __attribute__((packed)) MacroHeader {
    uint32_t magic;
    uint16_t count;
    uint16_t reserved;
    uint32_t crc;
    uint32_t reserved2;
};

constexpr uint16_t MAX_EVENTS = (FLASH_SECTOR_SIZE - sizeof(MacroHeader)) / sizeof(MacroEvent);

struct __attribute__((packed)) MacroSlot {
    MacroHeader header;
    MacroEvent events[MAX_EVENTS];
    uint8_t padding[FLASH_SECTOR_SIZE - sizeof(MacroHeader) - MAX_EVENTS * sizeof(MacroEvent)];
};

static_assert(sizeof(MacroSlot) == FLASH_SECTOR_SIZE);
static_assert(MACRO_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

enum class State : uint8_t {
    Idle,
    Chord,
    RecordArmed,
    Recording,
    Playing,
};

// Saved slots are played straight from XIP flash. Only the slot being recorded
// lives in RAM, and only in Switch Pro mode: PC mode has just a few KB of heap
// left after core1's opus allocations, so this buffer comes from the heap that
// Switch mode frees by skipping opus (see core1_entry in audio.cpp).
MacroSlot *rec_slot = nullptr;
bool slot_valid[MACRO_SLOTS]{};

State state = State::Idle;
bool prev_mute = false;
bool mute_consumed = false;
int chord_slot = -1;
uint64_t chord_start_us = 0;

int active_slot = -1;
uint16_t rec_count = 0;
uint64_t rec_event_start_us = 0;

uint16_t play_index = 0;
uint64_t play_event_start_us = 0;
bool play_loop = false;

int pending_save_slot = -1;

uint32_t slot_crc(const MacroSlot &slot) {
    return crc32(reinterpret_cast<const uint8_t *>(slot.events), slot.header.count * sizeof(MacroEvent));
}

const MacroSlot &flash_slot(int slot) {
    return *reinterpret_cast<const MacroSlot *>(XIP_BASE + MACRO_FLASH_OFFSET + slot * FLASH_SECTOR_SIZE);
}

// A just-recorded slot stays in RAM until macro_task() has written it to flash.
const MacroSlot &play_slot(int slot) {
    return slot == pending_save_slot ? *rec_slot : flash_slot(slot);
}

void stick_decode(const uint8_t *p, int &x, int &y) {
    x = p[0] | ((p[1] & 0x0F) << 8);
    y = (p[1] >> 4) | (p[2] << 4);
}

bool stick_centered(const uint8_t *p) {
    int x, y;
    stick_decode(p, x, y);
    return std::abs(x - STICK_CENTER) < STICK_DEADZONE && std::abs(y - STICK_CENTER) < STICK_DEADZONE;
}

bool stick_moved(const uint8_t *a, const uint8_t *b) {
    int ax, ay, bx, by;
    stick_decode(a, ax, ay);
    stick_decode(b, bx, by);
    return std::abs(ax - bx) >= STICK_EVENT_DELTA || std::abs(ay - by) >= STICK_EVENT_DELTA;
}

bool frame_neutral(const MacroEvent &ev) {
    return ev.buttons[0] == 0 && ev.buttons[1] == 0 && ev.buttons[2] == 0 &&
           stick_centered(ev.sticks) && stick_centered(ev.sticks + 3);
}

void set_mute_light(MuteLight::MuteLight mode) {
    const SetStateData s{
        .AllowMuteLight = 1,
        .MuteLightMode = mode,
    };
    update_state(s);
}

void start_playback(int slot, bool loop) {
    if (!slot_valid[slot] || play_slot(slot).header.count == 0) {
        return;
    }
    active_slot = slot;
    play_index = 0;
    play_event_start_us = time_us_64();
    play_loop = loop;
    state = State::Playing;
    set_mute_light(loop ? MuteLight::On : MuteLight::Off);
    printf("[Macro] play slot %d loop=%d\n", slot, loop);
}

void stop_playback() {
    state = State::Idle;
    active_slot = -1;
    set_mute_light(MuteLight::Off);
    printf("[Macro] stop\n");
}

void push_event(const uint8_t io[9], uint64_t now) {
    MacroEvent &ev = rec_slot->events[rec_count];
    ev.dur_ms = 0;
    memcpy(ev.buttons, io, 3);
    memcpy(ev.sticks, io + 3, 6);
    ev.reserved = 0;
    rec_count++;
    rec_event_start_us = now;
}

void close_last_event(uint64_t now) {
    if (rec_count == 0) {
        return;
    }
    uint64_t ms = (now - rec_event_start_us) / 1000;
    if (ms > MAX_EVENT_MS) {
        ms = MAX_EVENT_MS;
    }
    if (ms == 0) {
        ms = 1;
    }
    rec_slot->events[rec_count - 1].dur_ms = static_cast<uint16_t>(ms);
}

void finish_recording(uint64_t now) {
    close_last_event(now);
    MacroSlot &slot = *rec_slot;
    uint16_t start = 0;
    while (start < rec_count && frame_neutral(slot.events[start])) {
        start++;
    }
    if (start > 0 && start < rec_count) {
        memmove(slot.events, slot.events + start, (rec_count - start) * sizeof(MacroEvent));
    }
    const uint16_t count = start < rec_count ? rec_count - start : 0;
    slot.header.magic = MACRO_MAGIC;
    slot.header.count = count;
    slot.header.reserved = 0;
    slot.header.reserved2 = 0;
    slot.header.crc = slot_crc(slot);
    slot_valid[active_slot] = count > 0;
    pending_save_slot = active_slot;
    printf("[Macro] recorded slot %d events=%u\n", active_slot, count);
    state = State::Idle;
    active_slot = -1;
    set_mute_light(MuteLight::Off);
}

void record_frame(const uint8_t io[9], uint64_t now) {
    const MacroEvent &last = rec_slot->events[rec_count - 1];
    const bool changed = memcmp(last.buttons, io, 3) != 0 ||
                         stick_moved(last.sticks, io + 3) ||
                         stick_moved(last.sticks + 3, io + 6);
    const bool too_long = (now - rec_event_start_us) / 1000 >= MAX_EVENT_MS;
    if (!changed && !too_long) {
        return;
    }
    if (rec_count >= MAX_EVENTS) {
        finish_recording(now);
        return;
    }
    close_last_event(now);
    push_event(io, now);
}

void apply_playback(uint8_t io[9], uint64_t now) {
    const MacroSlot &slot = play_slot(active_slot);
    while (now - play_event_start_us >= static_cast<uint64_t>(slot.events[play_index].dur_ms) * 1000) {
        play_event_start_us += static_cast<uint64_t>(slot.events[play_index].dur_ms) * 1000;
        play_index++;
        if (play_index >= slot.header.count) {
            if (!play_loop) {
                stop_playback();
                return;
            }
            play_index = 0;
        }
    }
    const MacroEvent &ev = slot.events[play_index];
    io[0] |= ev.buttons[0];
    io[1] |= ev.buttons[1];
    io[2] |= ev.buttons[2];
    if (!stick_centered(ev.sticks)) {
        memcpy(io + 3, ev.sticks, 3);
    }
    if (!stick_centered(ev.sticks + 3)) {
        memcpy(io + 6, ev.sticks + 3, 3);
    }
}

void save_flash_op(void *param) {
    const int slot = *static_cast<int *>(param);
    const uint32_t offset = MACRO_FLASH_OFFSET + slot * FLASH_SECTOR_SIZE;
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, reinterpret_cast<const uint8_t *>(rec_slot), FLASH_SECTOR_SIZE);
    restore_interrupts(interrupts);
}

}

void macro_init() {
    if (!is_switch_pro_mode()) {
        return;
    }
    rec_slot = static_cast<MacroSlot *>(malloc(sizeof(MacroSlot)));
    for (int i = 0; i < MACRO_SLOTS; i++) {
        const MacroSlot &slot = flash_slot(i);
        slot_valid[i] = slot.header.magic == MACRO_MAGIC &&
                        slot.header.count > 0 &&
                        slot.header.count <= MAX_EVENTS &&
                        slot.header.crc == slot_crc(slot);
        printf("[Macro] slot %d valid=%d events=%u\n", i, slot_valid[i], slot_valid[i] ? slot.header.count : 0);
    }
}

void macro_process(uint8_t io[9], bool mute, bool circle, bool cross, bool triangle, bool square) {
    if (rec_slot == nullptr) {
        return;
    }
    const uint64_t now = time_us_64();
    const bool mute_edge = mute && !prev_mute;
    prev_mute = mute;

    int face = -1;
    if (circle) face = 0;
    else if (cross) face = 1;
    else if (triangle) face = 2;
    else if (square) face = 3;

    if (!mute) {
        mute_consumed = false;
    }

    switch (state) {
        case State::Recording:
            if (mute_edge) {
                finish_recording(now);
                mute_consumed = true;
                break;
            }
            record_frame(io, now);
            return;

        case State::Playing:
            if (mute_edge) {
                stop_playback();
                mute_consumed = true;
                break;
            }
            apply_playback(io, now);
            return;

        case State::RecordArmed:
            // rec_slot is still busy until the previous recording is flashed.
            if (!mute && face < 0 && pending_save_slot < 0) {
                state = State::Recording;
                rec_count = 0;
                push_event(io, now);
                printf("[Macro] recording slot %d\n", active_slot);
            }
            break;

        case State::Chord:
            if (!mute) {
                state = State::Idle;
                chord_slot = -1;
                break;
            }
            if (face != chord_slot) {
                const uint64_t held = now - chord_start_us;
                const int slot = chord_slot;
                state = State::Idle;
                chord_slot = -1;
                mute_consumed = true;
                if (held < PLAY_LOOP_HOLD_US) {
                    start_playback(slot, false);
                } else {
                    start_playback(slot, true);
                }
                break;
            }
            if (now - chord_start_us >= RECORD_HOLD_US) {
                state = State::RecordArmed;
                active_slot = chord_slot;
                chord_slot = -1;
                mute_consumed = true;
                set_mute_light(MuteLight::Breathing);
                printf("[Macro] armed slot %d\n", active_slot);
            }
            break;

        case State::Idle:
            if (mute && !mute_consumed && face >= 0) {
                state = State::Chord;
                chord_slot = face;
                chord_start_us = now;
            }
            break;
    }

    if (mute) {
        io[0] &= static_cast<uint8_t>(~0x0F);
    }
}

void macro_task() {
    if (pending_save_slot < 0) {
        return;
    }
    int slot = pending_save_slot;
    pending_save_slot = -1;
    watchdog_update();
    const int rc = flash_safe_execute(save_flash_op, &slot, 1000);
    watchdog_update();
    printf("[Macro] save slot %d rc=%d\n", slot, rc);
}

void macro_restore_mute_light() {
    switch (state) {
        case State::RecordArmed:
        case State::Recording:
            set_mute_light(MuteLight::Breathing);
            break;
        case State::Playing:
            set_mute_light(play_loop ? MuteLight::On : MuteLight::Off);
            break;
        default:
            set_mute_light(MuteLight::Off);
            break;
    }
}

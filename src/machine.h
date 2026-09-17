/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// machine.h — one quadrant: a GBA, the thread driving it, and the little
// window through which the host thread is allowed to look at it.
//
// The division of labour is the one the previous four-machine project settled
// on. A machine belongs to the thread that runs it. The host thread never
// touches a core; it writes keys, reads finished frames out from under a lock,
// and reads a handful of atomics for the status display. Everything shared is
// in this header, and there is deliberately not much of it.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gba_instance.h"

namespace gql {

constexpr int kMaxPlayers = 4;

// What the status gutter shows, and roughly the order things go wrong in.
enum class LinkState {
    Off,          // not asked to link at all
    Dialling,     // no Dolphin answering yet
    Waiting,      // connected, but the guest has not entered JOY bus mode
    Linked,       // connected and talking
    Lost,         // was linked, Dolphin went away
    Cable,        // on the link cable with the other machines
};

// What a machine's serial port is plugged into. A GBA has one, and mGBA has
// one driver slot per core, so these are exclusive.
enum class LinkMode { None, Dolphin, Cable };

const char* link_state_name(LinkState s);

struct Machine {
    int index = 0;

    // Per machine, because four people may be playing four different games.
    // Empty rom_path means boot the BIOS with no cartridge, which is what a
    // guest waiting to be handed a program over the link does.
    std::string rom_path;
    std::string save_path;
    GbaInstance gba;
    std::thread thread;

    // Host thread writes, core thread reads. A GBA keypad fits in one 16-bit
    // store, so this needs no lock — active low, 0x03FF is nothing held.
    std::atomic<uint16_t> keys{0x03FF};

    // Finished frames, copied out under the lock so the host thread never
    // reads a framebuffer that is halfway through being written.
    std::mutex fb_mutex;
    std::vector<uint32_t> pixels;   // ABGR8888, 240x160
    bool has_frame = false;

    // Status, for the gutter. Cheap to read and always slightly stale, which
    // is fine for something a person looks at.
    std::atomic<uint64_t> frames{0};
    std::atomic<double> fps{0.0};
    std::atomic<LinkState> link{LinkState::Off};
    std::atomic<bool> booted{false};   // open() succeeded

    // Set by the host thread before the core thread starts, read by it after.
    bool dialled = false;

    LinkMode mode = LinkMode::None;

    // Stops this machine alone, so a player can be handed a different
    // cartridge without the other three being taken down with them.
    std::atomic<bool> stop{false};

    Machine() = default;
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
};

}  // namespace gql

/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// gba_instance.h — one GBA, and everything the host needs from it.
//
// This is the seam the previous project settled on, re-cut around libmgba
// instead of a recompiler. The rule it exists to enforce is the same: an
// instance belongs to the thread that opened it and must be driven only from
// there. The host thread never touches a core; it reads finished frames and
// drained audio through the small locked surface at the bottom of this file.
//
// libmgba makes that easy — an mCore is self-contained, with no process-wide
// state to collide over — so four of these run side by side with no more
// ceremony than four of anything else.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct mCore;
struct VFile;

namespace gql {

// Point mGBA's logging at stderr with a level worth reading. Left alone it
// prints every DMA, SWI and register poke a game makes, which at four cores is
// thousands of lines a second and hides the one message that matters.
// Process-wide, so this is called once, before any core is opened.
void install_logger(bool verbose);

// Tag this thread's mGBA log lines with a player number, since the logger is
// shared by every core and the lines are otherwise indistinguishable. -1 for
// untagged, which is the default.
void set_log_player(int player);

class GbaInstance {
public:
    // The real thing, unwidened. The recompiler's 284-column view was a
    // property of that engine, not of the console, and does not come across.
    static constexpr int kWidth = 240;
    static constexpr int kHeight = 160;

    GbaInstance() = default;
    ~GbaInstance();
    GbaInstance(const GbaInstance&) = delete;
    GbaInstance& operator=(const GbaInstance&) = delete;

    // `rom` may be empty, which boots the BIOS with no cartridge — the state a
    // GBA sits in waiting for a multiboot download, and how Four Swords
    // Adventures expects to find it. That path needs a real BIOS: mGBA's HLE
    // BIOS covers SWI calls only and has none of the boot ROM's JOY-bus code.
    // `bios` may be empty when a ROM boots on its own.
    bool open(const std::string& rom, const std::string& bios,
              unsigned sample_rate, std::string* err);
    void close();

    // Runs until the next frame is complete. Once the Dolphin link is attached
    // this stops being a bounded call: the clock socket decides when the core
    // may advance, and mGBA's driver waits inside the run for it. Callers must
    // hold no lock the host thread wants.
    void run_frame();

    // Active-low KEYINPUT, the convention the binding model speaks. mGBA wants
    // the opposite; the inversion happens here so only one place knows.
    void set_keys(uint16_t keyinput);

    // Interleaved stereo at the host rate passed to open(). `frames` is sample
    // pairs, and the return is how many were actually available.
    //
    // Resampled on the way out, which is not optional. A GBA's output rate is
    // whatever the running program's SOUNDBIAS resolution field says: 32768 Hz
    // from reset, doubling per step up to 262144 Hz, changed by a register
    // write at any moment. There is no rate to open the sound card at, so the
    // card gets a fixed one and mGBA's resampler absorbs the difference —
    // including a mid-game change, which core_sample_rate() reports.
    std::size_t drain_audio(int16_t* out, std::size_t frames);

    // The core's current native rate, for diagnostics. Not the rate of the
    // samples drain_audio() hands back.
    unsigned core_sample_rate() const;

    // ABGR8888, kWidth * kHeight, valid until the next run_frame().
    const uint32_t* pixels() const { return video_.data(); }

    bool open_ok() const { return core_ != nullptr; }

    // --- The Dolphin link -------------------------------------------------
    //
    // Two calls, because the two halves belong to different threads and the
    // order of both matters.
    //
    // dial() opens the pair of sockets and nothing else. It is called from the
    // host thread, once per instance, *in player order*: Dolphin assigns the
    // connections it accepts to SI slots in the order they arrive, so dialling
    // four at once from four threads shuffles the players between quadrants at
    // random.
    //
    // attach() hands the connected driver to the core, and must run on the
    // thread that owns the core because it schedules against the core's own
    // timing. It must also come after dial() succeeded: mGBA's driver gives up
    // permanently if its first timing event finds no socket — it returns
    // without rescheduling itself, and nothing ever winds it up again.
    bool dial(const std::string& host, uint16_t data_port, uint16_t clock_port,
              std::string* err);
    void attach();
    bool linked() const;

    // Break the link from *another* thread, to get the core thread out of a
    // stalled run_frame().
    //
    // When Dolphin stops granting cycles, mGBA's driver waits for more and
    // run_frame() never returns — so a core thread parked in a stall cannot be
    // joined, and the app hangs on the way out instead of quitting. Shutting
    // the sockets down makes the waiting read fail, the driver gives up, and
    // the frame finishes. shutdown() rather than close(): it wakes the reader
    // without freeing a descriptor the other thread is still holding.
    void shutdown_link();

private:
    mCore* core_ = nullptr;
    VFile* rom_vf_ = nullptr;
    VFile* bios_vf_ = nullptr;
    std::vector<uint32_t> video_;

    // Resampling from the core's rate to the host's. Held by pointer so this
    // header does not drag mGBA's internals into everything that includes it.
    struct Audio;
    Audio* audio_ = nullptr;
    unsigned host_rate_ = 0;

    struct Link;
    Link* link_ = nullptr;
};

}  // namespace gql

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

// Narrow the log to the serial port, at full detail. Everything mGBA logs
// about a link, and nothing about anything else — with four guests, its full
// output is thousands of lines a second and the twenty that matter are not
// findable in it.
void log_only_sio();

class CableGroup;

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
    // `save` is where the cartridge's battery-backed memory lives. It is
    // created if missing and written back as the guest writes it, so it must
    // be somewhere the process can write; empty means run without one, which
    // is right for a BIOS boot with no cartridge and wrong for anything else.
    bool open(const std::string& rom, const std::string& bios,
              const std::string& save, unsigned sample_rate,
              std::string* err);
    void close();

    // Runs until a video frame completes, or until something suspends the
    // core, whichever comes first. True if a frame was finished.
    //
    // Not mCore::runFrame, which is a trap for anything driving a core
    // alongside others. That loops until the frame counter moves and ignores
    // the CPU interrupt, so a core told to stop keeps running to the end of
    // its frame — up to 280,000 cycles after the coordinator believes it has
    // stopped. mGBA's own thread calls runLoop for exactly this reason. The
    // link cable needs a core that stops when it is told to, and "next
    // frame" is far too coarse a moment to stop at.
    //
    // Once a link is attached this also stops being a bounded call: the far
    // end decides when the core may advance. Callers must hold no lock the
    // host thread wants.
    bool run_frame();

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

    // --- The link cable between our own machines ---------------------------
    //
    // Mutually exclusive with the Dolphin link, because a GBA has one serial
    // port and mGBA has one driver slot per core. Call from the core's own
    // thread, before the run loop, like attach().
    //
    // `preferred_id` is the position on the cable this machine would like,
    // which is its quadrant, so that player one is the parent.
    void attach_cable(CableGroup* group, int preferred_id);

    // Block while the coordinator has this machine asleep.
    //
    // The coordinator stops a machine that has run ahead by marking it asleep
    // and forcing its CPU out of the run loop, so run_frame() returns early
    // and this is where the waiting actually happens. Call it from the core's
    // own thread, straight after run_frame(), and hold no lock the other
    // machines want.
    void cable_wait();

    // Release a machine parked in cable_wait(), from another thread, so it can
    // be joined. Same job as shutdown_link() does for Dolphin.
    void wake_cable();

    bool on_cable() const;

    // What the coordinator thinks of this machine. Zero id means it never
    // registered; zero devices means the guest is being told nothing else is
    // on the cable, which is exactly what a game that cannot find anyone sees.
    unsigned cable_id() const;
    int cable_devices() const;
    int cable_player_id() const;
    unsigned long cable_sleeps() const;
    // Times this machine gave up waiting to be woken. Should be zero.
    unsigned long cable_timeouts() const;

    // Defined in the implementation; named here only so the coordinator's
    // callbacks, which are free functions, can reach it.
    struct Cable;

    // Whether the far end is still there. Call it from the core's own thread.
    //
    // mGBA's driver does not notice a link that has gone away: on end-of-file
    // its clock read returns nothing, its command read fails, and it falls
    // through to advancing the core by its own grain with nothing gating it.
    // A dropped link therefore looks like a core running at twenty times speed
    // rather than one that has stopped, so it has to be detected out here.
    bool link_alive() const;

    // Whether the guest has put its serial port in JOY bus mode.
    //
    // This is the question to ask first when a link looks connected but
    // nothing happens. mGBA's driver answers Dolphin's JOY commands only while
    // the guest is in this mode — otherwise it reads the command, sends no
    // reply, and both ends sit there looking healthy. A GBA waiting for a
    // multiboot download is in JOY bus mode; a cartridge that never wants the
    // link never enters it.
    bool joybus_active() const;

    // The raw serial mode and RCNT, for when "not JOY bus" is not a useful
    // enough answer. JOY bus is mode 12, and RCNT[15:14] = 11 is what selects
    // it.
    int sio_mode() const;
    uint16_t rcnt() const;

    // Every serial mode the guest has been in since boot, as a bitmask of
    // 1 << mode. Sampling the current mode once a frame would miss a JOY bus
    // window that opens and closes inside one — and "did it ever get there"
    // is the question, not "is it there now". Call from the core's thread.
    uint32_t sio_modes_seen() const { return modes_seen_; }
    void note_sio_mode();

    // How much of the guest's screen is not black, in percent. A GBA holding a
    // multiboot wait screen is not blank, and a core that never got past the
    // BIOS is — which is otherwise hard to tell apart without looking.
    int screen_activity() const;

private:
    mCore* core_ = nullptr;
    VFile* rom_vf_ = nullptr;
    VFile* bios_vf_ = nullptr;
    VFile* save_vf_ = nullptr;
    std::vector<uint32_t> video_;

    // Resampling from the core's rate to the host's. Held by pointer so this
    // header does not drag mGBA's internals into everything that includes it.
    struct Audio;
    Audio* audio_ = nullptr;
    unsigned host_rate_ = 0;

    struct Link;
    Link* link_ = nullptr;

    Cable* cable_ = nullptr;
    uint32_t modes_seen_ = 0;
};

}  // namespace gql

/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba_instance.h"

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/vfs.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/dolphin.h>

#include <fcntl.h>
#include <sys/socket.h>

#include <cerrno>

#include <cstdarg>
#include <cstdio>

namespace gql {
namespace {

// A frame of stereo at the lowest rate the card is likely to want, with room
// to spare. The resampler wants somewhere to put a frame's worth in one go and
// this is never the thing that runs short.
constexpr std::size_t kHostBufferFrames = 4096;

// Which player's core is running on this thread, for the shared logger.
thread_local int t_log_player = -1;

int g_log_levels = mLOG_FATAL | mLOG_ERROR | mLOG_WARN;

void log_to_stderr(struct mLogger*, int category, enum mLogLevel level,
                   const char* format, va_list args) {
    if (!(level & g_log_levels)) return;
    char line[512];
    std::vsnprintf(line, sizeof line, format, args);
    const char* cat = mLogCategoryName(category);
    if (t_log_player >= 0)
        std::fprintf(stderr, "[p%d %s] %s\n", t_log_player + 1,
                     cat ? cat : "?", line);
    else
        std::fprintf(stderr, "[%s] %s\n", cat ? cat : "?", line);
}

struct mLogger g_logger = {log_to_stderr, nullptr};

}  // namespace

void install_logger(bool verbose) {
    // GAME_ERROR is the guest doing something invalid, which for a commercial
    // ROM is usually normal and always noisy. STUB is mGBA telling us about
    // its own unimplemented corners. Neither belongs in the default output.
    g_log_levels = verbose
                       ? (mLOG_ALL & ~mLOG_GAME_ERROR)
                       : (mLOG_FATAL | mLOG_ERROR | mLOG_WARN);
    mLogSetDefaultLogger(&g_logger);
}

void set_log_player(int player) { t_log_player = player; }

// mGBA's Dolphin driver, and the one flag telling us whether it was ever
// handed a live pair of sockets. Kept out of the header with the rest.
struct GbaInstance::Link {
    struct GBASIODolphin dol;
    bool dialled = false;
    bool attached = false;
};

// Everything between the core's mixer and the host's sound card. Kept out of
// the header so nothing else has to see mGBA's internals.
struct GbaInstance::Audio {
    struct mAudioBuffer out;
    struct mAudioResampler resampler;
    // The rate the resampler was last told about. A game writing SOUNDBIAS
    // changes the core's rate underneath us, and the resampler has to be told
    // or everything after that point plays at the wrong speed.
    unsigned source_rate = 0;
};

GbaInstance::~GbaInstance() { close(); }

bool GbaInstance::open(const std::string& rom, const std::string& bios,
                       unsigned sample_rate, std::string* err) {
    close();

    const auto fail = [&](const char* what) {
        if (err) *err = what;
        close();
        return false;
    };

    core_ = GBACoreCreate();
    if (!core_) return fail("could not create a GBA core");
    if (!core_->init(core_)) return fail("core init failed");

    // The core reads its options out of this. Without it the defaults are
    // whatever zeroed memory happens to mean.
    mCoreInitConfig(core_, nullptr);

    video_.assign(static_cast<std::size_t>(kWidth) * kHeight, 0);
    core_->setVideoBuffer(core_, video_.data(), kWidth);

    if (!bios.empty()) {
        bios_vf_ = VFileOpen(bios.c_str(), O_RDONLY);
        if (!bios_vf_) return fail("cannot open the BIOS image");
        if (!core_->loadBIOS(core_, bios_vf_, 0))
            return fail("BIOS image rejected");
        bios_vf_ = nullptr;   // the core owns it now
        core_->opts.useBios = true;
    }

    // No ROM is a legitimate configuration, not a missing argument: it boots
    // the BIOS with an empty cartridge slot, which is where a GBA waits for a
    // multiboot download.
    if (!rom.empty()) {
        rom_vf_ = VFileOpen(rom.c_str(), O_RDONLY);
        if (!rom_vf_) return fail("cannot open the ROM");
        if (!core_->loadROM(core_, rom_vf_)) return fail("ROM rejected");
        rom_vf_ = nullptr;    // likewise
    } else if (bios.empty()) {
        return fail("no ROM and no BIOS — nothing to boot");
    }

    host_rate_ = sample_rate;
    audio_ = new Audio();
    mAudioBufferInit(&audio_->out, kHostBufferFrames, 2);
    mAudioResamplerInit(&audio_->resampler, mINTERPOLATOR_SINC);
    mAudioResamplerSetDestination(&audio_->resampler, &audio_->out,
                                  host_rate_);

    core_->reset(core_);
    return true;
}

void GbaInstance::close() {
    if (audio_) {
        mAudioResamplerDeinit(&audio_->resampler);
        mAudioBufferDeinit(&audio_->out);
        delete audio_;
        audio_ = nullptr;
    }
    if (core_) {
        // Unhook the driver before the core goes: the driver holds a timing
        // event scheduled against it.
        if (link_ && link_->attached) {
            struct GBA* gba = static_cast<struct GBA*>(core_->board);
            GBASIOSetDriver(&gba->sio, nullptr);
            link_->attached = false;
        }
        core_->deinit(core_);   // takes the ROM and BIOS VFiles with it
        core_ = nullptr;
    }
    if (link_) {
        GBASIODolphinDestroy(&link_->dol);
        delete link_;
        link_ = nullptr;
    }
    // Only ever set while open() is still deciding whether the core will take
    // them; past that point they belong to the core.
    if (rom_vf_) { rom_vf_->close(rom_vf_); rom_vf_ = nullptr; }
    if (bios_vf_) { bios_vf_->close(bios_vf_); bios_vf_ = nullptr; }
    video_.clear();
}

bool GbaInstance::dial(const std::string& host, uint16_t data_port,
                       uint16_t clock_port, std::string* err) {
    if (!core_) {
        if (err) *err = "no core to link";
        return false;
    }
    if (!link_) {
        link_ = new Link();
        GBASIODolphinCreate(&link_->dol);
    }
    if (link_->dialled) return true;

    struct Address addr;
    // Returns an errno-style int, so zero is success. Testing it as a bool
    // gets the answer exactly backwards.
    if (SocketResolveHost(host.c_str(), &addr) != 0) {
        if (err) *err = "cannot resolve " + host;
        return false;
    }
    // Dolphin listens and we dial out — the opposite of how a GBA link cable
    // is usually modelled, and the reason nothing works until Dolphin is
    // already running with its SI ports set to "GBA (TCP)".
    if (!GBASIODolphinConnect(&link_->dol, &addr,
                              static_cast<short>(data_port),
                              static_cast<short>(clock_port))) {
        if (err) *err = "no Dolphin listening on " + host;
        return false;
    }
    link_->dialled = true;
    return true;
}

void GbaInstance::attach() {
    if (!core_ || !link_ || !link_->dialled || link_->attached) return;
    struct GBA* gba = static_cast<struct GBA*>(core_->board);
    // Sets driver->p and then runs the driver's init, which schedules its
    // first timing event. Both need the sockets to already be live.
    GBASIOSetDriver(&gba->sio, &link_->dol.d);
    link_->attached = true;
}

void GbaInstance::shutdown_link() {
    if (!link_ || !link_->dialled) return;
    for (Socket s : {link_->dol.data, link_->dol.clock})
        if (!SOCKET_FAILED(s)) ::shutdown(s, SHUT_RDWR);
}

bool GbaInstance::link_alive() const {
    if (!link_ || !link_->attached) return false;
    if (SOCKET_FAILED(link_->dol.data)) return false;
    // Peek rather than read: the driver owns this socket and must still get
    // every byte. A zero-length result is the peer having closed; EAGAIN just
    // means nothing is waiting, which is the normal case.
    char b;
    const ssize_t n = ::recv(link_->dol.data, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return false;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    return true;
}

bool GbaInstance::joybus_active() const {
    if (!core_) return false;
    const struct GBA* gba = static_cast<const struct GBA*>(core_->board);
    return gba->sio.mode == GBA_SIO_JOYBUS;
}

int GbaInstance::sio_mode() const {
    if (!core_) return -1;
    return static_cast<int>(
        static_cast<const struct GBA*>(core_->board)->sio.mode);
}

uint16_t GbaInstance::rcnt() const {
    if (!core_) return 0;
    return static_cast<const struct GBA*>(core_->board)->sio.rcnt;
}

void GbaInstance::note_sio_mode() {
    const int m = sio_mode();
    if (m >= 0 && m < 32) modes_seen_ |= 1u << m;
}

int GbaInstance::screen_activity() const {
    if (video_.empty()) return 0;
    std::size_t lit = 0;
    // Every 37th pixel: a prime stride walks the whole frame without lining up
    // with any tile or scanline boundary, and 2.7% of the screen is plenty to
    // tell "black" from "not black".
    for (std::size_t i = 0; i < video_.size(); i += 37)
        if ((video_[i] & 0x00FFFFFFu) != 0) ++lit;
    return static_cast<int>(100 * lit / (video_.size() / 37 + 1));
}

bool GbaInstance::linked() const {
    return link_ && link_->attached &&
           GBASIODolphinIsConnected(&link_->dol);
}

void GbaInstance::run_frame() {
    if (!core_) return;
    core_->runFrame(core_);
}

void GbaInstance::set_keys(uint16_t keyinput) {
    if (!core_) return;
    // KEYINPUT is active low and mGBA's key mask is active high, over the same
    // ten bits in the same order.
    core_->setKeys(core_, static_cast<uint32_t>(~keyinput) & 0x03FF);
}

unsigned GbaInstance::core_sample_rate() const {
    return core_ ? core_->audioSampleRate(core_) : 0;
}

std::size_t GbaInstance::drain_audio(int16_t* out, std::size_t frames) {
    if (!core_ || !audio_ || frames == 0) return 0;

    const unsigned rate = core_->audioSampleRate(core_);
    if (rate && rate != audio_->source_rate) {
        // First call, or the guest moved SOUNDBIAS. Re-point the resampler at
        // the same buffer with the rate it is actually filling it at.
        audio_->source_rate = rate;
        mAudioResamplerSetSource(&audio_->resampler,
                                 core_->getAudioBuffer(core_), rate, true);
    }
    mAudioResamplerProcess(&audio_->resampler);

    // mAudioBuffer counts stereo frames, not samples: it multiplies by the
    // channel count itself, at both ends. Asking for `frames * 2` here writes
    // twice the caller's buffer, and halving the result throws away half the
    // audio that was drained — which starves the sound card into an underrun
    // roughly twice a second and sounds like it.
    return mAudioBufferRead(&audio_->out, out, frames);
}

}  // namespace gql

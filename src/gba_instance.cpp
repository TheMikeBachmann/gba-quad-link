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
#include <mgba/internal/gba/video.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/dolphin.h>
#include <mgba/internal/gba/sio/lockstep.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "cable.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstddef>

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
int g_only_category = -1;

void log_to_stderr(struct mLogger*, int category, enum mLogLevel level,
                   const char* format, va_list args) {
    if (!(level & g_log_levels)) return;
    if (g_only_category >= 0 && category != g_only_category) return;
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

// Open a ROM, reaching inside a .7z or .zip if that is what it is.
//
// Real ROM libraries are archives — every one of the nine hundred on the
// machine this was written for is a .7z — so a program that only opens bare
// .gba files is a program with nothing to open. mGBA vendors the LZMA SDK, so
// this costs a dependency on nothing.
//
// The ROM is copied out rather than used in place: a file inside an archive
// only lives as long as the archive is open, and the core keeps its ROM for
// the whole session.
VFile* open_rom(const std::string& path, struct mCore* core) {
    VDir* archive = VDirOpenArchive(path.c_str());
    if (!archive) return VFileOpen(path.c_str(), O_RDONLY);   // a plain file

    VFile* found = nullptr;
    archive->rewind(archive);
    while (struct VDirEntry* de = archive->listNext(archive)) {
        if (de->type(de) != VFS_FILE) continue;
        VFile* vf = archive->openFile(archive, de->name(de), O_RDONLY);
        if (!vf) continue;
        // Ask the core rather than trusting the extension: archives from a ROM
        // set carry readmes and patches alongside the cartridge.
        if (core->isROM(vf)) {
            const ssize_t n = vf->size(vf);
            if (n > 0) {
                std::vector<uint8_t> buf(static_cast<std::size_t>(n));
                vf->seek(vf, 0, SEEK_SET);   // isROM left it wherever it liked
                if (vf->read(vf, buf.data(), static_cast<std::size_t>(n)) == n)
                    found = VFileMemChunk(buf.data(),
                                          static_cast<std::size_t>(n));
            }
        }
        vf->close(vf);
        if (found) break;
    }
    archive->close(archive);
    return found;
}

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

void log_only_sio() {
    g_only_category = mLogCategoryById("gba.sio");
    g_log_levels = mLOG_ALL;
    mLogSetDefaultLogger(&g_logger);
}

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

// One machine's end of the cable: mGBA's driver, plus the sleep/wake plumbing
// the coordinator drives it with.
//
// mGBA's own implementation of this defers to mCoreThread, whose "sleep" posts
// a request that the thread honours at its next opportunity rather than
// blocking on the spot. Ours has to behave the same way, and can: the
// coordinator forces the sleeping core's CPU out of its run loop immediately
// afterwards, so run_frame() returns and the waiting happens in cable_wait().
// Blocking inside this callback instead would deadlock, because it is called
// while the coordinator's own mutex is held.
struct GbaInstance::Cable {
    struct GBASIOLockstepDriver driver;
    struct mLockstepUser user;
    CableGroup* group = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    // Read on the hot path between CPU chunks, so an atomic rather than
    // something needing the mutex; the mutex and the condition variable are
    // only for the waiting itself.
    std::atomic<bool> asleep{false};
    unsigned long sleeps = 0;
    std::atomic<unsigned long> timeouts{0};

    // Where this machine actually sits on the cable, which is not its
    // quadrant: the coordinator packs whoever is attached into positions from
    // zero with no gaps, and renumbers on every join and departure. Kept
    // current by the callback rather than polled, because the moment it
    // changes is the moment a different machine has to start carrying the
    // pace, and a second of nobody doing that is a second of everyone
    // running as fast as they can.
    std::atomic<int> player_id{-1};
    bool leaving = false;      // shutting down; never sleep again
    int preferred_id = -1;
    bool attached = false;
};

namespace {

GbaInstance::Cable* cable_of(struct mLockstepUser* user) {
    // `user` is the member, so step back to the object holding it.
    return reinterpret_cast<GbaInstance::Cable*>(
        reinterpret_cast<char*>(user) - offsetof(GbaInstance::Cable, user));
}

void cable_sleep(struct mLockstepUser* user) {
    GbaInstance::Cable* c = cable_of(user);
    std::lock_guard<std::mutex> lk(c->mutex);
    c->asleep.store(true, std::memory_order_relaxed);
    ++c->sleeps;
}

void cable_wake(struct mLockstepUser* user) {
    GbaInstance::Cable* c = cable_of(user);
    {
        std::lock_guard<std::mutex> lk(c->mutex);
        c->asleep.store(false, std::memory_order_relaxed);
    }
    c->cv.notify_all();
}

int cable_requested_id(struct mLockstepUser* user) {
    return cable_of(user)->preferred_id;
}

void cable_player_id_changed(struct mLockstepUser* user, int id) {
    cable_of(user)->player_id.store(id, std::memory_order_relaxed);
}

}  // namespace

void GbaInstance::attach_cable(CableGroup* group, int preferred_id) {
    if (!core_ || !group || cable_ || (link_ && link_->attached)) return;

    cable_ = new Cable();
    cable_->group = group;
    cable_->preferred_id = preferred_id;
    memset(&cable_->user, 0, sizeof(cable_->user));
    cable_->user.sleep = cable_sleep;
    cable_->user.wake = cable_wake;
    cable_->user.requestedId = cable_requested_id;
    cable_->user.playerIdChanged = cable_player_id_changed;

    GBASIOLockstepDriverCreate(&cable_->driver, &cable_->user);
    GBASIOLockstepCoordinatorAttach(
        static_cast<struct GBASIOLockstepCoordinator*>(group->raw()),
        &cable_->driver);

    struct GBA* gba = static_cast<struct GBA*>(core_->board);
    GBASIOSetDriver(&gba->sio, &cable_->driver.d);
    cable_->attached = true;
}

void GbaInstance::cable_wait() {
    if (!cable_) return;
    std::unique_lock<std::mutex> lk(cable_->mutex);

    // Bounded, because only a running machine can wake a sleeping one, so a
    // cable on which everybody is asleep stays that way for good. mGBA guards
    // that state with an assertion which does nothing in a release build, and
    // it is reachable: hand a machine a different cartridge while it is
    // suspended and the whole cable can settle into it.
    //
    // Waiting forever turns a moment's confusion into a locked-up window.
    // Giving up and running turns it into a hitch, and into exactly what a
    // real cable does when one console stops answering — the others carry on
    // and the game notices its data is stale. The previous project reached the
    // same conclusion about its own cable for the same reason.
    const bool woken = cable_->cv.wait_for(
        lk, std::chrono::milliseconds(50), [this] {
            return !cable_->asleep.load(std::memory_order_relaxed) ||
                   cable_->leaving;
        });
    if (!woken) {
        // Clear it locally or run_frame will hand control straight back and
        // spin. The coordinator will suspend this machine again if it still
        // wants to.
        cable_->asleep.store(false, std::memory_order_relaxed);
        cable_->timeouts.fetch_add(1, std::memory_order_relaxed);
    }
}

void GbaInstance::wake_cable() {
    if (!cable_) return;
    {
        std::lock_guard<std::mutex> lk(cable_->mutex);
        cable_->leaving = true;
        cable_->asleep.store(false, std::memory_order_relaxed);
    }
    cable_->cv.notify_all();
}

bool GbaInstance::on_cable() const { return cable_ && cable_->attached; }

unsigned GbaInstance::cable_id() const {
    return cable_ ? cable_->driver.lockstepId : 0;
}

int GbaInstance::cable_devices() const {
    if (!cable_ || !cable_->attached) return -1;
    struct GBASIODriver* d = &cable_->driver.d;
    return d->connectedDevices ? d->connectedDevices(d) : -1;
}

int GbaInstance::cable_player_id() const {
    // From the callback, not by reaching into the coordinator's table, which
    // another machine's thread may be rearranging as we read it.
    return cable_ ? cable_->player_id.load(std::memory_order_relaxed) : -1;
}

unsigned long GbaInstance::cable_sleeps() const {
    return cable_ ? cable_->sleeps : 0;
}

unsigned long GbaInstance::cable_timeouts() const {
    return cable_ ? cable_->timeouts.load(std::memory_order_relaxed) : 0;
}

GbaInstance::~GbaInstance() { close(); }

bool GbaInstance::open(const std::string& rom, const std::string& bios,
                       const std::string& save, unsigned sample_rate,
                       std::string* err) {
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
        rom_vf_ = open_rom(rom, core_);
        if (!rom_vf_)
            return fail("cannot open the ROM, or the archive holds no ROM");
        if (!core_->loadROM(core_, rom_vf_)) return fail("ROM rejected");
        rom_vf_ = nullptr;    // likewise
    } else if (bios.empty()) {
        return fail("no ROM and no BIOS — nothing to boot");
    }

    // After the ROM, because mGBA works out the save's type and size from the
    // cartridge header and has nothing to go on before that.
    if (!rom.empty() && !save.empty()) {
        save_vf_ = VFileOpen(save.c_str(), O_CREAT | O_RDWR);
        if (!save_vf_) {
            // Not fatal. A game that cannot write its save is still a game;
            // one that refuses to start is not.
            std::fprintf(stderr, "save: cannot open %s — progress will not be "
                                 "kept\n", save.c_str());
        } else if (!core_->loadSave(core_, save_vf_)) {
            std::fprintf(stderr, "save: %s rejected — progress will not be "
                                 "kept\n", save.c_str());
            save_vf_ = nullptr;   // the core took it and did not want it
        } else {
            save_vf_ = nullptr;   // the core owns it now and flushes on deinit
        }
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
        if (cable_ && cable_->attached) {
            struct GBA* gba = static_cast<struct GBA*>(core_->board);
            GBASIOSetDriver(&gba->sio, nullptr);
            GBASIOLockstepCoordinatorDetach(
                static_cast<struct GBASIOLockstepCoordinator*>(
                    cable_->group->raw()),
                &cable_->driver);
            cable_->attached = false;
        }
        core_->deinit(core_);   // takes the ROM and BIOS VFiles with it
        core_ = nullptr;
    }
    if (cable_) {
        wake_cable();
        delete cable_;
        cable_ = nullptr;
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
    if (save_vf_) { save_vf_->close(save_vf_); save_vf_ = nullptr; }
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

bool GbaInstance::link_starved() const {
    if (!link_ || !link_->attached) return false;
    // Ten frames of unreplenished budget. A healthy link never approaches
    // this; a silent one passes it almost immediately and keeps going.
    const int32_t kStarved = -(VIDEO_TOTAL_LENGTH * 10);
    return link_->dol.clockSlice < kStarved;
}

bool GbaInstance::linked() const {
    return link_ && link_->attached &&
           GBASIODolphinIsConnected(&link_->dol);
}

bool GbaInstance::run_frame() {
    if (!core_) return false;
    const uint32_t start = core_->frameCounter(core_);

    // The same safety bound mCore::runFrame uses: a core that is halted, or
    // waiting on a link that has gone quiet, will not finish a frame, and this
    // must still come back so the caller can notice.
    struct GBA* gba = static_cast<struct GBA*>(core_->board);
    const int32_t begin = mTimingCurrentTime(&gba->timing);
    const int32_t bound = VIDEO_TOTAL_LENGTH + VIDEO_HORIZONTAL_LENGTH;

    while (core_->frameCounter(core_) == start &&
           mTimingCurrentTime(&gba->timing) - begin < bound) {
        core_->runLoop(core_);
        // Suspended mid-frame. Hand control back so the caller can wait; the
        // frame is finished later, from wherever it got to.
        if (cable_ && cable_->asleep.load(std::memory_order_relaxed)) break;
    }
    return core_->frameCounter(core_) != start;
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

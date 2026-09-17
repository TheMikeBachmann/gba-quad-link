/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// audio.h — several guests, one pair of speakers.
//
// The previous project heard exactly one machine and threw the other three
// away, on the grounds that four independent races mixed together is noise.
// That is true of four independent races. It is not true of two people playing
// the same game, or of one person wanting their own machine louder than the
// others, and deciding for everybody is worse than letting them choose.
//
// So each machine gets a ring buffer it fills from its own thread, and the
// host thread mixes whatever is switched on. A buffer rather than a direct
// hand-off because the four threads run at four slightly different moments and
// none of them may wait for the others — a machine linked to a GameCube is
// already being told when it may run, and being told again by the sound card
// would be one master too many.

#pragma once

#include <SDL2/SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gql {

constexpr int kMixSources = 4;

class AudioMixer {
public:
    // `rate` is the sound card's, which is fixed; see GbaInstance::drain_audio
    // for why the guests' rates are not.
    bool open(int rate);
    void close();
    bool ok() const { return dev_ != 0; }

    // From a machine's own thread. Interleaved stereo. Never blocks, and
    // drops its oldest audio rather than wait for anyone.
    void push(int source, const int16_t* frames, std::size_t count);

    // From the host thread, once a frame. Mixes what is switched on and hands
    // it to the sound card.
    void pump();

    void set_enabled(int source, bool on);
    bool enabled(int source) const;
    // 0.0 to 2.0; above one is real amplification and will clip a loud guest.
    void set_gain(int source, float gain);
    float gain(int source) const;

    unsigned long underruns() const { return underruns_.load(); }
    unsigned long drops() const { return drops_.load(); }

private:
    struct Source {
        std::mutex mutex;
        std::vector<int16_t> ring;   // interleaved stereo, frames * 2
        std::size_t head = 0;        // next frame to read
        std::size_t used = 0;        // frames held
        std::atomic<bool> on{false};
        std::atomic<float> gain{1.0f};
    };

    std::size_t available(Source& s);
    std::size_t take(Source& s, int16_t* out, std::size_t frames);

    SDL_AudioDeviceID dev_ = 0;
    int rate_ = 48000;
    bool started_ = false;           // host thread only
    Source src_[kMixSources];
    std::vector<int16_t> mix_;
    std::vector<int16_t> scratch_;
    std::atomic<unsigned long> underruns_{0};
    std::atomic<unsigned long> drops_{0};
};

}  // namespace gql

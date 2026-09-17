/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "audio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gql {
namespace {

// A third of a second per machine. Enough to ride out a machine hitching or
// being held by Dolphin, small enough that the audio never lags noticeably
// behind the picture.
constexpr std::size_t kRingFrames = 16384;

// Enough cushion to survive a scheduling hiccup without a gap.
constexpr Uint32 kPrerollBytes = 24000;   // 125 ms of 48kHz stereo S16
// Past this the queue is lagging audibly; stop feeding rather than let it grow.
constexpr Uint32 kCeilingBytes = 76800;   // 400 ms

}  // namespace

bool AudioMixer::open(int rate) {
    SDL_AudioSpec want{};
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioSpec got{};
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (!dev_) return false;
    rate_ = got.freq;
    for (Source& s : src_) s.ring.assign(kRingFrames * 2, 0);
    mix_.assign(4096 * 2, 0);
    scratch_.assign(4096 * 2, 0);
    return true;
}

void AudioMixer::close() {
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
}

void AudioMixer::set_enabled(int i, bool on) {
    if (i < 0 || i >= kMixSources) return;
    Source& s = src_[i];
    if (on && !s.on.load(std::memory_order_relaxed)) {
        // Switching a machine on starts it from now. Whatever is left in its
        // ring is from whenever it was last heard and would play as a burst of
        // something that already happened.
        std::lock_guard<std::mutex> lk(s.mutex);
        s.head = 0;
        s.used = 0;
    }
    s.on.store(on, std::memory_order_relaxed);
}

bool AudioMixer::enabled(int i) const {
    return i >= 0 && i < kMixSources &&
           src_[i].on.load(std::memory_order_relaxed);
}

void AudioMixer::set_gain(int i, float g) {
    if (i >= 0 && i < kMixSources)
        src_[i].gain.store(std::clamp(g, 0.0f, 2.0f), std::memory_order_relaxed);
}

float AudioMixer::gain(int i) const {
    return i >= 0 && i < kMixSources
               ? src_[i].gain.load(std::memory_order_relaxed)
               : 0.0f;
}

void AudioMixer::push(int i, const int16_t* frames, std::size_t count) {
    if (i < 0 || i >= kMixSources || !frames || count == 0) return;
    Source& s = src_[i];
    // Nobody is listening to this one. Its core still has to be drained, which
    // its own thread does regardless, but there is no reason to copy the
    // result into a ring that will only overflow — and counting those
    // overflows as dropped audio made the figure meaningless: three silent
    // machines produced two million of them in fifteen seconds.
    if (!s.on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.ring.empty()) return;

    for (std::size_t n = 0; n < count; ++n) {
        const std::size_t slot = (s.head + s.used) % kRingFrames;
        s.ring[slot * 2] = frames[n * 2];
        s.ring[slot * 2 + 1] = frames[n * 2 + 1];
        if (s.used < kRingFrames) {
            ++s.used;
        } else {
            // Full. The oldest audio goes, not the newest: a machine that has
            // run ahead should be heard where it is now, not a third of a
            // second ago. Waiting is not an option — this is a guest's own
            // thread and something else may already be waiting on it.
            s.head = (s.head + 1) % kRingFrames;
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::size_t AudioMixer::available(Source& s) {
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.used;
}

std::size_t AudioMixer::take(Source& s, int16_t* out, std::size_t frames) {
    std::lock_guard<std::mutex> lk(s.mutex);
    const std::size_t n = std::min(frames, s.used);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t slot = (s.head + i) % kRingFrames;
        out[i * 2] = s.ring[slot * 2];
        out[i * 2 + 1] = s.ring[slot * 2 + 1];
    }
    s.head = (s.head + n) % kRingFrames;
    s.used -= n;
    return n;
}

void AudioMixer::pump() {
    if (!dev_) return;

    const Uint32 queued = SDL_GetQueuedAudioSize(dev_);
    if (started_ && queued == 0) {
        // Everything switched off, or every machine stalled at once. Rebuild
        // the cushion rather than restart the device on one small block,
        // which crackles.
        started_ = false;
        underruns_.fetch_add(1, std::memory_order_relaxed);
        SDL_PauseAudioDevice(dev_, 1);
    }
    // Whether there is room for more. Note that this does not stop the rings
    // being drained below: leaving them alone while the queue is long means
    // they fill and overflow, and what they then hold is old. Better to take
    // the audio and throw it away knowingly than to leave stale audio sitting
    // in a buffer waiting to be played late.
    const bool room = queued <= kCeilingBytes;

    // However much the least-supplied machine has — not the most.
    //
    // Taking the largest and padding the others to match sounds exactly as bad
    // as it should: the output then runs at the rate of whichever machine is
    // furthest ahead, every slower one is zero-filled to keep up, and those
    // fills are audible as chopping. The surplus is then thrown away at the
    // queue ceiling, so the figure to watch is the drop count: four machines
    // mixed that way dropped half a million blocks in forty seconds where one
    // machine dropped none.
    //
    // A machine that has genuinely stopped is excluded instead of allowed to
    // hold up the rest, which is the case padding was reaching for.
    std::size_t have[kMixSources];
    std::size_t most = 0;
    for (int i = 0; i < kMixSources; ++i) {
        have[i] = src_[i].on.load(std::memory_order_relaxed)
                      ? available(src_[i]) : 0;
        most = std::max(most, have[i]);
    }
    // Nothing at all while somebody else has a quarter second banked is a
    // machine that has stopped, not one that is merely a little behind.
    const std::size_t kStalled = 12000;
    std::size_t want = mix_.size() / 2;
    bool any_live = false;
    for (int i = 0; i < kMixSources; ++i) {
        if (!src_[i].on.load(std::memory_order_relaxed)) continue;
        live_[i] = !(have[i] == 0 && most > kStalled);
        if (!live_[i]) continue;
        want = std::min(want, have[i]);
        any_live = true;
    }
    if (!any_live || want == 0) return;

    std::fill(mix_.begin(), mix_.begin() + want * 2, 0);
    bool any = false;
    for (int i = 0; i < kMixSources; ++i) {
        Source& s = src_[i];
        if (!s.on.load(std::memory_order_relaxed) || !live_[i]) continue;
        const std::size_t got = take(s, scratch_.data(), want);
        if (got == 0) continue;
        any = true;
        const float g = s.gain.load(std::memory_order_relaxed);
        for (std::size_t n = 0; n < got * 2; ++n) {
            // Accumulated wide and clamped once at the end. Clamping each
            // machine as it is added would distort a quiet one for being in
            // the company of a loud one.
            const int32_t acc = static_cast<int32_t>(mix_[n]) +
                                static_cast<int32_t>(scratch_[n] * g);
            mix_[n] = static_cast<int16_t>(std::clamp(acc, -32768, 32767));
        }
    }
    if (!any) return;
    if (!room) {
        // Drained and discarded. The guests are ahead of the sound card, which
        // over a long session they will drift into being; dropping the surplus
        // here keeps the lag from growing without bound.
        drops_.fetch_add(want, std::memory_order_relaxed);
        return;
    }

    SDL_QueueAudio(dev_, mix_.data(),
                   static_cast<Uint32>(want * 2 * sizeof(int16_t)));
    if (!started_ && SDL_GetQueuedAudioSize(dev_) >= kPrerollBytes) {
        started_ = true;
        SDL_PauseAudioDevice(dev_, 0);
    }
}

}  // namespace gql

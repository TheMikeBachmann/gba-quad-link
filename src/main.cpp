/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// main.cpp — milestone one: a single libmgba core in a window, with a pad.
//
// Deliberately one core and not four. The point of this step is to settle the
// things that have nothing to do with networking — that the core builds and
// boots, that the picture is the right way round, that audio comes out at a
// sane speed, that a controller reaches the guest — before the Dolphin socket
// arrives and every one of those becomes hard to tell apart from a link fault.
//
// The shape is already the four-machine one, though, because none of it gets
// easier later: the core runs on its own thread and latches finished frames
// into a locked buffer, and the host thread draws whatever is in that buffer
// at its own rate. That indirection looks like overkill for one core. It is
// what keeps the window alive when Dolphin stops granting cycles and a core
// thread sits inside run_frame() for half a second.

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "controls.h"
#include "gba_instance.h"
#include "paths.h"

namespace {

using gql::GbaInstance;

// What the sound card is opened at. The core's own rate is a moving target —
// see the note on GbaInstance::drain_audio — so the host end is pinned and the
// resampler meets it.
constexpr int kHostSampleRate = 48000;

std::atomic<bool> g_quit{false};

// The one place the core thread and the host thread meet.
struct Screen {
    std::mutex mutex;
    std::vector<uint32_t> pixels;   // ABGR8888, kWidth * kHeight
    bool has_frame = false;
    std::atomic<uint64_t> frames{0};
};

// Player one's sound on its way to the speakers, carried over from the
// four-machine build. The producer is the core thread, paced to the console's
// refresh, so samples arrive at close to real time and SDL's own queue is
// enough — but "close" drifts over a long session, so it is watched from both
// ends: refill the cushion on an underrun rather than restarting the device on
// one small block, and drop rather than let the lag grow without bound.
struct AudioOut {
    SDL_AudioDeviceID dev = 0;
    bool started = false;           // producer thread only
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> dropped{0};

    // Bytes, at 48000 Hz stereo S16 — four bytes a frame.
    static constexpr Uint32 kPrerollBytes = 24000;    // 125 ms
    static constexpr Uint32 kCeilingBytes = 76800;    // 400 ms

    void push(const int16_t* samples, std::size_t frames) {
        if (!dev || frames == 0) return;
        const Uint32 queued = SDL_GetQueuedAudioSize(dev);
        if (started && queued == 0) {
            started = false;
            underruns.fetch_add(1, std::memory_order_relaxed);
            SDL_PauseAudioDevice(dev, 1);
        }
        if (queued > kCeilingBytes) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        SDL_QueueAudio(dev, samples,
                       static_cast<Uint32>(frames * 2 * sizeof(int16_t)));
        if (!started && SDL_GetQueuedAudioSize(dev) >= kPrerollBytes) {
            started = true;
            SDL_PauseAudioDevice(dev, 0);
        }
    }
};

// Runs the core to completion on this thread. The instance is created here
// rather than handed in: it belongs to whoever drives it.
void core_thread(const std::string& rom, const std::string& bios,
                 Screen* screen, AudioOut* audio,
                 std::atomic<uint16_t>* keys) {
    gql::set_log_player(0);
    GbaInstance gba;
    std::string err;
    if (!gba.open(rom, bios, kHostSampleRate, &err)) {
        std::fprintf(stderr, "core: %s\n", err.c_str());
        g_quit.store(true);
        return;
    }
    std::printf("core: booted %s, mixing at %u Hz into %d Hz\n",
                rom.empty() ? "the BIOS with no cartridge" : rom.c_str(),
                gba.core_sample_rate(), kHostSampleRate);
    std::fflush(stdout);

    std::vector<int16_t> sink(4096 * 2);
    const auto started = std::chrono::steady_clock::now();
    uint64_t frame = 0;

    // Self-paced, for now. This is the line that inverts once the clock socket
    // is attached: Dolphin grants cycles and the core waits for them inside
    // run_frame(), and pacing here would fight it.
    constexpr double kFrameSeconds = 1.0 / 59.7275;

    while (!g_quit.load(std::memory_order_relaxed)) {
        gba.set_keys(keys->load(std::memory_order_relaxed));
        gba.run_frame();
        ++frame;

        const std::size_t got = gba.drain_audio(sink.data(), sink.size() / 2);
        if (audio) audio->push(sink.data(), got);

        {
            std::lock_guard<std::mutex> lk(screen->mutex);
            std::memcpy(screen->pixels.data(), gba.pixels(),
                        screen->pixels.size() * sizeof(uint32_t));
            screen->has_frame = true;
        }
        screen->frames.store(frame, std::memory_order_relaxed);

        std::this_thread::sleep_until(
            started + std::chrono::duration_cast<
                          std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(kFrameSeconds * frame)));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string rom_path = "roms/mario_kart_super_circuit_usa.gba";
    std::string bios_path;          // optional while a ROM boots on its own
    std::string cfg_path;
    int scale = 3;
    bool fullscreen = false;
    bool integer_scale = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--rom") rom_path = next();
        else if (a == "--no-rom") rom_path.clear();
        else if (a == "--bios") bios_path = next();
        else if (a == "--controls") cfg_path = next();
        else if (a == "--scale") scale = std::atoi(next());
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--integer-scale") integer_scale = true;
        else if (a == "--verbose") verbose = true;
        else {
            std::fprintf(stderr,
                "usage: %s [--rom P | --no-rom] [--bios P] [--controls P]\n"
                "          [--scale N] [--fullscreen] [--integer-scale] [--verbose]\n",
                argv[0]);
            return 2;
        }
    }
    if (scale < 1) scale = 1;
    gql::install_logger(verbose);

    const std::string settings_dir = gql::config_dir();
    if (cfg_path.empty()) {
        cfg_path = settings_dir.empty()
                       ? std::string("controls.cfg")
                       : settings_dir + "/controls.cfg";
    }

    if (!rom_path.empty()) rom_path = gql::find_asset(rom_path);
    if (!bios_path.empty()) bios_path = gql::find_asset(bios_path);

    gql::Controls controls;
    controls.reset_to_defaults();
    if (gql::load_controls(cfg_path, &controls))
        std::printf("controls loaded from %s\n", cfg_path.c_str());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)
            != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    AudioOut audio;
    {
        SDL_AudioSpec want{};
        want.freq     = kHostSampleRate;
        want.format   = AUDIO_S16SYS;
        want.channels = 2;
        want.samples  = 1024;
        SDL_AudioSpec got{};
        audio.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (!audio.dev)
            std::fprintf(stderr, "audio: %s — running silent\n",
                         SDL_GetError());
        else
            std::printf("audio: %d Hz, %d channels\n", got.freq, got.channels);
    }

    // One pad for now, claimed by whichever shows up first. The four-way
    // assignment model comes across with the compositor.
    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks() && !pad; ++i) {
        if (!SDL_IsGameController(i)) continue;
        pad = SDL_GameControllerOpen(i);
        if (pad)
            std::printf("controller: %s\n",
                        SDL_GameControllerName(pad) ? SDL_GameControllerName(pad)
                                                    : "unknown");
    }
    if (!pad) std::printf("no controller — the keyboard drives player 1\n");

    SDL_Window* win = SDL_CreateWindow(
        "gba-quad-link", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        GbaInstance::kWidth * scale, GbaInstance::kHeight * scale,
        SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u));
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    // ABGR8888 is what a 32-bit mColor is on this platform; it is also what
    // mGBA's own SDL frontend asks for.
    SDL_Texture* tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
        GbaInstance::kWidth, GbaInstance::kHeight);
    SDL_SetTextureScaleMode(tex, integer_scale ? SDL_ScaleModeNearest
                                               : SDL_ScaleModeLinear);

    Screen screen;
    screen.pixels.assign(
        static_cast<std::size_t>(GbaInstance::kWidth) * GbaInstance::kHeight, 0);
    std::atomic<uint16_t> keys{0x03FF};

    std::thread core(core_thread, rom_path, bios_path, &screen, &audio, &keys);

    auto last_report = std::chrono::steady_clock::now();
    while (!g_quit.load(std::memory_order_relaxed)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g_quit.store(true);
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad)
                pad = SDL_GameControllerOpen(ev.cdevice.which);
            if (ev.type == SDL_KEYDOWN &&
                ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                g_quit.store(true);
        }

        keys.store(controls.read(0, pad, SDL_GetKeyboardState(nullptr)),
                   std::memory_order_relaxed);

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(ren, &win_w, &win_h);
        int draw_w, draw_h;
        if (integer_scale) {
            const int units = std::max(1, std::min(win_w / GbaInstance::kWidth,
                                                   win_h / GbaInstance::kHeight));
            draw_w = GbaInstance::kWidth * units;
            draw_h = GbaInstance::kHeight * units;
        } else {
            const double fit = std::min(
                static_cast<double>(win_w) / GbaInstance::kWidth,
                static_cast<double>(win_h) / GbaInstance::kHeight);
            draw_w = static_cast<int>(GbaInstance::kWidth * fit);
            draw_h = static_cast<int>(GbaInstance::kHeight * fit);
        }
        SDL_Rect dst{(win_w - draw_w) / 2, (win_h - draw_h) / 2, draw_w, draw_h};

        {
            std::lock_guard<std::mutex> lk(screen.mutex);
            if (screen.has_frame)
                SDL_UpdateTexture(tex, nullptr, screen.pixels.data(),
                                  GbaInstance::kWidth * sizeof(uint32_t));
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, &dst);
        SDL_RenderPresent(ren);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(5)) {
            std::printf("frames: %llu   audio: %llu underruns, %llu drops\n",
                        (unsigned long long)screen.frames.load(),
                        (unsigned long long)audio.underruns.load(),
                        (unsigned long long)audio.dropped.load());
            std::fflush(stdout);
            last_report = now;
        }
    }

    g_quit.store(true);
    if (core.joinable()) core.join();

    SDL_DestroyTexture(tex);
    if (pad) SDL_GameControllerClose(pad);
    if (audio.dev) SDL_CloseAudioDevice(audio.dev);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// main.cpp — four Game Boy Advances in one window, linked to Dolphin.
//
// Each machine runs on its own thread and owns its core outright. The host
// thread polls input, copies finished frames out under a lock, composites, and
// draws — at its own rate, which is not any of the guests' rates and must not
// become one. That separation is the whole point: Dolphin decides when a core
// may advance, and a core with nothing granted sits inside run_frame() for up
// to half a second at a time. The window has to stay alive through that, and
// the other three have to keep running.

#include <SDL2/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "controls.h"
#include "gba_instance.h"
#include "layout.h"
#include "machine.h"
#include "paths.h"
#include "romlist.h"

namespace {

using gql::GbaInstance;
using gql::LinkState;
using gql::Machine;
using gql::kButtons;
using gql::kMaxPlayers;

// The sound card's rate. The guests' rate is their own business and changes
// under them; see the note on GbaInstance::drain_audio.
constexpr int kHostSampleRate = 48000;

std::atomic<bool> g_quit{false};

// Input is withheld while a binding is being captured, so the button pressed
// to bind cannot also reach a guest. The guests keep *running*, unlike in the
// previous project which paused them: a paused core stops answering Dolphin,
// and Dolphin blocks on the thread it emulates the GameCube on until it does,
// so pausing here would freeze the game for everyone.
std::atomic<bool> g_input_held{false};

struct AudioOut {
    SDL_AudioDeviceID dev = 0;
    bool started = false;           // producer thread only
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> dropped{0};

    // Bytes at 48000 Hz stereo S16 — four per frame.
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

// Runs one machine until told to stop. The instance was opened and dialled by
// the host thread; from here on it belongs to this one.
void machine_thread(Machine* me, AudioOut* audio) {
    gql::set_log_player(me->index);

    // Must happen here, not on the host thread: attaching schedules the
    // driver's first event against this core's timing.
    if (me->dialled) {
        me->gba.attach();
        me->link.store(LinkState::Waiting, std::memory_order_relaxed);
    }

    std::vector<int16_t> sink(4096 * 2);
    uint64_t frame = 0;
    bool linked = me->dialled;

    // A ceiling for when nothing else sets the pace, and only then. While the
    // link is healthy Dolphin blocks until this guest answers, so sleeping
    // here is time the GameCube spends stopped — see the long note where the
    // ceiling is applied.
    constexpr double kFrameSeconds = 1.0 / 59.7275;
    const auto period = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(kFrameSeconds));
    auto deadline = std::chrono::steady_clock::now();
    auto fps_mark = std::chrono::steady_clock::now();
    uint64_t fps_frame = 0;

    // A guest waiting to be given a program cycles through serial modes
    // looking for a host, so whether a once-a-frame sample catches it in JOY
    // bus is chance. Reporting that raw makes the status column flicker
    // several times a second between "waiting" and "linked", which is
    // accurate and useless. Latch the last time it was seen there instead.
    auto joybus_mark = std::chrono::steady_clock::time_point{};
    constexpr auto kJoybusHold = std::chrono::milliseconds(750);

    while (!g_quit.load(std::memory_order_relaxed) &&
           !me->stop.load(std::memory_order_relaxed)) {
        me->gba.set_keys(g_input_held.load(std::memory_order_relaxed)
                             ? 0x03FF
                             : me->keys.load(std::memory_order_relaxed));
        me->gba.run_frame();
        me->gba.note_sio_mode();
        ++frame;

        const std::size_t got = me->gba.drain_audio(sink.data(),
                                                    sink.size() / 2);
        // Every machine's mixer is drained whether or not anyone is listening;
        // only the one holding the speakers passes the samples on.
        if (audio) audio->push(sink.data(), got);

        {
            std::lock_guard<std::mutex> lk(me->fb_mutex);
            std::memcpy(me->pixels.data(), me->gba.pixels(),
                        me->pixels.size() * sizeof(uint32_t));
            me->has_frame = true;
        }
        me->frames.store(frame, std::memory_order_relaxed);

        if (linked) {
            if (!me->gba.link_alive()) {
                linked = false;
                me->link.store(LinkState::Lost, std::memory_order_relaxed);
                // Quietly if we are the ones closing the sockets. Shutdown
                // breaks every link on purpose, and four machines each
                // announcing it reads like a fault at exactly the moment
                // nothing is wrong.
                if (!g_quit.load(std::memory_order_relaxed)) {
                    std::printf("p%d link: Dolphin went away\n", me->index + 1);
                    std::fflush(stdout);
                }
            } else {
                const auto t = std::chrono::steady_clock::now();
                if (me->gba.joybus_active()) joybus_mark = t;
                me->link.store(t - joybus_mark < kJoybusHold
                                   ? LinkState::Linked
                                   : LinkState::Waiting,
                               std::memory_order_relaxed);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - fps_mark >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(
                now - fps_mark).count();
            me->fps.store((frame - fps_frame) / secs,
                          std::memory_order_relaxed);
            fps_mark = now;
            fps_frame = frame;
        }

        if (linked) {
            deadline = now;
        } else {
            deadline += period;
            if (deadline < now) deadline = now;   // never bank credit
            std::this_thread::sleep_until(deadline);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    int players = kMaxPlayers;
    std::string rom_path;           // default for every machine
    std::string bios_path = "bios/gba_bios.bin";
    std::string cfg_path;
    std::string host;               // empty: run unlinked
    std::string rom_dir_arg;        // empty: look in the usual places
    uint16_t data_port = 54970, clock_port = 49420;
    int scale = 2;
    bool fullscreen = false, integer_scale = false, verbose = false;
    int audio_player = 0;

    // --player N opens a section: flags after it apply to that machine alone,
    // until the next --player. Before any section they set the default for
    // every machine. The setup screen is the real interface; this is for
    // getting at it from a script.
    std::string per_rom[kMaxPlayers];
    int section = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--players") players = std::atoi(next());
        else if (a == "--player") {
            section = std::atoi(next()) - 1;
            if (section < 0 || section >= kMaxPlayers) {
                std::fprintf(stderr, "--player takes 1-%d\n", kMaxPlayers);
                return 2;
            }
        }
        else if (a == "--rom") {
            if (section >= 0) per_rom[section] = next();
            else rom_path = next();
        }
        else if (a == "--bios") bios_path = next();
        else if (a == "--controls") cfg_path = next();
        else if (a == "--host") host = next();
        else if (a == "--rom-dir") rom_dir_arg = next();
        else if (a == "--data-port")
            data_port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--clock-port")
            clock_port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--scale") scale = std::atoi(next());
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--integer-scale") integer_scale = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--audio-player") audio_player = std::atoi(next()) - 1;
        else {
            std::fprintf(stderr,
                "usage: %s [--players 1-4] [--rom P] [--bios P] [--host H]\n"
                "          [--player N [--rom P]]... [--rom-dir D]\n"
                "          [--data-port N] [--clock-port N] [--controls P]\n"
                "          [--scale N] [--fullscreen] [--integer-scale]\n"
                "          [--audio-player 1-4] [--verbose]\n"
                "\n"
                "With no --rom each GBA boots its BIOS with an empty cartridge\n"
                "slot, which is where a real one waits to be handed a program\n"
                "over the link. That is what Four Swords Adventures expects,\n"
                "and it needs a real BIOS dump.\n", argv[0]);
            return 2;
        }
    }
    players = std::clamp(players, 1, kMaxPlayers);
    if (scale < 1) scale = 1;
    if (audio_player < 0 || audio_player >= players) audio_player = 0;
    gql::install_logger(verbose);

    const std::string settings_dir = gql::config_dir();
    if (cfg_path.empty()) {
        cfg_path = settings_dir.empty() ? std::string("controls.cfg")
                                        : settings_dir + "/controls.cfg";
    }
    const std::string imgui_ini = settings_dir.empty()
                                      ? std::string("imgui.ini")
                                      : settings_dir + "/imgui.ini";

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
        want.freq = kHostSampleRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        SDL_AudioSpec got{};
        audio.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (!audio.dev)
            std::fprintf(stderr, "audio: %s — running silent\n", SDL_GetError());
        else
            std::printf("audio: player %d heard, %d Hz %d channels\n",
                        audio_player + 1, got.freq, got.channels);
    }

    SDL_GameController* pads[kMaxPlayers] = {nullptr, nullptr, nullptr, nullptr};
    SDL_JoystickID pad_ids[kMaxPlayers] = {-1, -1, -1, -1};
    Machine machines[kMaxPlayers];
    for (int i = 0; i < kMaxPlayers; ++i) machines[i].index = i;

    // Claim the lowest free quadrant, so pads land on players 1..4 in the
    // order they appear and a removed pad's slot is reused.
    const auto attach_pad = [&](int device_index) {
        if (!SDL_IsGameController(device_index)) return;
        // SDL reports already-present devices as ADDED events too, so the same
        // pad arrives twice — once from the startup scan and once from the
        // queue. Claiming it twice gives two quadrants the same controller.
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
        for (int s = 0; s < kMaxPlayers; ++s) if (pad_ids[s] == id) return;
        for (int s = 0; s < players; ++s) {
            if (pads[s]) continue;
            SDL_GameController* c = SDL_GameControllerOpen(device_index);
            if (!c) return;
            pads[s] = c;
            pad_ids[s] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
            controls.player[s].pad = pad_ids[s];
            const char* n = SDL_GameControllerName(c);
            std::printf("controller attached to player %d: %s\n", s + 1,
                        n ? n : "unknown");
            std::fflush(stdout);
            return;
        }
    };
    const auto detach_pad = [&](SDL_JoystickID id) {
        for (int s = 0; s < kMaxPlayers; ++s) {
            if (pad_ids[s] != id) continue;
            SDL_GameControllerClose(pads[s]);
            pads[s] = nullptr;
            pad_ids[s] = -1;
            machines[s].keys.store(0x03FF, std::memory_order_relaxed);
            std::printf("controller removed from player %d\n", s + 1);
            std::fflush(stdout);
            return;
        }
    };
    // Move a pad to a quadrant, swapping with whoever held that seat. Plug
    // order decides the initial assignment and there is otherwise no way to
    // change it; pressing a button on a pad while setting a player up is the
    // clearest statement of which seat that pad belongs to.
    const auto claim_pad = [&](SDL_JoystickID id, int player) {
        if (id < 0 || player < 0 || player >= players) return;
        if (pad_ids[player] == id) return;
        int from = -1;
        for (int s = 0; s < kMaxPlayers; ++s) if (pad_ids[s] == id) from = s;
        if (from < 0) return;
        std::swap(pads[from], pads[player]);
        std::swap(pad_ids[from], pad_ids[player]);
        controls.player[from].pad = pad_ids[from];
        controls.player[player].pad = pad_ids[player];
        machines[from].keys.store(0x03FF, std::memory_order_relaxed);
        machines[player].keys.store(0x03FF, std::memory_order_relaxed);
    };
    const auto release_pad = [&](int player) -> bool {
        if (player < 0 || player >= players || !pads[player]) return true;
        for (int s = 0; s < kMaxPlayers; ++s) {
            if (s == player || pads[s]) continue;
            std::swap(pads[s], pads[player]);
            std::swap(pad_ids[s], pad_ids[player]);
            controls.player[s].pad = pad_ids[s];
            controls.player[player].pad = pad_ids[player];
            machines[player].keys.store(0x03FF, std::memory_order_relaxed);
            return true;
        }
        return false;
    };

    for (int i = 0; i < SDL_NumJoysticks(); ++i) attach_pad(i);

    // --- bring the machines up -------------------------------------------
    //
    // Opened here rather than on their own threads because the dialling below
    // has to happen in player order, and that is far easier to guarantee from
    // one thread than to coordinate between four.
    for (int i = 0; i < players; ++i) {
        machines[i].rom_path =
            per_rom[i].empty() ? rom_path : gql::find_asset(per_rom[i]);
        machines[i].save_path =
            gql::save_path(machines[i].rom_path, i);
        std::string err;
        if (!machines[i].gba.open(machines[i].rom_path, bios_path,
                                  machines[i].save_path, kHostSampleRate,
                                  &err)) {
            std::fprintf(stderr, "player %d: %s\n", i + 1, err.c_str());
            return 1;
        }
        machines[i].pixels.assign(
            static_cast<std::size_t>(GbaInstance::kWidth) * GbaInstance::kHeight,
            0);
        machines[i].booted.store(true);
    }
    for (int i = 0; i < players; ++i) {
        std::printf("p%d: %s%s\n", i + 1,
                    machines[i].rom_path.empty()
                        ? "BIOS, no cartridge"
                        : machines[i].rom_path.c_str(),
                    machines[i].save_path.empty() ? "" : "  (save kept)");
    }

    if (!host.empty()) {
        // In player order, one at a time. Dolphin assigns the connections it
        // accepts to SI slots in the order they arrive, so dialling four at
        // once would scatter the players across the GameCube's ports at
        // random — and the scatter would differ every run.
        for (int i = 0; i < players; ++i) {
            std::string err;
            machines[i].link.store(LinkState::Dialling);
            if (machines[i].gba.dial(host, data_port, clock_port, &err)) {
                machines[i].dialled = true;
                std::printf("p%d link: connected to %s -> SI slot %d\n",
                            i + 1, host.c_str(), i + 1);
            } else {
                machines[i].link.store(LinkState::Off);
                std::printf("p%d link: %s\n", i + 1, err.c_str());
            }
            std::fflush(stdout);
        }
    }

    for (int i = 0; i < players; ++i) {
        machines[i].thread = std::thread(
            machine_thread, &machines[i], i == audio_player ? &audio : nullptr);
    }

    // --- window -----------------------------------------------------------
    const int canvas_w = GbaInstance::kWidth * 2;
    const int canvas_h = GbaInstance::kHeight * 2;
    SDL_Window* win = SDL_CreateWindow(
        "gba-quad-link", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        canvas_w * scale, canvas_h * scale,
        SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u));
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetIO().IniFilename = imgui_ini.c_str();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    SDL_Texture* textures[kMaxPlayers] = {nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < players; ++i) {
        textures[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        GbaInstance::kWidth,
                                        GbaInstance::kHeight);
        SDL_SetTextureScaleMode(textures[i], integer_scale ? SDL_ScaleModeNearest
                                                           : SDL_ScaleModeLinear);
    }

    std::printf("F1, or Select+Start on any pad, opens the controls menu.\n");
    std::fflush(stdout);

    bool menu_open = false;
    bool setup_open = false;
    int capture_player = -1, capture_button = -1;
    std::string status;

    // The cartridge library, scanned once. Nine hundred entries is nothing to
    // hold in memory and far too slow to re-read every frame.
    std::string rom_dir = rom_dir_arg.empty() ? gql::default_rom_dir()
                                              : rom_dir_arg;
    std::vector<gql::RomEntry> roms = gql::scan_roms(rom_dir);
    char rom_filter[64] = {0};
    int browsing_for = -1;   // which player is picking, -1 for nobody
    if (!roms.empty())
        std::printf("library: %d cartridges in %s\n", (int)roms.size(),
                    rom_dir.c_str());

    // Hand one machine a different cartridge, without disturbing the others.
    // The thread has to go first: the instance belongs to it, and reopening a
    // core underneath a thread that is running it is not a thing that can be
    // made safe.
    const auto restart_machine = [&](int i, const std::string& new_rom) {
        Machine& m = machines[i];
        m.stop.store(true);
        m.gba.shutdown_link();          // in case it is parked in a stall
        if (m.thread.joinable()) m.thread.join();
        m.gba.close();

        m.rom_path = new_rom;
        m.save_path = gql::save_path(new_rom, i);
        std::string err;
        if (!m.gba.open(m.rom_path, bios_path, m.save_path, kHostSampleRate,
                        &err)) {
            m.booted.store(false);
            status = "Player " + std::to_string(i + 1) + ": " + err;
            return;
        }
        m.booted.store(true);
        m.link.store(LinkState::Off);
        m.dialled = false;
        // Relinking is deliberately not attempted here. Dolphin hands out SI
        // slots in the order connections arrive, so a machine that reconnects
        // on its own would land in whatever slot happened to be next and the
        // players would swap places mid-game.
        m.stop.store(false);
        m.thread = std::thread(machine_thread, &m,
                               i == audio_player ? &audio : nullptr);
        status = "Player " + std::to_string(i + 1) + ": " +
                 (new_rom.empty() ? std::string("BIOS, no cartridge")
                                  : std::filesystem::path(new_rom).stem().string());
    };

    const auto begin_capture = [&](int p, int b) {
        capture_player = p;
        capture_button = b;
        g_input_held.store(true);
        status = "Press a button or key for player " + std::to_string(p + 1) +
                 " " + gql::button_name(b) + "   (Esc cancels)";
    };
    const auto end_capture = [&]() {
        capture_player = capture_button = -1;
        g_input_held.store(false);
    };

    auto last_report = std::chrono::steady_clock::now();
    while (!g_quit.load(std::memory_order_relaxed)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (menu_open) ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) g_quit.store(true);
            if (ev.type == SDL_CONTROLLERDEVICEADDED) attach_pad(ev.cdevice.which);
            if (ev.type == SDL_CONTROLLERDEVICEREMOVED) detach_pad(ev.cdevice.which);

            // Select+Start on any pad stands in for F1. Game Mode has no
            // keyboard, and a player whose buttons are wrong needs a way in
            // that does not depend on the bindings being right.
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && capture_player < 0) {
                const int b = ev.cbutton.button;
                if (b == SDL_CONTROLLER_BUTTON_BACK ||
                    b == SDL_CONTROLLER_BUTTON_START) {
                    SDL_GameController* c =
                        SDL_GameControllerFromInstanceID(ev.cbutton.which);
                    if (c && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)
                          && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) {
                        // One shortcut, both windows: Game Mode has no second
                        // key to press, and a player who cannot work out
                        // which one they want can see both.
                        const bool any = menu_open || setup_open;
                        menu_open = setup_open = !any;
                        status.clear();
                        continue;
                    }
                }
            }

            // Capture swallows the press, so binding a button cannot also
            // trigger whatever menu control sits under the cursor.
            if (capture_player >= 0) {
                gql::Binding bind;
                if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        status = "Binding cancelled";
                        end_capture();
                        continue;
                    }
                    bind.source = gql::Binding::Source::Key;
                    bind.code = ev.key.keysym.scancode;
                } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    // The press decides the pad as well as the binding: a
                    // binding taken from a controller that drives some other
                    // quadrant would silently do nothing.
                    claim_pad(ev.cbutton.which, capture_player);
                    bind.source = gql::Binding::Source::PadButton;
                    bind.code = ev.cbutton.button;
                } else if (ev.type == SDL_CONTROLLERAXISMOTION &&
                           std::abs(ev.caxis.value) > 20000) {
                    claim_pad(ev.caxis.which, capture_player);
                    bind.source = gql::Binding::Source::PadAxis;
                    bind.code = ev.caxis.axis;
                    bind.dir = ev.caxis.value > 0 ? 1 : -1;
                } else {
                    continue;
                }
                controls.player[capture_player].buttons[capture_button] = bind;
                status = "Bound player " + std::to_string(capture_player + 1) +
                         " " + gql::button_name(capture_button) + " to " +
                         bind.label();
                end_capture();
                continue;
            }

            if (ev.type == SDL_KEYDOWN) {
                const SDL_Scancode sc = ev.key.keysym.scancode;
                if (sc == SDL_SCANCODE_F1) { menu_open = !menu_open; status.clear(); }
                else if (sc == SDL_SCANCODE_F2) { setup_open = !setup_open; status.clear(); }
                else if (sc == SDL_SCANCODE_ESCAPE && !menu_open) g_quit.store(true);
            }
        }

        const Uint8* ks = SDL_GetKeyboardState(nullptr);
        for (int i = 0; i < players; ++i)
            machines[i].keys.store(controls.read(i, pads[i], ks),
                                   std::memory_order_relaxed);

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(ren, &win_w, &win_h);
        const gql::Layout lay = gql::compute_layout(win_w, win_h, integer_scale);

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        for (int i = 0; i < players; ++i) {
            {
                std::lock_guard<std::mutex> lk(machines[i].fb_mutex);
                if (machines[i].has_frame)
                    SDL_UpdateTexture(textures[i], nullptr,
                                      machines[i].pixels.data(),
                                      GbaInstance::kWidth * sizeof(uint32_t));
            }
            const gql::Rect& r = lay.quadrant[i];
            SDL_Rect dst{r.x, r.y, r.w, r.h};
            SDL_RenderCopy(ren, textures[i], nullptr, &dst);
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (lay.gutters) {
            // Drawn straight onto the background list rather than as windows:
            // these are decoration, never interactive, and must not steal
            // focus from the menu when it is open.
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            for (int i = 0; i < players; ++i) {
                const gql::Rect& g = lay.gutter[i % 2];
                const int half = lay.gutter[0].h / 2;
                const float y = static_cast<float>(g.y + (i / 2) * half);
                const float x = static_cast<float>(g.x) + 8.0f;
                const LinkState st = machines[i].link.load();
                ImU32 col = IM_COL32(150, 150, 150, 255);
                if (st == LinkState::Linked) col = IM_COL32(120, 220, 120, 255);
                else if (st == LinkState::Lost) col = IM_COL32(230, 110, 110, 255);
                else if (st == LinkState::Waiting || st == LinkState::Dialling)
                    col = IM_COL32(230, 200, 110, 255);

                char line[64];
                std::snprintf(line, sizeof line, "P%d", i + 1);
                dl->AddText(ImVec2(x, y + 10), IM_COL32(230, 230, 230, 255), line);
                dl->AddText(ImVec2(x, y + 28), col,
                            gql::link_state_name(st));
                std::snprintf(line, sizeof line, "%.0f fps",
                              machines[i].fps.load());
                dl->AddText(ImVec2(x, y + 46), IM_COL32(170, 170, 170, 255), line);
                const char* pn = pads[i] ? SDL_GameControllerName(pads[i]) : nullptr;
                std::snprintf(line, sizeof line, "%.14s", pn ? pn : "no pad");
                dl->AddText(ImVec2(x, y + 64), IM_COL32(150, 150, 150, 255), line);
            }
        }

        if (setup_open) {
            ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
            ImGui::Begin("Setup", nullptr, ImGuiWindowFlags_NoCollapse);

            if (browsing_for < 0) {
                ImGui::TextUnformatted("What each player is running. "
                                       "F2 or Select+Start closes this.");
                ImGui::Separator();
                for (int p = 0; p < players; ++p) {
                    ImGui::PushID(3000 + p);
                    const std::string cart =
                        machines[p].rom_path.empty()
                            ? std::string("— no cartridge (waiting for a link) —")
                            : std::filesystem::path(machines[p].rom_path).stem().string();
                    char label[32];
                    std::snprintf(label, sizeof label, "Player %d", p + 1);
                    ImGui::TextUnformatted(label);
                    ImGui::SameLine(110.0f);
                    ImGui::SetNextItemWidth(340.0f);
                    if (ImGui::Button(cart.c_str(), ImVec2(340, 0))) {
                        browsing_for = p;
                        rom_filter[0] = '\0';
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear")) restart_machine(p, "");
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (roms.empty()) {
                    ImGui::TextWrapped(
                        "No cartridges found. Looked in \"%s\". Point "
                        "--rom-dir at a folder of .gba, .zip or .7z files, or "
                        "set GQL_ROM_DIR.",
                        rom_dir.empty() ? "(nowhere)" : rom_dir.c_str());
                } else {
                    ImGui::Text("%d cartridges in %s", (int)roms.size(),
                                rom_dir.c_str());
                }
                if (ImGui::Button("Rescan")) {
                    roms = gql::scan_roms(rom_dir);
                    status = "Found " + std::to_string(roms.size()) + " cartridges";
                }
            } else {
                ImGui::Text("Cartridge for player %d", browsing_for + 1);
                ImGui::SameLine();
                if (ImGui::SmallButton("cancel")) browsing_for = -1;
                ImGui::Separator();
                ImGui::SetNextItemWidth(-FLT_MIN);
                // Typing is the only practical way through a library this
                // size; scrolling nine hundred entries with a stick is not.
                ImGui::InputTextWithHint("##filter", "type to narrow…",
                                         rom_filter, sizeof rom_filter);
                const std::vector<int> hits = gql::filter_roms(roms, rom_filter);
                ImGui::Text("%d of %d", (int)hits.size(), (int)roms.size());
                if (ImGui::BeginChild("list", ImVec2(0, 0), true)) {
                    // Only the visible rows are built: nine hundred buttons a
                    // frame is a tenth of the frame budget for nothing.
                    ImGuiListClipper clip;
                    clip.Begin((int)hits.size());
                    while (clip.Step()) {
                        for (int n = clip.DisplayStart; n < clip.DisplayEnd; ++n) {
                            const gql::RomEntry& e = roms[hits[n]];
                            ImGui::PushID(n);
                            if (ImGui::Selectable(e.display.c_str())) {
                                restart_machine(browsing_for, e.path);
                                browsing_for = -1;
                            }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndChild();
            }
            if (!status.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted(status.c_str());
            }
            ImGui::End();
        }

        if (menu_open) {
            ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_FirstUseEver);
            ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoCollapse);
            ImGui::TextUnformatted(
                "The machines keep running — Dolphin is waiting on them. "
                "F1 or Select+Start closes this.");
            ImGui::Separator();

            if (ImGui::BeginTable("binds", players + 1,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableSetupColumn("Button");
                for (int p = 0; p < players; ++p) {
                    const char* pn = pads[p] ? SDL_GameControllerName(pads[p]) : nullptr;
                    char h[80];
                    std::snprintf(h, sizeof h, "P%d — %s", p + 1,
                                  pn ? pn : "no pad");
                    ImGui::TableSetupColumn(h);
                }
                ImGui::TableHeadersRow();
                for (int b = 0; b < kButtons; ++b) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(gql::button_name(b));
                    for (int p = 0; p < players; ++p) {
                        ImGui::TableSetColumnIndex(p + 1);
                        ImGui::PushID(p * kButtons + b);
                        const bool capturing =
                            capture_player == p && capture_button == b;
                        const std::string lbl =
                            capturing ? "press…"
                                      : controls.player[p].buttons[b].label();
                        if (ImGui::Button(lbl.c_str(), ImVec2(-FLT_MIN, 0)))
                            begin_capture(p, b);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::TextUnformatted(
                "Controller — which physical pad drives each quadrant. "
                "Binding a button from a pad also moves it here.");
            for (int p = 0; p < players; ++p) {
                ImGui::PushID(2000 + p);
                char label[96];
                std::snprintf(label, sizeof label, "Player %d", p + 1);
                const char* cur = pads[p] ? (SDL_GameControllerName(pads[p])
                                                 ? SDL_GameControllerName(pads[p])
                                                 : "unknown pad")
                                          : "none";
                ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo(label, cur)) {
                    if (ImGui::Selectable("none", !pads[p]))
                        if (!release_pad(p))
                            status = "Every quadrant has a pad; nowhere to put this one";
                    for (int q = 0; q < players; ++q) {
                        if (!pads[q]) continue;
                        const char* n = SDL_GameControllerName(pads[q]);
                        char item[96];
                        std::snprintf(item, sizeof item, "%s##%d",
                                      n ? n : "unknown pad", q);
                        if (ImGui::Selectable(item, q == p))
                            claim_pad(pad_ids[q], p);
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            ImGui::Separator();
            for (int p = 0; p < players; ++p) {
                if (p) ImGui::SameLine();
                ImGui::PushID(1000 + p);
                char lbl[32];
                std::snprintf(lbl, sizeof lbl, "Reset P%d", p + 1);
                if (ImGui::Button(lbl)) {
                    controls.reset_player(p);
                    status = "Player " + std::to_string(p + 1) + " reset";
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Save"))
                status = gql::save_controls(cfg_path, controls)
                             ? "Saved to " + cfg_path
                             : "Could not write " + cfg_path;
            ImGui::SameLine();
            if (ImGui::Button("Reset all to defaults")) {
                controls.reset_to_defaults();
                status = "All players reset to defaults";
            }
            ImGui::SameLine();
            // Escape closes the window; Game Mode has no keyboard and no title
            // bar, so this is the only way out from a controller.
            if (ImGui::Button("Quit")) g_quit.store(true);
            if (!status.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted(status.c_str());
            }
            ImGui::End();
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(5)) {
            std::printf("fps:");
            for (int i = 0; i < players; ++i)
                std::printf(" p%d %5.2f/%s", i + 1, machines[i].fps.load(),
                            gql::link_state_name(machines[i].link.load()));
            std::printf("   audio: %llu underruns, %llu drops\n",
                        (unsigned long long)audio.underruns.load(),
                        (unsigned long long)audio.dropped.load());
            std::fflush(stdout);
            last_report = now;
        }
    }

    g_quit.store(true);
    // Before the joins, not after: a machine parked in a stalled run_frame()
    // never reaches the top of its loop to notice.
    for (int i = 0; i < players; ++i) machines[i].gba.shutdown_link();
    for (int i = 0; i < players; ++i)
        if (machines[i].thread.joinable()) machines[i].thread.join();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (textures[i]) SDL_DestroyTexture(textures[i]);
        if (pads[i]) SDL_GameControllerClose(pads[i]);
    }
    if (audio.dev) SDL_CloseAudioDevice(audio.dev);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

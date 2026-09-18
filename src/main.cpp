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

#include "audio.h"
#include "cable.h"
#include "controls.h"
#include "gba_instance.h"
#include "layout.h"
#include "machine.h"
#include "paths.h"
#include "ap_fetch.h"
#include "ap_patch.h"
#include "ap_worlds.h"
#include "romlist.h"
#include "settings.h"

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


// Runs one machine until told to stop. The instance was opened and dialled by
// the host thread; from here on it belongs to this one.
void machine_thread(Machine* me, gql::AudioMixer* mixer) {
    gql::set_log_player(me->index);

    if (me->mode == gql::LinkMode::Cable)
        me->link.store(LinkState::Cable, std::memory_order_relaxed);

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
        // May come back without a finished frame: the coordinator suspends a
        // machine that has run ahead, and it stops where it is rather than at
        // a frame boundary.
        const bool completed = me->gba.run_frame();
        me->gba.cable_wait();
        me->gba.note_sio_mode();
        me->ap.serve(me->gba);
        if (me->want_title.exchange(false, std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(me->fb_mutex);
            me->title = me->gba.rom_title();
            me->have_title = true;
        }
        if (completed) ++frame;

        // Every machine is drained whether or not anyone is listening — an
        // undrained core backs up — but the mixer decides what is heard.
        const std::size_t got = me->gba.drain_audio(sink.data(),
                                                    sink.size() / 2);
        if (mixer) mixer->push(me->index, sink.data(), got);

        if (completed) {
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
        if (!completed) continue;   // pace and report on whole frames only
        if (now - fps_mark >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(
                now - fps_mark).count();
            me->fps.store((frame - fps_frame) / secs,
                          std::memory_order_relaxed);
            fps_mark = now;
            fps_frame = frame;

        }

        // On the cable, only the parent is paced. The coordinator already
        // keeps the others in step with it — that is its whole job — and it
        // does so by suspending whoever has run ahead, which is a far tighter
        // instrument than four independent sleeps.
        //
        // Four machines each sleeping to hold 59.7fps at their own frame
        // boundaries is the same mistake as pacing a guest Dolphin is waiting
        // on, in a different costume: a child asleep at the wrong moment is a
        // child that cannot take part in the transfer the parent is running,
        // and the game sees a cable with nobody on the end of it.
        // Read every frame now that it is a plain atomic kept current by the
        // coordinator, so a change of parent is picked up immediately.
        const bool cable_child = me->gba.on_cable() &&
                                 me->gba.cable_player_id() > 0;
        if (linked || cable_child) {
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
    bool cable_all = false;         // every machine on one link cable
    // Whatever any player presses drives every machine. Walking four guests
    // through the same menus to reach a link screen is four times the work for
    // one person, and the menus are identical — but the moment the game asks
    // the four of them to choose something different, it is the opposite of
    // what is wanted. So it is a toggle, not a setting.
    bool mirror_start = false;
    // Four machines that have to be walked through the same menus to reach a
    // link screen is four times the work for one person, and the menus are
    // identical. One input driving all of them gets them there together.
    bool log_sio = false;
    // Hands each machine a different cartridge in turn while everything runs,
    // which is the one thing the setup screen does that no other test reaches.
    bool self_test_restart = false;
    // Gives player two player three's cartridge once the others have settled,
    // which is someone walking up and joining a game already in progress. The
    // established players must keep their positions on the cable; a newcomer
    // taking the parent's seat stops everybody.
    bool self_test_join = false;
    // Reads each guest's cartridge title out of its own memory, which is what
    // an Archipelago game client does to decide whether it is looking at the
    // cartridge it expects.
    bool dump_rom_title = false;
    // Listen for Archipelago's game clients, one port per machine.
    bool archipelago = false;
    uint16_t data_port = 54970, clock_port = 49420;
    int scale = 2;
    bool fullscreen = false, integer_scale = false, verbose = false;
    int audio_player_arg = -1;   // -1: use the remembered selection

    // --player N opens a section: flags after it apply to that machine alone,
    // until the next --player. Before any section they set the default for
    // every machine. The setup screen is the real interface; this is for
    // getting at it from a script.
    std::string per_rom[kMaxPlayers];
    std::string per_patch[kMaxPlayers];
    std::string ap_server_arg;
    gql::LinkMode per_mode[kMaxPlayers] = {};
    bool per_mode_set[kMaxPlayers] = {};
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
        else if (a == "--link") {
            const std::string m = next();
            gql::LinkMode mode = gql::LinkMode::Cable;
            if (m == "dolphin") mode = gql::LinkMode::Dolphin;
            else if (m != "cable") {
                std::fprintf(stderr, "--link takes cable or dolphin\n");
                return 2;
            }
            if (section >= 0) {
                per_mode[section] = mode;
                per_mode_set[section] = true;
            } else {
                for (int k = 0; k < kMaxPlayers; ++k) {
                    per_mode[k] = mode;
                    per_mode_set[k] = true;
                }
            }
        }
        else if (a == "--patch") {
            if (section < 0) {
                std::fprintf(stderr, "--patch needs a --player before it\n");
                return 2;
            }
            per_patch[section] = next();
        }
        else if (a == "--ap-server") ap_server_arg = next();
        else if (a == "--bios") bios_path = next();
        else if (a == "--controls") cfg_path = next();
        else if (a == "--host") host = next();
        else if (a == "--rom-dir") rom_dir_arg = next();
        else if (a == "--cable") cable_all = true;
        else if (a == "--mirror-input") mirror_start = true;
        else if (a == "--log-sio") log_sio = true;
        else if (a == "--self-test-restart") self_test_restart = true;
        else if (a == "--self-test-join") self_test_join = true;
        else if (a == "--dump-rom-title") dump_rom_title = true;
        else if (a == "--archipelago") archipelago = true;
        else if (a == "--data-port")
            data_port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--clock-port")
            clock_port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--scale") scale = std::atoi(next());
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--integer-scale") integer_scale = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--audio-player")
            audio_player_arg = std::atoi(next()) - 1;
        else {
            std::fprintf(stderr,
                "usage: %s [--players 1-4] [--rom P] [--bios P] [--host H]\n"
                "          [--player N [--rom P] [--link MODE] [--patch P]]...\n"
                "          [--archipelago] [--ap-server HOST:PORT]\n"
                "          [--rom-dir D]\n"
                "          [--cable] [--mirror-input]\n"
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
    if (audio_player_arg >= players) audio_player_arg = 0;
    gql::install_logger(verbose);
    if (log_sio) gql::log_only_sio();

    const std::string settings_dir = gql::config_dir();
    if (cfg_path.empty()) {
        cfg_path = settings_dir.empty() ? std::string("controls.cfg")
                                        : settings_dir + "/controls.cfg";
    }
    // Remembered from last time, then overridden by anything on the command
    // line. Asking someone to type the path to their cartridges on a
    // television, with a controller, every time they sit down, is not a
    // reasonable thing to do to a person.
    const std::string settings_path =
        settings_dir.empty() ? std::string("settings.cfg")
                             : settings_dir + "/settings.cfg";
    gql::Settings settings;
    gql::load_settings(settings_path, &settings);
    if (rom_dir_arg.empty()) rom_dir_arg = settings.rom_dir;
    if (host.empty()) host = settings.host;

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

    gql::AudioMixer mixer;
    if (!mixer.open(kHostSampleRate))
        std::fprintf(stderr, "audio: %s — running silent\n", SDL_GetError());
    for (int i = 0; i < kMaxPlayers; ++i) {
        mixer.set_enabled(i, settings.audio_on[i]);
        mixer.set_gain(i, settings.audio_gain[i]);
    }
    // --audio-player still means "only this one", which is what it always
    // meant, and is now one arrangement of many rather than the only one.
    if (audio_player_arg >= 0) {
        for (int i = 0; i < kMaxPlayers; ++i)
            mixer.set_enabled(i, i == audio_player_arg);
    }
    {
        int on = 0;
        for (int i = 0; i < kMaxPlayers; ++i) if (mixer.enabled(i)) ++on;
        std::printf("audio: %d Hz, %d machine%s heard\n", kHostSampleRate,
                    on, on == 1 ? "" : "s");
    }

    SDL_GameController* pads[kMaxPlayers] = {nullptr, nullptr, nullptr, nullptr};
    SDL_JoystickID pad_ids[kMaxPlayers] = {-1, -1, -1, -1};
    // Declared before the machines, and this is not a matter of taste. Local
    // objects are destroyed in reverse order, and a machine's destructor
    // detaches it from the cable — so a cable declared after them is torn down
    // first and every machine then reaches into a coordinator that no longer
    // exists. It survived review because it only goes wrong on the way out,
    // and only crashes when the freed page happens to be unmapped.
    //
    // One cable, shared by whichever machines are plugged into it. Cheap when
    // nothing is.
    gql::CableGroup cables[gql::kMaxCables];

    Machine machines[kMaxPlayers];
    for (int i = 0; i < kMaxPlayers; ++i) machines[i].index = i;

    // Where a machine should ask to sit on its cable: after everyone already
    // there.
    //
    // Position is not quadrant, and asking for the quadrant is actively
    // harmful once people are already playing. A cable has one parent, at
    // position zero, and it is the only machine that starts transfers — so a
    // newcomer whose quadrant sorts ahead of the existing players takes the
    // parent's seat from underneath them. The machine that joins is by
    // definition sitting on a title screen, so the cable then has a parent
    // that never speaks, and everybody's link stops.
    //
    // Joining at the end leaves the established players where they were,
    // which is also what walking up and plugging into the end of a chain
    // does. Four machines attaching together at startup still come out in
    // quadrant order, because they attach in quadrant order.
    const auto next_slot_in = [&](int group, int self) {
        int n = 0;
        for (int j = 0; j < players; ++j) {
            if (j == self) continue;
            if (machines[j].mode != gql::LinkMode::Cable) continue;
            if (machines[j].group != group) continue;
            if (machines[j].gba.on_cable()) ++n;
        }
        return n;
    };

    // Work out which cable each machine belongs on.
    //
    // You link with the people playing your game, so cartridge is the grouping
    // — and it is a better signal than watching the serial port, because by
    // the time two machines are visibly trying to talk to each other they have
    // already failed to. A machine with an override goes where it is told,
    // which is what the cases cartridge identity gets wrong need: the Mario
    // Advance games all link to play Mario Bros., Pokemon versions trade with
    // each other, and in single-pak multiplayer only one machine has a
    // cartridge at all.
    //
    // Returns true if anything moved.
    const auto compute_groups = [&]() {
        int before[kMaxPlayers];
        for (int i = 0; i < players; ++i) before[i] = machines[i].group;

        // What decides who plays with whom: the cartridge, unless a machine
        // has been told which cable to take.
        const auto key_of = [&](int i) {
            return machines[i].group_override >= 0
                       ? "!override " + std::to_string(machines[i].group_override)
                       : machines[i].rom_path;
        };

        // Which cable number each game keeps, decided by where that game is
        // already being played rather than by whose quadrant comes first.
        //
        // This is the whole difficulty. A third person picking up the game two
        // others are already playing must join *them*; if instead the number
        // follows the newcomer, the two who changed nothing are recorded as
        // having moved, get torn off their cable and restarted, and the link
        // they already had is destroyed by somebody else sitting down. So each
        // game claims the cable number that the most machines already playing
        // it are sitting on, and the newcomer is the one who moves.
        std::vector<std::string> keys;
        for (int i = 0; i < players; ++i) {
            const std::string k = key_of(i);
            if (std::find(keys.begin(), keys.end(), k) == keys.end())
                keys.push_back(k);
        }

        struct Claim { std::string key; int want; int weight; };
        std::vector<Claim> claims;
        for (const std::string& k : keys) {
            int tally[gql::kMaxCables] = {};
            for (int i = 0; i < players; ++i)
                if (key_of(i) == k && machines[i].group >= 0 &&
                    machines[i].group < gql::kMaxCables)
                    ++tally[machines[i].group];
            int want = 0, weight = -1;
            for (int g = 0; g < gql::kMaxCables; ++g)
                if (tally[g] > weight) { weight = tally[g]; want = g; }
            claims.push_back(Claim{k, want, weight});
        }
        // Strongest claim first, so an established group keeps its number and
        // whoever is joining takes what is left.
        std::stable_sort(claims.begin(), claims.end(),
                         [](const Claim& a, const Claim& b) {
                             return a.weight > b.weight;
                         });

        std::string key_of_group[gql::kMaxCables];
        bool used[gql::kMaxCables] = {};
        for (const Claim& c : claims) {
            int g = -1;
            if (!used[c.want]) g = c.want;
            else
                for (int k = 0; k < gql::kMaxCables && g < 0; ++k)
                    if (!used[k]) g = k;
            if (g < 0) g = 0;
            used[g] = true;
            key_of_group[g] = c.key;
        }

        for (int i = 0; i < players; ++i) {
            const std::string key = key_of(i);
            for (int g = 0; g < gql::kMaxCables; ++g)
                if (used[g] && key_of_group[g] == key) { machines[i].group = g; break; }
        }

        for (int i = 0; i < players; ++i)
            if (before[i] != machines[i].group) return true;
        return false;
    };

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

    // Before anything reads a mode. Applying these after the dialling and the
    // cable attach meant every machine still held its default when the two
    // things that care about modes ran, so a machine asked for Dolphin was
    // never dialled and then excluded from the cable for wanting Dolphin —
    // ending up attached to nothing at all.
    for (int i = 0; i < players; ++i)
        if (per_mode_set[i]) machines[i].mode = per_mode[i];

    if (!host.empty()) {
        // A host and no per-player choice means everyone is going to the
        // GameCube, which is what Four Swords Adventures wants.
        bool any_named = false;
        for (int i = 0; i < players; ++i) if (per_mode_set[i]) any_named = true;
        if (!any_named)
            for (int i = 0; i < players; ++i)
                machines[i].mode = gql::LinkMode::Dolphin;

        // In player order, one at a time. Dolphin assigns the connections it
        // accepts to SI slots in the order they arrive, so dialling several at
        // once would scatter the players across the GameCube's ports at
        // random — and the scatter would differ every run.
        for (int i = 0; i < players; ++i) {
            if (machines[i].mode != gql::LinkMode::Dolphin) continue;
            std::string err;
            machines[i].link.store(LinkState::Dialling);
            if (machines[i].gba.dial(host, data_port, clock_port, &err)) {
                machines[i].dialled = true;
                std::printf("p%d link: connected to %s\n", i + 1, host.c_str());
            } else {
                // Falls back to the cable rather than to nothing, so the mode
                // the menu shows is the mode the machine is actually in.
                machines[i].mode = gql::LinkMode::Cable;
                machines[i].link.store(LinkState::Off);
                std::printf("p%d link: %s\n", i + 1, err.c_str());
            }
            std::fflush(stdout);
        }
    }

    compute_groups();

    {
        int on_cable = 0;
        for (int i = 0; i < players; ++i)
            if (machines[i].mode == gql::LinkMode::Cable) ++on_cable;
        if (on_cable) {
        // All of them, here, before any thread starts. Attaching from each
        // machine's own thread let the first one run for however long it took
        // the last one to be scheduled — thousands of frames, with the
        // coordinator free-running because it believed it had one player. Any
        // two guests that are meant to hand each other a word every frame
        // cannot be that far apart. mGBA's own frontend attaches its players
        // together for the same reason.
        //
        // A machine's position on the cable is its quadrant, so player one is
        // the parent, which is what the numbering means to a game.
        // The quadrant is only a *preference*. The coordinator packs whoever
        // is actually on the cable into positions 0..n-1 with no gaps, so if
        // players one and three are the only two plugged in, they become
        // cable positions 0 and 1 — the third quadrant is the link's second
        // player. That is what a real cable does, and it is why a machine
        // asks for its quadrant rather than being told its position.
        for (int i = 0; i < players; ++i)
            if (machines[i].mode == gql::LinkMode::Cable)
                machines[i].gba.attach_cable(&cables[machines[i].group],
                                            next_slot_in(machines[i].group, i));
        std::printf("link cable: %d machines chained\n", on_cable);
        }
    }

    if (archipelago) {
        // Every machine wants a client. Which one is actually listening at any
        // moment is decided in the main loop, one at a time; see there for
        // why they cannot all listen at once.
        for (int i = 0; i < players && i < gql::kApPortCount; ++i)
            machines[i].ap_wants_client = true;
    }

    for (int i = 0; i < players; ++i) {
        machines[i].thread = std::thread(
            machine_thread, &machines[i], &mixer);
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
    bool mirror_input = mirror_start;
    int capture_player = -1, capture_button = -1;
    std::string status;

    // The cartridge library, scanned once. Nine hundred entries is nothing to
    // hold in memory and far too slow to re-read every frame.
    std::string rom_dir = rom_dir_arg.empty() ? gql::default_rom_dir()
                                              : rom_dir_arg;
    std::vector<gql::RomEntry> roms = gql::scan_roms(rom_dir);
    char rom_filter[64] = {0};
    int browsing_for = -1;   // which player is picking, -1 for nobody
    std::string browsing_dir;             // non-empty while choosing a folder
    std::vector<gql::DirEntry> dir_entries;

    // Choosing a patch file. Patches live wherever Archipelago put them; the
    // default is this app's own folder for them, which is where the docs say
    // to drop them.
    // Which setting the folder browser is currently choosing for.
    enum class DirTarget { Roms, Patches };
    DirTarget dir_target = DirTarget::Roms;

    int browsing_patch_for = -1;
    std::string patch_dir = settings.patch_dir.empty()
                                ? (gql::data_dir().empty()
                                       ? std::string("patches")
                                       : gql::data_dir() + "/patches")
                                : settings.patch_dir;
    std::vector<gql::RomEntry> patch_entries;
    // Found at startup rather than when the Archipelago tab is first opened,
    // because patches can be named on the command line and there may be no
    // menu in it at all.
    if (settings.ap_dir.empty()) settings.ap_dir = gql::find_ap_install();
    gql::ApInstall ap_install;
    const std::string ap_install_dir =
        (gql::data_dir().empty() ? std::string("archipelago")
                                 : gql::data_dir() + "/archipelago");
    if (!ap_server_arg.empty()) settings.ap_server = ap_server_arg;
    if (!roms.empty())
        std::printf("library: %d cartridges in %s\n", (int)roms.size(),
                    rom_dir.c_str());

    // Written back straight away rather than on exit: a session that ends by
    // having its window closed, or by crashing, should still have remembered
    // where the cartridges were.
    const auto remember = [&]() {
        settings.rom_dir = rom_dir;
        settings.patch_dir = patch_dir;
        settings.host = host;
        gql::save_settings(settings_path, settings);
    };
    if (!rom_dir.empty() && rom_dir != settings.rom_dir) remember();


    // Everything between choosing a patch file and having a game running.
    //
    // On a thread because the middle of it can take two minutes: if the
    // cartridge a patch wants has never been hashed, every archive in the
    // library has to be opened to find it. Nothing here touches a core; it
    // ends by leaving a path where the host thread will find it.
    // Drops a player out of Archipelago entirely: kills their client, gives
    // up their port, and takes them out of the queue for one. Also how a
    // client that has hung is got rid of, since it would otherwise hold the
    // listener against everybody behind it.
    const auto clear_archipelago = [&](int i) {
        Machine& m = machines[i];
        if (m.ap_thread.joinable()) m.ap_thread.join();
        m.ap_session.stop();
        m.ap_wants_client = false;
        m.ap.close();
        m.ap_rom_ready.store(false);
        m.ap_patch_path.clear();
        m.ap_stage.store(Machine::ApStage::Idle);
        m.set_ap_note("");
    };

    const auto begin_archipelago = [&](int i, const std::string& chosen) {
        Machine& m = machines[i];
        clear_archipelago(i);
        // Made absolute here and not further down, because the client is
        // started with its working directory changed to the Archipelago
        // folder — a frozen build finds its own libraries relative to where it
        // sits — and a relative path stops meaning anything at that point.
        // The failure is quiet from our side: we read the patch fine, and the
        // client is the one that cannot find it.
        std::error_code pec;
        const std::string patch_path =
            std::filesystem::absolute(chosen, pec).string();
        m.ap_patch_path = pec ? chosen : patch_path;
        m.ap_stage.store(Machine::ApStage::Searching);
        m.set_ap_note("reading patch");

        const std::string ap_dir = settings.ap_dir;
        const std::string server = settings.ap_server;
        const std::string library = rom_dir;
        const std::string cache = gql::data_dir().empty()
                                      ? std::string()
                                      : gql::data_dir() + "/base_roms";

        m.ap_thread = std::thread([&m, i, patch_path, ap_dir, server, library, cache]() {
            gql::ApPatch patch;
            if (!gql::read_ap_patch(patch_path, &patch)) {
                m.set_ap_note("not an Archipelago patch file");
                m.ap_stage.store(Machine::ApStage::Failed);
                return;
            }
            m.ap_patch = patch;
            m.set_ap_note("finding " + patch.game);

            const std::string base =
                gql::find_base_rom(library, patch.base_checksums, cache, patch.game);
            if (base.empty()) {
                m.set_ap_note("no cartridge in your library matches this patch");
                m.ap_stage.store(Machine::ApStage::Failed);
                return;
            }

            m.ap_stage.store(Machine::ApStage::Configuring);
            gql::ApWorld world;
            if (!gql::find_ap_world(ap_dir, patch.game, &world)) {
                m.set_ap_note(patch.game + " is not installed in Archipelago");
                m.ap_stage.store(Machine::ApStage::Failed);
                return;
            }
            std::string err;
            if (!gql::set_ap_rom_path(ap_dir, world, base, &err)) {
                m.set_ap_note("could not configure Archipelago: " + err);
                m.ap_stage.store(Machine::ApStage::Failed);
                return;
            }

            // Where the client will write the patched cartridge.
            std::filesystem::path rom = patch_path;
            rom.replace_extension(patch.result_ending);

            m.ap_stage.store(Machine::ApStage::Starting);
            m.set_ap_note("starting client for " + patch.player_name);
            // A patch from the website carries its own address; one generated
            // locally does not, and then we have to be told.
            const std::string where = patch.server.empty() ? server : patch.server;
            if (!m.ap_session.start(ap_dir, patch_path, where, rom.string(), &err)) {
                m.set_ap_note(err);
                m.ap_stage.store(Machine::ApStage::Failed);
                return;
            }

            // The client patches before it does anything else, so this is
            // quick — but it is somebody else's program and may fail.
            for (int t = 0; t < 120; ++t) {
                if (!m.ap_session.produced_rom().empty()) {
                    std::lock_guard<std::mutex> lk(m.ap_mutex);
                    m.ap_rom = m.ap_session.produced_rom();
                    m.ap_rom_ready.store(true);
                    m.ap_stage.store(Machine::ApStage::Ready);
                    m.ap_note = "patched " + patch.player_name;
                    return;
                }
                if (!m.ap_session.running()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            m.set_ap_note("client did not produce a cartridge");
            m.ap_stage.store(Machine::ApStage::Failed);
        });
    };

    // Patches named on the command line, started now that everything they
    // need exists. Each takes a while and runs on its own thread, so this
    // only kicks them off.
    for (int i = 0; i < players; ++i) {
        if (per_patch[i].empty()) continue;
        if (settings.ap_dir.empty()) {
            std::fprintf(stderr, "--patch: no Archipelago install found\n");
            break;
        }
        begin_archipelago(i, per_patch[i]);
    }

    // Hand one machine a different cartridge, without disturbing the others.
    // The thread has to go first: the instance belongs to it, and reopening a
    // core underneath a thread that is running it is not a thing that can be
    // made safe.
    const auto restart_machine = [&](int i, const std::string& new_rom,
                                     gql::LinkMode new_mode) {
        Machine& m = machines[i];
        m.stop.store(true);
        // Both of the ways a machine can be somewhere other than the top of
        // its loop. Setting stop is not enough for either: a machine waiting
        // on Dolphin for cycles that are not coming never reaches the check,
        // and one the coordinator has suspended is asleep on a condition
        // variable that only the coordinator was ever going to signal. This
        // join runs on the host thread, so missing either of them does not
        // hang a machine, it hangs the window.
        m.gba.shutdown_link();
        m.gba.wake_cable();
        if (m.thread.joinable()) m.thread.join();
        m.gba.close();

        m.rom_path = new_rom;
        m.save_path = gql::save_path(new_rom, i);
        m.mode = new_mode;
        std::string err;
        if (!m.gba.open(m.rom_path, bios_path, m.save_path, kHostSampleRate,
                        &err)) {
            m.booted.store(false);
            status = "Player " + std::to_string(i + 1) + ": " + err;
            return;
        }
        m.booted.store(true);
        m.dialled = false;
        // Joining the cable happens here, before the thread starts, for the
        // same reason it does at startup: the coordinator renumbers everyone
        // on every attach, and doing that to a machine that is mid-frame on
        // its own thread is asking for the desynchronization we just spent an
        // afternoon removing.
        if (new_mode == gql::LinkMode::Cable) {
            m.gba.attach_cable(&cables[m.group], next_slot_in(m.group, i));
            m.link.store(LinkState::Cable);
        } else if (new_mode == gql::LinkMode::Dolphin && !host.empty()) {
            // Dialling and attaching both happen here, while this machine has
            // no thread of its own — so the attach is ordered before anything
            // starts running the core, which is what it needs.
            std::string lerr;
            m.link.store(LinkState::Dialling);
            if (m.gba.dial(host, data_port, clock_port, &lerr)) {
                m.gba.attach();
                m.dialled = true;
                m.link.store(LinkState::Waiting);
                status = "Player " + std::to_string(i + 1) +
                         " joined Dolphin";
            } else {
                // Back on the cable, not nowhere, so what the menu says
                // stays true.
                m.mode = gql::LinkMode::Cable;
                m.gba.attach_cable(&cables[m.group], next_slot_in(m.group, i));
                m.link.store(LinkState::Cable);
                status = "Player " + std::to_string(i + 1) + ": " + lerr;
            }
        }
        // Relinking is deliberately not attempted here. Dolphin hands out SI
        // slots in the order connections arrive, so a machine that reconnects
        // on its own would land in whatever slot happened to be next and the
        // players would swap places mid-game.
        m.stop.store(false);
        m.thread = std::thread(machine_thread, &m,
                               &mixer);
        status = "Player " + std::to_string(i + 1) + ": " +
                 (new_rom.empty() ? std::string("BIOS, no cartridge")
                                  : std::filesystem::path(new_rom).stem().string());
    };

    // A cartridge change can move other machines between cables — someone
    // picking up the game two others are playing joins their cable, and
    // someone putting it down leaves. Only the machines that actually moved
    // are restarted; restarting a player who is not affected would throw away
    // whatever they were in the middle of for nothing.
    const auto regroup = [&]() {
        int before[kMaxPlayers];
        for (int i = 0; i < players; ++i) before[i] = machines[i].group;
        if (!compute_groups()) return;
        for (int i = 0; i < players; ++i) {
            if (before[i] == machines[i].group) continue;
            if (machines[i].mode != gql::LinkMode::Cable) continue;
            restart_machine(i, machines[i].rom_path, machines[i].mode);
        }
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
    auto next_self_test = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    int self_test_at = 0;
    while (!g_quit.load(std::memory_order_relaxed)) {
        if (self_test_restart && self_test_at < players &&
            std::chrono::steady_clock::now() >= next_self_test) {
            std::printf("self-test: restarting p%d\n", self_test_at + 1);
            std::fflush(stdout);
            restart_machine(self_test_at, machines[self_test_at].rom_path,
                            machines[self_test_at].mode);
            std::printf("self-test: p%d back\n", self_test_at + 1);
            std::fflush(stdout);
            ++self_test_at;
            next_self_test = std::chrono::steady_clock::now() +
                             std::chrono::seconds(3);
        }
        if (self_test_join && self_test_at == 0 &&
            std::chrono::steady_clock::now() >= next_self_test) {
            std::printf("self-test: p2 joins p3's game\n");
            std::fflush(stdout);
            restart_machine(1, machines[2].rom_path, machines[1].mode);
            regroup();
            self_test_at = 1;
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // Either window being open means ImGui needs the events. Feeding
            // them only while the controls menu was up left the setup window
            // drawn, visible and completely unclickable.
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
                        menu_open = !menu_open;
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
                if (sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_F2) {
                    menu_open = !menu_open;
                    status.clear();
                }
                else if (sc == SDL_SCANCODE_F3) {
                    mirror_input = !mirror_input;
                    std::printf("mirrored input: %s\n",
                                mirror_input ? "on" : "off");
                    std::fflush(stdout);
                }
                else if (sc == SDL_SCANCODE_ESCAPE && !menu_open) g_quit.store(true);
            }
        }

        // A machine whose Archipelago setup has finished gets its cartridge
        // here, on the thread allowed to restart machines.
        for (int i = 0; i < players; ++i) {
            if (!machines[i].ap_rom_ready.exchange(false)) continue;
            std::string rom;
            {
                std::lock_guard<std::mutex> lk(machines[i].ap_mutex);
                rom = machines[i].ap_rom;
            }
            if (rom.empty()) continue;
            restart_machine(i, rom, machines[i].mode);
            regroup();
            machines[i].ap_wants_client = true;
            status = "Player " + std::to_string(i + 1) + ": " +
                     machines[i].ap_patch.player_name + " (" +
                     machines[i].ap_patch.game + ")";
        }

        // Exactly one Archipelago listener is open at a time.
        //
        // A game client finds its emulator by walking ports 43055 upwards and
        // taking the first that answers, and with two listeners open the
        // second client's connection is accepted into the backlog of a socket
        // nobody is accepting from: it waits there until it times out, rather
        // than being refused and moving on to the next port. One at a time is
        // what makes four clients sort themselves across four quadrants.
        //
        // Which client ends up on which quadrant does not matter, and it is
        // worth knowing that it does not. A client takes its identity from the
        // cartridge it finds rather than from the patch it was started with,
        // so one that attaches to another player's quadrant simply plays that
        // slot. Four clients over four quadrants is right in any order.
        {
            const long long now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

            int waiting = -1;
            for (int i = 0; i < players; ++i)
                if (machines[i].ap.listening()) { waiting = i; break; }

            if (waiting < 0) {
                for (int i = 0; i < players; ++i) {
                    Machine& m = machines[i];
                    if (!m.ap_wants_client || m.ap.client_connected()) continue;
                    // A client that has died is not coming, and must not hold
                    // the port shut against the players whose clients live.
                    // Nothing checks this when the client is somebody else's,
                    // which is what --archipelago means.
                    if (!archipelago && !m.ap_session.running()) continue;
                    const int port = gql::kApPortFirst + i;
                    if (m.ap.open(port, i)) {
                        m.ap_listen_since_ms = now_ms;
                        std::printf("p%d archipelago: listening on "
                                    "127.0.0.1:%d\n", i + 1, port);
                    } else {
                        m.ap_wants_client = false;
                        m.set_ap_note("port " + std::to_string(port) +
                                      " is taken - is BizHawk running?");
                    }
                    std::fflush(stdout);
                    break;
                }
            } else if (now_ms - machines[waiting].ap_listen_since_ms > 20000 &&
                       machines[waiting].ap_stage.load() ==
                           Machine::ApStage::Ready) {
                // The head of the queue holds the port, so a client that never
                // arrives stops the other three getting theirs. Say so, rather
                // than moving on: giving the port to the next player would let
                // a slow client attach to the wrong quadrant later, which is
                // the failure this whole arrangement exists to prevent.
                machines[waiting].set_ap_note(
                    "waiting for this client to attach - clear the slot to let "
                    "the others through");
            }
        }

        const Uint8* ks = SDL_GetKeyboardState(nullptr);
        if (mirror_input) {
            // Whatever any player presses drives every machine. Only for
            // getting four guests through the same menu; useless for playing.
            uint16_t all = 0x03FF;
            for (int i = 0; i < players; ++i)
                all &= controls.read(i, pads[i], ks);
            for (int i = 0; i < players; ++i)
                machines[i].keys.store(all, std::memory_order_relaxed);
        } else {
            for (int i = 0; i < players; ++i)
                machines[i].keys.store(controls.read(i, pads[i], ks),
                                       std::memory_order_relaxed);
        }

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(ren, &win_w, &win_h);
        // Who is on screen. Everyone else carries on unseen.
        int visible[kMaxPlayers];
        int n_visible = 0;
        for (int i = 0; i < players; ++i)
            if (machines[i].shown) visible[n_visible++] = i;
        if (n_visible == 0) {   // never leave a blank window
            visible[0] = 0;
            n_visible = 1;
        }
        const gql::Layout lay =
            gql::compute_layout(win_w, win_h, n_visible, integer_scale);

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        for (int slot = 0; slot < lay.count; ++slot) {
            const int i = visible[slot];
            {
                std::lock_guard<std::mutex> lk(machines[i].fb_mutex);
                if (machines[i].has_frame)
                    SDL_UpdateTexture(textures[i], nullptr,
                                      machines[i].pixels.data(),
                                      GbaInstance::kWidth * sizeof(uint32_t));
            }
            const gql::Rect& r = lay.screen[slot];
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
            for (int slot = 0; slot < lay.count; ++slot) {
                const int i = visible[slot];
                const gql::Rect& g = lay.gutter[slot % 2];
                const int half = lay.gutter[0].h / 2;
                const float y = static_cast<float>(g.y + (slot / 2) * half);
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

                // Archipelago, for the same reason the link state is here:
                // what goes wrong is one of the four quietly not being
                // connected, and the menu is the one place nobody is looking
                // while they play.
                const auto stage = machines[i].ap_stage.load();
                if (stage == Machine::ApStage::Idle) continue;

                const std::string fault = machines[i].ap.fault();
                ImU32 apc = IM_COL32(230, 200, 110, 255);
                std::string what;
                if (stage == Machine::ApStage::Failed || !fault.empty()) {
                    apc = IM_COL32(230, 110, 110, 255);
                    what = fault.empty() ? machines[i].get_ap_note() : fault;
                } else if (machines[i].ap.client_connected()) {
                    // Only read once the setup thread has moved the stage on,
                    // which is what publishes the patch it read.
                    apc = IM_COL32(120, 220, 120, 255);
                    what = machines[i].ap_patch.player_name;
                } else {
                    what = Machine::stage_name(stage);
                }
                std::snprintf(line, sizeof line, "AP %.16s", what.c_str());
                dl->AddText(ImVec2(x, y + 82), apc, line);

                // The last thing the game client asked to be shown — an item
                // going out or coming in. This is the only place it appears
                // at all; a GBA has no room to draw it over the game.
                const std::string msg = machines[i].ap.message();
                if (!msg.empty()) {
                    std::snprintf(line, sizeof line, "%.18s", msg.c_str());
                    dl->AddText(ImVec2(x, y + 100),
                                IM_COL32(170, 170, 170, 255), line);
                }
            }
        }

        if (menu_open) {
            ImGui::SetNextWindowSize(ImVec2(790, 560), ImGuiCond_FirstUseEver);
            ImGui::Begin("gba-quad-link", nullptr, ImGuiWindowFlags_NoCollapse);
            ImGui::TextUnformatted(
                "The machines keep running - Dolphin and the cable are waiting "
                "on them. F1 or Select+Start closes this.");
            ImGui::Separator();
            // Choosing a folder replaces the whole window rather than living
            // inside one tab. Both the cartridge library and the patch folder
            // need it, they are on different tabs, and a view that draws
            // itself somewhere other than where it was asked for is invisible.
            if (!browsing_dir.empty()) {
            // Walking the filesystem with a controller. Folders only —
            // picking a cartridge is the other browser's job, and this one
            // only has to arrive at the folder they live in.
            ImGui::TextUnformatted("Where are your cartridges?");
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) {
                browsing_dir.clear();
                dir_entries.clear();
            }
            ImGui::Separator();

            for (const gql::DirEntry& r : gql::quick_roots()) {
                ImGui::PushID(r.path.c_str());
                if (ImGui::SmallButton(r.name.c_str())) {
                    browsing_dir = r.path;
                    dir_entries = gql::list_subdirs(browsing_dir);
                }
                ImGui::PopID();
                ImGui::SameLine();
            }
            ImGui::NewLine();

            ImGui::TextWrapped("%s", browsing_dir.c_str());
            const int here = gql::count_roms(browsing_dir);
            // Said before they commit, so the right folder is recognisable
            // without having to choose it and find out.
            if (here > 0)
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f),
                                   "%d cartridges in this folder", here);
            else
                ImGui::TextDisabled("no cartridges directly in this folder");

            ImGui::BeginDisabled(here == 0);
            if (ImGui::Button("Use this folder")) {
                rom_dir = browsing_dir;
                roms = gql::scan_roms(rom_dir);
                remember();
                status = "Found " + std::to_string(roms.size()) +
                         " cartridges";
                browsing_dir.clear();
                dir_entries.clear();
            }
            ImGui::EndDisabled();
            ImGui::Separator();

            if (ImGui::BeginChild("dirs", ImVec2(0, 0), true)) {
                const std::string up = gql::parent_dir(browsing_dir);
                if (!up.empty() && ImGui::Selectable(".."))  {
                    browsing_dir = up;
                    dir_entries = gql::list_subdirs(browsing_dir);
                }
                for (int n = 0; n < (int)dir_entries.size(); ++n) {
                    ImGui::PushID(n);
                    const int inside = gql::count_roms(dir_entries[n].path);
                    char label[320];
                    std::snprintf(label, sizeof label, "%s%s",
                                  dir_entries[n].name.c_str(),
                                  inside ? "   >" : "");
                    if (ImGui::Selectable(label)) {
                        browsing_dir = dir_entries[n].path;
                        dir_entries = gql::list_subdirs(browsing_dir);
                    }
                    if (inside) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%d)", inside);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            } else {
            ImGui::BeginTabBar("tabs");
            if (ImGui::BeginTabItem("Games")) {

            if (browsing_for < 0 && browsing_dir.empty()) {
                ImGui::TextWrapped(
                    "Tick to show a player on screen. Unticking one does not "
                    "stop it - the game keeps running, so somebody taking a "
                    "break comes back to where they left off, and everybody "
                    "still playing gets a bigger share of the screen.");
                ImGui::Separator();
                for (int p = 0; p < players; ++p) {
                    ImGui::PushID(3000 + p);
                    const std::string cart =
                        machines[p].rom_path.empty()
                            ? std::string("(no cartridge - waiting for a link)")
                            : std::filesystem::path(machines[p].rom_path).stem().string();
                    char label[32];
                    std::snprintf(label, sizeof label, "Player %d", p + 1);
                    // On screen or not. Not running or not — a machine nobody
                    // is looking at keeps playing, so stepping out for ten
                    // minutes costs you nothing.
                    bool shown = machines[p].shown;
                    if (ImGui::Checkbox("##shown", &shown)) {
                        machines[p].shown = shown;
                        int n = 0;
                        for (int q = 0; q < players; ++q)
                            if (machines[q].shown) ++n;
                        status = n <= 1 ? std::string("One screen - full window")
                               : n == 2 ? std::string("Two screens - split")
                                        : std::to_string(n) + " screens";
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(label);
                    ImGui::SameLine(110.0f);
                    if (ImGui::Button(cart.c_str(), ImVec2(330, 0))) {
                        browsing_for = p;
                        rom_filter[0] = '\0';
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear")) {
                        restart_machine(p, "", machines[p].mode);
                        regroup();
                    }

                    // What this machine's serial port is plugged into. A GBA
                    // has one, so these are exclusive.
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(92.0f);
                    // Label and mode together. Listing the labels separately
                    // and casting the chosen index to the enum needs the two
                    // orders to agree, and when they silently did not, picking
                    // "dolphin" put a machine on the cable and picking "cable"
                    // sent it to a Dolphin that was not there.
                    struct ModeChoice { const char* label; gql::LinkMode mode; };
                    static const ModeChoice kModes[] = {
                        {"cable",   gql::LinkMode::Cable},
                        {"dolphin", gql::LinkMode::Dolphin},
                    };
                    constexpr int kModeCount =
                        static_cast<int>(sizeof kModes / sizeof kModes[0]);
                    int cur = 0;
                    for (int k = 0; k < kModeCount; ++k)
                        if (kModes[k].mode == machines[p].mode) cur = k;
                    const char* labels[kModeCount];
                    for (int k = 0; k < kModeCount; ++k) labels[k] = kModes[k].label;
                    if (machines[p].mode == gql::LinkMode::Cable) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(104.0f);
                        // "auto" is by cartridge and is right nearly always.
                        // The letters are for the games cartridge identity
                        // gets wrong - trading between versions, the Mario
                        // Advance link, one cartridge shared by four people.
                        const char* kGroups[] = {"auto", "A", "B", "C", "D"};
                        int gsel = machines[p].group_override + 1;
                        if (ImGui::Combo("##group", &gsel, kGroups, 5)) {
                            machines[p].group_override = gsel - 1;
                            regroup();
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("on %c", 'A' + machines[p].group);
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(92.0f);
                    if (ImGui::Combo("##mode", &cur, labels, kModeCount)) {
                        const gql::LinkMode want = kModes[cur].mode;
                        if (want != machines[p].mode) {
                            if (want == gql::LinkMode::Dolphin && host.empty())
                                status = "No Dolphin to join - set a host "
                                         "below, or start with --host";
                            else {
                                restart_machine(p, machines[p].rom_path, want);
                                regroup();
                            }
                        }
                    }

                    // Handing the same cartridge to someone else is the common
                    // case — four people sitting down to the same game — and
                    // making each of them find it again in a list of hundreds
                    // is the kind of thing that gets a program put down.
                    if (!machines[p].rom_path.empty() && players > 1) {
                        ImGui::SameLine();
                        ImGui::TextUnformatted("to");
                        for (int q = 0; q < players; ++q) {
                            if (q == p) continue;
                            ImGui::SameLine();
                            ImGui::PushID(q);
                            char n[8];
                            std::snprintf(n, sizeof n, "%d", q + 1);
                            // Already holding it is not a reason to restart
                            // them; that would throw away whatever they were
                            // in the middle of.
                            const bool same =
                                machines[q].rom_path == machines[p].rom_path;
                            ImGui::BeginDisabled(same);
                            if (ImGui::SmallButton(n)) {
                                restart_machine(q, machines[p].rom_path,
                                                machines[q].mode);
                                regroup();
                            }
                            ImGui::EndDisabled();
                            if (same && ImGui::IsItemHovered(
                                    ImGuiHoveredFlags_AllowWhenDisabled))
                                ImGui::SetTooltip("Player %d already has it",
                                                  q + 1);
                            ImGui::PopID();
                        }
                        ImGui::SameLine();
                        bool any_other = false;
                        for (int q = 0; q < players; ++q)
                            if (q != p && machines[q].rom_path != machines[p].rom_path)
                                any_other = true;
                        ImGui::BeginDisabled(!any_other);
                        if (ImGui::SmallButton("all")) {
                            for (int q = 0; q < players; ++q)
                                if (q != p &&
                                    machines[q].rom_path != machines[p].rom_path)
                                    restart_machine(q, machines[p].rom_path,
                                                    machines[q].mode);
                            regroup();
                            status = "All players given " +
                                     std::filesystem::path(machines[p].rom_path)
                                         .stem().string();
                        }
                        ImGui::EndDisabled();
                    }
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
                ImGui::Separator();
                {
                    static char host_buf[64] = {0};
                    static bool host_init = false;
                    if (!host_init) {
                        std::snprintf(host_buf, sizeof host_buf, "%s",
                                      host.c_str());
                        host_init = true;
                    }
                    ImGui::SetNextItemWidth(220.0f);
                    if (ImGui::InputText("Dolphin host", host_buf,
                                         sizeof host_buf)) {
                        host = host_buf;
                        remember();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "The machine Dolphin is running on. Players take "
                            "the GameCube's controller ports in the order they "
                            "join, so switch them on in the order you want "
                            "them numbered - or just read your own quadrant "
                            "and ignore what the game calls you.");
                }
                ImGui::Separator();
                if (ImGui::Checkbox("Mirror one player's controls to all four "
                                    "(F3)", &mirror_input)) {}
                ImGui::TextWrapped(
                    "For walking every machine through the same menu at once. "
                    "Turn it off before anyone has to choose something of "
                    "their own - a character, a kart, a track vote.");
                ImGui::Separator();
                if (ImGui::Button("Change folder...")) {
                    dir_target = DirTarget::Roms;
                    browsing_dir = rom_dir.empty() ? gql::default_rom_dir()
                                                   : rom_dir;
                    if (browsing_dir.empty()) browsing_dir = "/";
                    dir_entries = gql::list_subdirs(browsing_dir);
                }
                ImGui::SameLine();
                if (ImGui::Button("Rescan")) {
                    roms = gql::scan_roms(rom_dir);
                    remember();
                    status = "Found " + std::to_string(roms.size()) +
                             " cartridges";
                }
            } else {
                ImGui::Text("Cartridge for player %d", browsing_for + 1);
                ImGui::SameLine();
                if (ImGui::SmallButton("cancel")) browsing_for = -1;
                ImGui::Separator();
                ImGui::SetNextItemWidth(-FLT_MIN);
                // Typing is the only practical way through a library this
                // size; scrolling nine hundred entries with a stick is not.
                ImGui::InputTextWithHint("##filter", "type to narrow...",
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
                                restart_machine(browsing_for, e.path,
                                                machines[browsing_for].mode);
                                regroup();
                                browsing_for = -1;
                            }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Archipelago")) {
                if (settings.ap_dir.empty()) {
                    const std::string found = gql::find_ap_install();
                    if (!found.empty()) { settings.ap_dir = found; remember(); }
                }
                if (settings.ap_dir.empty()) {
                    // The one step this cannot do on its own, until it is
                    // asked to. Behind a button rather than automatic:
                    // ninety megabytes is a lot to spend because somebody
                    // opened a tab to see what was on it, and what arrives
                    // is a program that then gets run.
                    ImGui::TextWrapped(
                        "Archipelago is not installed where this can find it. "
                        "It can be fetched now, or put a copy in "
                        "~/.local/share/gba-quad-link/archipelago yourself "
                        "(an unpacked AppImage is fine).");
                    ImGui::Spacing();

                    const auto st = ap_install.stage();
                    if (ap_install.busy()) {
                        const float p = ap_install.progress();
                        if (p >= 0.0f)
                            ImGui::ProgressBar(p, ImVec2(360, 0));
                        else
                            // Negative runs ImGui's indeterminate bar, which
                            // is the honest thing to show while GitHub has
                            // not said how big it is.
                            ImGui::ProgressBar(
                                -1.0f * static_cast<float>(ImGui::GetTime()),
                                ImVec2(360, 0), "working");
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) ap_install.cancel();
                        ImGui::TextDisabled("%s", ap_install.note().c_str());
                    } else {
                        if (ImGui::Button("Download Archipelago (about 90 MB)",
                                          ImVec2(360, 0)))
                            ap_install.start(ap_install_dir);
                        if (st == gql::ApInstall::Stage::Failed) {
                            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f),
                                               "%s", ap_install.note().c_str());
                        }
                    }

                    // Adopt it the moment it lands, so the tab fills in
                    // rather than asking to be reopened.
                    if (st == gql::ApInstall::Stage::Done &&
                        !ap_install.installed().empty()) {
                        settings.ap_dir = ap_install.installed();
                        remember();
                    }
                } else {
                if (browsing_patch_for >= 0) {
                    ImGui::Text("Patch for player %d", browsing_patch_for + 1);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("cancel")) browsing_patch_for = -1;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("change folder...")) {
                        dir_target = DirTarget::Patches;
                        browsing_dir = patch_dir;
                        if (browsing_dir.empty()) browsing_dir = "/";
                        dir_entries = gql::list_subdirs(browsing_dir);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("rescan"))
                        patch_entries = gql::scan_roms(patch_dir);
                    ImGui::Separator();
                    ImGui::TextWrapped("From %s", patch_dir.c_str());
                    if (patch_entries.empty())
                        ImGui::TextDisabled(
                            "Nothing here. Patch files come from a multiworld - a "
                            "room page on the website, or the output of a local "
                            "generation - and end in .ap followed by the game.");
                    if (ImGui::BeginChild("patches", ImVec2(0, 0), true)) {
                        for (int n = 0; n < (int)patch_entries.size(); ++n) {
                            ImGui::PushID(n);
                            if (ImGui::Selectable(patch_entries[n].display.c_str())) {
                                begin_archipelago(browsing_patch_for,
                                                  patch_entries[n].path);
                                browsing_patch_for = -1;
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndChild();
                } else {
                    ImGui::TextWrapped("Using Archipelago at %s",
                                       settings.ap_dir.c_str());
                    ImGui::Separator();
                    {
                        static char srv[96] = {0};
                        static bool srv_init = false;
                        if (!srv_init) {
                            std::snprintf(srv, sizeof srv, "%s",
                                          settings.ap_server.c_str());
                            srv_init = true;
                        }
                        ImGui::SetNextItemWidth(260.0f);
                        if (ImGui::InputText("Multiworld address", srv, sizeof srv)) {
                            settings.ap_server = srv;
                            remember();
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("(?)");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "Only needed for a seed generated locally. A "
                                "patch downloaded from a room carries its own "
                                "address and this is ignored.");
                    }
                    ImGui::Separator();
                    for (int p = 0; p < players; ++p) {
                        ImGui::PushID(5000 + p);
                        char lbl[24];
                        std::snprintf(lbl, sizeof lbl, "Player %d", p + 1);
                        ImGui::TextUnformatted(lbl);
                        ImGui::SameLine(90.0f);
                        const auto stage = machines[p].ap_stage.load();
                        const std::string note = machines[p].get_ap_note();
                        if (ImGui::Button(machines[p].ap_patch_path.empty()
                                              ? "Choose a patch..."
                                              : std::filesystem::path(
                                                    machines[p].ap_patch_path)
                                                    .filename().string().c_str(),
                                          ImVec2(300, 0))) {
                            browsing_patch_for = p;
                            patch_entries = gql::scan_roms(patch_dir);
                        }
                        // Only continue the line when there is actually
                        // something to put on it. A SameLine followed by
                        // nothing leaves the cursor where it is, and the next
                        // player's row lands beside this one.
                        if (stage != Machine::ApStage::Idle) {
                            ImGui::SameLine();
                            if (ImGui::Button("Clear")) clear_archipelago(p);
                            ImGui::SameLine();

                            const bool attached = machines[p].ap.client_connected();
                            const bool listening = machines[p].ap.listening();
                            const std::string fault = machines[p].ap.fault();
                            if (stage == Machine::ApStage::Failed) {
                                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f),
                                                   "%s", note.c_str());
                            } else if (!fault.empty()) {
                                // Takes precedence over "attached", which is
                                // true and beside the point: the client is
                                // connected and getting nowhere, and this says
                                // why.
                                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f),
                                                   "%s", fault.c_str());
                            } else if (attached) {
                                ImGui::TextColored(ImVec4(0.6f, 0.95f, 0.65f, 1.0f),
                                                   "attached on port %d",
                                                   machines[p].ap.port());
                            } else if (listening) {
                                ImGui::TextDisabled("%s", note.empty()
                                    ? "waiting for its client" : note.c_str());
                            } else if (machines[p].ap_wants_client) {
                                // Ports are handed out one at a time, so this
                                // player is behind somebody. Worth saying, or
                                // a slot that is merely queued looks stuck.
                                ImGui::TextDisabled("waiting for a free port");
                            } else {
                                ImGui::TextDisabled("%s", note.empty()
                                    ? Machine::stage_name(stage) : note.c_str());
                            }
                        }
                        ImGui::PopID();
                    }

                    // What the clients are saying, in the order they said it.
                    // Connecting to a multiworld goes wrong in a dozen ways —
                    // a version mismatch, a wrong slot name, a server that
                    // moved — and every one of them is a line here. Hiding it
                    // would mean the only symptom is a quadrant that never
                    // starts.
                    ImGui::Separator();
                    struct Tagged { unsigned long long seq; int player; std::string text; };
                    std::vector<Tagged> lines;
                    for (int p = 0; p < players; ++p)
                        for (const auto& l : machines[p].ap_session.recent())
                            lines.push_back(Tagged{l.seq, p, l.text});
                    std::sort(lines.begin(), lines.end(),
                              [](const Tagged& a, const Tagged& b) {
                                  return a.seq < b.seq;
                              });

                    if (lines.empty()) {
                        ImGui::TextDisabled(
                            "No client output yet. Choose a patch for a player "
                            "and what its client says will appear here.");
                    } else {
                        // BeginChild is paired with EndChild whatever it
                        // returns. Treating its result as a condition and
                        // ending unconditionally closes a scope that was never
                        // opened, which unwinds the tab bar and the window with
                        // it — the visible symptom being tabs that vanish and a
                        // complaint stuck to the mouse pointer.
                        ImGui::BeginChild("aplog", ImVec2(0, 0), true,
                                          ImGuiWindowFlags_HorizontalScrollbar);
                        static std::size_t shown = 0;
                        static const ImVec4 kPlayerColour[4] = {
                            {0.55f, 0.80f, 1.00f, 1.0f}, {1.00f, 0.80f, 0.50f, 1.0f},
                            {0.60f, 0.95f, 0.65f, 1.0f}, {1.00f, 0.60f, 0.70f, 1.0f},
                        };
                        for (const Tagged& t : lines) {
                            ImGui::TextColored(kPlayerColour[t.player & 3], "P%d",
                                               t.player + 1);
                            ImGui::SameLine();
                            ImGui::TextUnformatted(t.text.c_str());
                        }
                        // Follow the tail only while new lines are arriving, so
                        // scrolling back to read something does not fight it.
                        if (lines.size() != shown) {
                            shown = lines.size();
                            ImGui::SetScrollHereY(1.0f);
                        }
                        ImGui::EndChild();
                    }
                    }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Audio")) {
                ImGui::TextWrapped(
                    "Which machines you hear, and how loud. Four unrelated "
                    "games at once is noise, which is why one is the default - "
                    "but two people playing the same game is not, and neither "
                    "is wanting your own machine louder than the rest.");
                ImGui::Separator();
                for (int p = 0; p < players; ++p) {
                    ImGui::PushID(4000 + p);
                    char lbl[32];
                    std::snprintf(lbl, sizeof lbl, "Player %d", p + 1);
                    bool on = mixer.enabled(p);
                    if (ImGui::Checkbox(lbl, &on)) {
                        mixer.set_enabled(p, on);
                        settings.audio_on[p] = on;
                        remember();
                    }
                    ImGui::SameLine(120.0f);
                    float g = mixer.gain(p);
                    ImGui::BeginDisabled(!on);
                    ImGui::SetNextItemWidth(360.0f);
                    if (ImGui::SliderFloat("##gain", &g, 0.0f, 2.0f, "%.2fx")) {
                        mixer.set_gain(p, g);
                        settings.audio_gain[p] = g;
                        remember();
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("only")) {
                        for (int q = 0; q < players; ++q) {
                            mixer.set_enabled(q, q == p);
                            settings.audio_on[q] = (q == p);
                        }
                        remember();
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                ImGui::Text("%lu underruns, %lu dropped blocks",
                            mixer.underruns(), mixer.drops());
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Controls")) {

            if (ImGui::BeginTable("binds", players + 1,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableSetupColumn("Button");
                for (int p = 0; p < players; ++p) {
                    const char* pn = pads[p] ? SDL_GameControllerName(pads[p]) : nullptr;
                    char h[80];
                    std::snprintf(h, sizeof h, "P%d - %s", p + 1,
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
                            capturing ? "press..."
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
                "Controller - which physical pad drives each quadrant. "
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
            ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            }

            ImGui::Separator();
            // Escape closes the window; Game Mode has no keyboard and no title
            // bar, so this is the only way out from a controller.
            if (ImGui::Button("Quit")) g_quit.store(true);
            if (!status.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted(status.c_str());
            }
            ImGui::End();
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        mixer.pump();
        SDL_RenderPresent(ren);

        if (dump_rom_title) {
            static auto asked = std::chrono::steady_clock::now();
            static bool done = false;
            const auto t = std::chrono::steady_clock::now();
            if (!done && t - asked > std::chrono::seconds(3)) {
                for (int i = 0; i < players; ++i)
                    machines[i].want_title.store(true, std::memory_order_relaxed);
                done = true;
            }
            static bool printed = false;
            if (done && !printed && t - asked > std::chrono::seconds(4)) {
                printed = true;
                for (int i = 0; i < players; ++i) {
                    std::lock_guard<std::mutex> lk(machines[i].fb_mutex);
                    // Both: the cartridge's own title, and the offset
                    // Archipelago's Emerald client reads to identify a patch.
                    std::uint8_t ap[32] = {};
                    machines[i].gba.read_memory("ROM", 0x108, ap, sizeof ap);
                    std::string apname;
                    for (std::uint8_t c : ap) { if (!c) break; if (c >= 0x20 && c < 0x7F) apname.push_back(char(c)); }
                    std::printf("p%d title \"%s\"  ap@0x108 \"%s\"  "
                                "EWRAM %zu IWRAM %zu ROM %zu SRAM %zu\n",
                                i + 1, machines[i].title.c_str(), apname.c_str(),
                                machines[i].gba.memory_size("EWRAM"),
                                machines[i].gba.memory_size("IWRAM"),
                                machines[i].gba.memory_size("ROM"),
                                machines[i].gba.memory_size("Save RAM"));
                }
                std::fflush(stdout);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(5)) {
            std::printf("fps:");
            for (int i = 0; i < players; ++i) {
                std::printf(" p%d %5.2f %s/%s", i + 1, machines[i].fps.load(),
                            machines[i].mode == gql::LinkMode::Dolphin
                                ? (machines[i].gba.link_starved() ? "DOL?" : "dol")
                            : "cab",
                            gql::link_state_name(machines[i].link.load()));
                if (machines[i].mode == gql::LinkMode::Cable)
                    std::printf("[id%u pid%d dev%d sio%d rcnt%04X slp%lu]",
                                machines[i].gba.cable_id(),
                                machines[i].gba.cable_player_id(),
                                machines[i].gba.cable_devices(),
                                machines[i].gba.sio_mode(),
                                (unsigned)machines[i].gba.rcnt(),
                                machines[i].gba.cable_sleeps());
                    if (machines[i].ap.client_connected()) std::printf("[AP]");
                    if (machines[i].gba.cable_timeouts())
                        std::printf("[!%lu stalls]",
                                    machines[i].gba.cable_timeouts());
            }
            std::printf("   audio: %llu underruns, %llu drops\n",
                        (unsigned long long)mixer.underruns(),
                        (unsigned long long)mixer.drops());
            std::fflush(stdout);
            last_report = now;
        }
    }

    g_quit.store(true);
    // Before the joins, not after: a machine parked in a stalled run_frame()
    // never reaches the top of its loop to notice.
    for (int i = 0; i < players; ++i) {
        machines[i].ap_session.stop();
        if (machines[i].ap_thread.joinable()) machines[i].ap_thread.join();
        machines[i].ap.close();
        machines[i].gba.shutdown_link();
        machines[i].gba.wake_cable();   // release anyone parked on the cable
    }
    for (int i = 0; i < players; ++i)
        if (machines[i].thread.joinable()) machines[i].thread.join();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (textures[i]) SDL_DestroyTexture(textures[i]);
        if (pads[i]) SDL_GameControllerClose(pads[i]);
    }
    mixer.close();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

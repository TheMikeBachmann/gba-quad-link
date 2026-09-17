/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "controls.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gql {
namespace {

// Same order as the Button enum, which is the KEYINPUT bit order.
const char* const kButtonNames[kButtons] = {
    "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L",
};

// Past this, a stick counts as a direction press. Generous enough that a worn
// stick still registers, tight enough that resting drift does not.
constexpr int kAxisThreshold = 12000;

}  // namespace

const char* button_name(int button) {
    return (button >= 0 && button < kButtons) ? kButtonNames[button] : "?";
}

std::string Binding::label() const {
    switch (source) {
        case Source::None:
            return "(unbound)";
        case Source::PadButton: {
            const char* n = SDL_GameControllerGetStringForButton(
                static_cast<SDL_GameControllerButton>(code));
            return std::string("Pad ") + (n ? n : "?");
        }
        case Source::PadAxis: {
            const char* n = SDL_GameControllerGetStringForAxis(
                static_cast<SDL_GameControllerAxis>(code));
            return std::string("Axis ") + (n ? n : "?") + (dir > 0 ? "+" : "-");
        }
        case Source::Key: {
            const char* n = SDL_GetScancodeName(
                static_cast<SDL_Scancode>(code));
            return std::string("Key ") + (n && *n ? n : "?");
        }
    }
    return "(unbound)";
}

void Controls::reset_player(int p) {
    if (p < 0 || p >= kPlayers) return;
    PlayerControls& pc = player[p];
    pc.pad = -1;
    for (Binding& b : pc.buttons) b = Binding{};

    // Every player gets the standard pad layout. It applies as soon as a
    // controller is assigned and costs nothing until then.
    const struct { int button; SDL_GameControllerButton pad; } pad_map[] = {
        {BTN_A,      SDL_CONTROLLER_BUTTON_A},
        {BTN_B,      SDL_CONTROLLER_BUTTON_B},
        {BTN_SELECT, SDL_CONTROLLER_BUTTON_BACK},
        {BTN_START,  SDL_CONTROLLER_BUTTON_START},
        {BTN_RIGHT,  SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
        {BTN_LEFT,   SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {BTN_UP,     SDL_CONTROLLER_BUTTON_DPAD_UP},
        {BTN_DOWN,   SDL_CONTROLLER_BUTTON_DPAD_DOWN},
        {BTN_R,      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {BTN_L,      SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    };
    for (const auto& m : pad_map) {
        pc.buttons[m.button].source = Binding::Source::PadButton;
        pc.buttons[m.button].code = m.pad;
    }
}

void Controls::reset_to_defaults() {
    for (int p = 0; p < kPlayers; ++p) reset_player(p);

    // Player one also answers to the keyboard, so the runner is usable by one
    // person with no controller at all. The d-pad bindings are left on the pad;
    // the keyboard equivalents are added as the directional bindings instead,
    // since a player using the keyboard has no stick to fall back on.
    const struct { int button; SDL_Scancode key; } keys[] = {
        {BTN_A, SDL_SCANCODE_X},        {BTN_B, SDL_SCANCODE_Z},
        {BTN_SELECT, SDL_SCANCODE_RSHIFT}, {BTN_START, SDL_SCANCODE_RETURN},
        {BTN_RIGHT, SDL_SCANCODE_RIGHT}, {BTN_LEFT, SDL_SCANCODE_LEFT},
        {BTN_UP, SDL_SCANCODE_UP},      {BTN_DOWN, SDL_SCANCODE_DOWN},
        {BTN_R, SDL_SCANCODE_C},        {BTN_L, SDL_SCANCODE_V},
    };
    for (const auto& k : keys) {
        player[0].buttons[k.button].source = Binding::Source::Key;
        player[0].buttons[k.button].code = k.key;
    }
}

uint16_t Controls::read(int p, SDL_GameController* pad,
                        const uint8_t* keyboard) const {
    uint16_t keys = 0x03FF;  // active low: nothing held
    if (p < 0 || p >= kPlayers) return keys;
    const PlayerControls& pc = player[p];

    for (int b = 0; b < kButtons; ++b) {
        const Binding& bind = pc.buttons[b];
        bool down = false;
        switch (bind.source) {
            case Binding::Source::None:
                break;
            case Binding::Source::PadButton:
                down = pad && SDL_GameControllerGetButton(
                           pad, static_cast<SDL_GameControllerButton>(bind.code));
                break;
            case Binding::Source::PadAxis:
                if (pad) {
                    const int v = SDL_GameControllerGetAxis(
                        pad, static_cast<SDL_GameControllerAxis>(bind.code));
                    down = bind.dir > 0 ? v > kAxisThreshold
                                        : v < -kAxisThreshold;
                }
                break;
            case Binding::Source::Key:
                down = keyboard && keyboard[bind.code];
                break;
        }
        if (down) keys &= static_cast<uint16_t>(~(1u << b));
    }
    return keys;
}

bool load_controls(const std::string& path, Controls* out) {
    std::ifstream f(path);
    if (!f) return false;

    // player <n> pad <instance-id>
    // player <n> <button-index> <source> <code> <dir>
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string tag;
        int p = -1;
        if (!(in >> tag >> p) || tag != "player") continue;
        if (p < 0 || p >= kPlayers) continue;

        std::string what;
        if (!(in >> what)) continue;
        if (what == "pad") {
            int id = -1;
            if (in >> id) out->player[p].pad = id;
            continue;
        }
        const int b = std::atoi(what.c_str());
        if (b < 0 || b >= kButtons) continue;
        int src = 0, code = 0, dir = 0;
        if (!(in >> src >> code >> dir)) continue;
        if (src < 0 || src > 3) continue;
        Binding& bind = out->player[p].buttons[b];
        bind.source = static_cast<Binding::Source>(src);
        bind.code = code;
        bind.dir = dir;
    }
    return true;
}

bool save_controls(const std::string& path, const Controls& in) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# gba-quad-link four-player controls.\n"
         "# player <n> pad <sdl-instance-id>\n"
         "# player <n> <button> <source 0=none 1=padbutton 2=padaxis 3=key>"
         " <code> <axis-dir>\n";
    for (int p = 0; p < kPlayers; ++p) {
        f << "player " << p << " pad " << in.player[p].pad << "\n";
        for (int b = 0; b < kButtons; ++b) {
            const Binding& bind = in.player[p].buttons[b];
            f << "player " << p << " " << b << " "
              << static_cast<int>(bind.source) << " " << bind.code << " "
              << bind.dir << "\n";
        }
    }
    return static_cast<bool>(f);
}

}  // namespace gql

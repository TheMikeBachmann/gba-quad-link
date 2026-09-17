/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// controls.h — what each player presses, and how it is stored.
//
// A player's ten GBA buttons are bound independently, and each binding may come
// from a controller button, a controller stick direction, or a keyboard key.
// Mixing matters in practice: with fewer working pads than players, a keyboard
// player fills a seat.

#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <string>

namespace gql {

constexpr int kPlayers = 4;
constexpr int kButtons = 10;

// GBA keypad order. The index is the bit position in KEYINPUT, which is active
// low — a pressed button clears its bit.
enum Button {
    BTN_A = 0, BTN_B, BTN_SELECT, BTN_START,
    BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN,
    BTN_R, BTN_L,
};

const char* button_name(int button);

struct Binding {
    enum class Source : uint8_t { None, PadButton, PadAxis, Key };
    Source source = Source::None;
    int code = 0;   // SDL_GameControllerButton, SDL_GameControllerAxis, or SDL_Scancode
    int dir = 0;    // axis only: +1 past the positive threshold, -1 past the negative

    bool bound() const { return source != Source::None; }
    // "Pad A", "Stick Left", "Key X", "—"
    std::string label() const;
};

struct PlayerControls {
    // Which attached controller drives this player, by SDL joystick instance id.
    // -1 means no pad; keyboard bindings still work.
    SDL_JoystickID pad = -1;
    Binding buttons[kButtons];
};

struct Controls {
    PlayerControls player[kPlayers];

    // Player 1 gets the keyboard and the rest wait for a pad, which is the
    // arrangement that makes a fresh install usable by one person.
    void reset_to_defaults();
    void reset_player(int player);

    // Active-low KEYINPUT for one player: 0x03FF with a bit cleared per press.
    uint16_t read(int player, SDL_GameController* pad_for_player,
                  const uint8_t* keyboard) const;
};

// Config lives beside the executable so a build directory carries its own
// settings. Both report success; a missing file on load is not a failure, it
// just leaves the defaults in place.
bool load_controls(const std::string& path, Controls* out);
bool save_controls(const std::string& path, const Controls& in);

}  // namespace gql

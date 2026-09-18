/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// settings.h — the handful of things worth remembering between sessions.
//
// Not the controls, which have their own file and their own format. These are
// the answers to questions nobody should be asked twice: where the cartridges
// live, and which machine Dolphin is on. Both are long strings that are
// miserable to type on a television with a controller, and both are the same
// every time for a given setup.

#pragma once

#include <string>

namespace gql {

struct Settings {
    std::string rom_dir;   // where the cartridge library lives
    std::string host;      // the machine Dolphin runs on

    // Which machines are heard, and how loudly. One machine at full volume is
    // the arrangement that makes sense on a first run — four unrelated games
    // at once really is noise — but it is a default, not a rule.
    bool audio_on[4] = {true, false, false, false};
    float audio_gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Archipelago: where it is installed, and the multiworld to join. The
    // address is remembered because a locally generated patch does not carry
    // one and nobody wants to type it twice.
    std::string ap_dir;
    std::string ap_server;
    std::string patch_dir;
};

// Missing file is not a failure; it just leaves the defaults alone.
bool load_settings(const std::string& path, Settings* out);
bool save_settings(const std::string& path, const Settings& in);

}  // namespace gql

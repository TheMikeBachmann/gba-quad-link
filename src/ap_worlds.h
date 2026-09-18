/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ap_worlds.h — working out where a game keeps its cartridge setting.
//
// Archipelago asks for a base cartridge with a file dialog the first time a
// game is used, and remembers the answer in host.yaml under a key belonging to
// that game. Since we already know which cartridge it wants — the patch says
// so, by checksum — the dialog is pure friction and can be answered in advance.
//
// The key is derivable. Archipelago builds it from the world's folder name
// plus "_options" unless the world declares its own, which Pokemon Emerald
// does and most do not. A world is either a directory or an .apworld, which is
// a zip, so both the folder name and any declared override can be read without
// running anything.

#pragma once

#include <string>
#include <vector>

namespace gql {

struct ApWorld {
    std::string game;          // "Pokemon Emerald", as a patch manifest names it
    std::string folder;        // "pokemon_emerald"
    std::string settings_key;  // "pokemon_emerald_settings" or "mzm_options"
    std::string rom_field;     // "rom_file" almost always
};

// Finds the world a patch belongs to, by the game name its manifest carries.
//
// A frozen Archipelago ships compiled bytecode rather than source, so the
// world's own declaration cannot simply be read. What can be read is what
// Archipelago has already written: it puts a stub in host.yaml for every world
// it knows, under that world's real key and with the real name of its
// cartridge field. Reading what it wrote is both easier and more honest than
// inferring what it would have written.
//
// The game name is still found in the world's files — bytecode keeps its
// string constants verbatim — which is what identifies the folder.
bool find_ap_world(const std::string& ap_dir, const std::string& game,
                   ApWorld* out);

// Points a game's cartridge setting at `rom`, editing the existing entry if
// there is one and adding it if not. False with a reason on failure.
//
// Editing rather than appending matters: Archipelago writes a stub with a
// default filename for every world it knows about, so the key is usually
// already there, and a second copy of it is not read.
bool set_ap_rom_path(const std::string& ap_dir, const ApWorld& world,
                     const std::string& rom, std::string* err);

}  // namespace gql

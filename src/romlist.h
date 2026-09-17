/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// romlist.h — the cartridges a player can choose from.
//
// A real library is a directory of archives with names like
// "Advance Wars (USA) (Rev 1).7z", nine hundred of them, and someone picking
// one from a sofa with a controller is not going to scroll. So the list is
// scanned once, sorted, and filtered by typing.

#pragma once

#include <string>
#include <vector>

namespace gql {

struct RomEntry {
    std::string path;      // what to hand to GbaInstance::open
    std::string display;   // the file name without extension or directory
};

// Everything in `dir` that might be a cartridge, sorted by display name.
// Recognises .gba and the archive formats mGBA can see inside; whether an
// archive actually holds a ROM is not known until it is opened, which is too
// slow to do for a whole library up front.
std::vector<RomEntry> scan_roms(const std::string& dir);

// Indices of the entries matching `needle`, which matches case-insensitively
// and on any part of the name. An empty needle matches everything.
std::vector<int> filter_roms(const std::vector<RomEntry>& roms,
                             const std::string& needle);

// One directory, for walking a filesystem with a controller. Typing a path on
// a television is not a thing anyone should be asked to do.
struct DirEntry {
    std::string name;   // what to show
    std::string path;   // where it goes
};

// Subdirectories of `dir`, sorted, with unreadable ones left out. The parent
// is not included; ask for it separately.
std::vector<DirEntry> list_subdirs(const std::string& dir);

// The containing directory, or an empty string at the root.
std::string parent_dir(const std::string& dir);

// How many cartridges are directly in `dir`, for telling someone they have
// found the right folder before they commit to it.
int count_roms(const std::string& dir);

// Sensible places to start from: home, anything mounted as removable media,
// and the root. A cartridge library is nearly always on a memory card.
std::vector<DirEntry> quick_roots();

// Where to look when the user has not said. The first of these that exists and
// holds something: $GQL_ROM_DIR, a "roms" directory beside the executable or
// the AppImage, $XDG_DATA_HOME/gba-quad-link/roms, ~/roms.
std::string default_rom_dir();

}  // namespace gql

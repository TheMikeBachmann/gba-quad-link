/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ap_patch.h — what an Archipelago patch file says about itself.
//
// A patch is a zip carrying an `archipelago.json` manifest, and that manifest
// is the reason this can be pleasant to use: it names the game, the slot, the
// server, and the checksum of the cartridge it expects to be applied to. So
// picking a patch file is enough — everything else is either in there or
// findable from it.
//
// One caveat worth knowing before designing around it: a patch generated on
// the website carries a real server address, and one generated locally carries
// an empty string. The address has to be asked for in the second case.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gql {

struct ApPatch {
    bool valid = false;
    std::string game;            // "Pokemon Emerald"
    std::string player_name;     // the slot to connect as
    std::string server;          // often empty; see above
    std::string base_checksum;   // md5 of the cartridge this applies to
    std::string result_ending;   // ".gba"
    int player = 0;
};

// Reads the manifest out of a patch file. False if it is not one.
bool read_ap_patch(const std::string& path, ApPatch* out);

// Lowercase hex md5 of a file, or empty if it cannot be read.
std::string md5_of_file(const std::string& path);

// Finds a cartridge in `dir` whose md5 matches, looking inside .7z and .zip
// archives as well as at plain files, and extracts it to `cache_dir` if it was
// in an archive. Returns the path to a real file on disk, or empty.
//
// Archipelago needs an actual file to patch, and a real library is archives,
// so something has to unpack. `progress` is called with each name as it is
// examined, because hashing nine hundred cartridges is not instant.
// `game` is the name from the manifest, used to try the likely cartridges
// first — hashing a whole library takes over a minute and the answer is nearly
// always the file whose name resembles the game.
std::string find_base_rom(const std::string& dir, const std::string& checksum,
                          const std::string& cache_dir,
                          const std::string& game = {},
                          void (*progress)(const std::string&, int, int) = nullptr);

}  // namespace gql

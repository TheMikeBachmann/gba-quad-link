/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "settings.h"

#include <fstream>
#include <sstream>

namespace gql {

bool load_settings(const std::string& path, Settings* out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        // Not trimmed: a path may legitimately end in a space, and the value
        // runs to the end of the line precisely so it can contain anything but
        // a newline. Cartridge directories have brackets and spaces in them.
        const std::string value = line.substr(eq + 1);
        if (key == "rom_dir") out->rom_dir = value;
        else if (key == "host") out->host = value;
    }
    return true;
}

bool save_settings(const std::string& path, const Settings& in) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# gba-quad-link settings. Values run to the end of the line.\n";
    if (!in.rom_dir.empty()) f << "rom_dir=" << in.rom_dir << "\n";
    if (!in.host.empty()) f << "host=" << in.host << "\n";
    return static_cast<bool>(f);
}

}  // namespace gql

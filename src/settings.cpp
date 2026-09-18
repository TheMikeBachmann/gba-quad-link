/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "settings.h"

#include <fstream>
#include <cstdlib>
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
        else if (key == "ap_dir") out->ap_dir = value;
        else if (key == "ap_server") out->ap_server = value;
        else if (key == "patch_dir") out->patch_dir = value;
        else if (key.rfind("audio_on", 0) == 0 && key.size() == 9) {
            const int i = key[8] - '1';
            if (i >= 0 && i < 4) out->audio_on[i] = (value == "1");
        } else if (key.rfind("audio_gain", 0) == 0 && key.size() == 11) {
            const int i = key[10] - '1';
            if (i >= 0 && i < 4) out->audio_gain[i] = std::strtof(value.c_str(), nullptr);
        }
    }
    return true;
}

bool save_settings(const std::string& path, const Settings& in) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# gba-quad-link settings. Values run to the end of the line.\n";
    if (!in.rom_dir.empty()) f << "rom_dir=" << in.rom_dir << "\n";
    if (!in.host.empty()) f << "host=" << in.host << "\n";
    if (!in.ap_dir.empty()) f << "ap_dir=" << in.ap_dir << "\n";
    if (!in.ap_server.empty()) f << "ap_server=" << in.ap_server << "\n";
    if (!in.patch_dir.empty()) f << "patch_dir=" << in.patch_dir << "\n";
    for (int i = 0; i < 4; ++i) {
        f << "audio_on" << (i + 1) << "=" << (in.audio_on[i] ? 1 : 0) << "\n";
        f << "audio_gain" << (i + 1) << "=" << in.audio_gain[i] << "\n";
    }
    return static_cast<bool>(f);
}

}  // namespace gql

/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "romlist.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include "paths.h"

namespace gql {
namespace {

namespace fs = std::filesystem;

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool looks_like_rom(const fs::path& p) {
    const std::string ext = lower(p.extension().string());
    return ext == ".gba" || ext == ".7z" || ext == ".zip";
}

}  // namespace

std::vector<RomEntry> scan_roms(const std::string& dir) {
    std::vector<RomEntry> out;
    if (dir.empty()) return out;

    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return out;

    for (const fs::directory_entry& e : it) {
        if (!e.is_regular_file(ec) || !looks_like_rom(e.path())) continue;
        out.push_back(RomEntry{e.path().string(), e.path().stem().string()});
    }
    std::sort(out.begin(), out.end(),
              [](const RomEntry& a, const RomEntry& b) {
                  return lower(a.display) < lower(b.display);
              });
    return out;
}

std::vector<int> filter_roms(const std::vector<RomEntry>& roms,
                             const std::string& needle) {
    std::vector<int> out;
    const std::string want = lower(needle);
    for (int i = 0; i < static_cast<int>(roms.size()); ++i) {
        if (want.empty() ||
            lower(roms[i].display).find(want) != std::string::npos)
            out.push_back(i);
    }
    return out;
}

std::string default_rom_dir() {
    std::vector<fs::path> candidates;
    if (const char* d = env("GQL_ROM_DIR")) candidates.emplace_back(d);
    if (const char* img = env("APPIMAGE")) {
        const fs::path p(img);
        if (p.has_parent_path()) candidates.push_back(p.parent_path() / "roms");
    }
    const std::string data = data_dir();
    if (!data.empty()) candidates.push_back(fs::path(data) / "roms");
    candidates.emplace_back("roms");
    if (const char* home = env("HOME")) candidates.push_back(fs::path(home) / "roms");

    std::error_code ec;
    for (const fs::path& c : candidates)
        if (fs::is_directory(c, ec)) return c.string();
    return {};
}

}  // namespace gql

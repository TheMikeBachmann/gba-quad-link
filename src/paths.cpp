/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "paths.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace gql {
namespace {

namespace fs = std::filesystem;

// Read an environment variable, treating unset and empty as the same thing —
// an empty XDG_* is defined to mean "unset", and an empty APPDIR is worse than
// useless because it turns into a path rooted at "/".
const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// Every directory an asset might live in, in the order they are tried.
std::vector<fs::path> asset_dirs() {
    std::vector<fs::path> dirs;
    if (const char* d = env("GQL_DATA_DIR")) dirs.emplace_back(d);
    if (const char* d = env("XDG_DATA_HOME"))
        dirs.emplace_back(fs::path(d) / "gba-quad-link");
    else if (const char* home = env("HOME"))
        dirs.emplace_back(fs::path(home) / ".local" / "share" / "gba-quad-link");
    // $APPIMAGE is the path of the .AppImage file itself, set by its runtime.
    // Looking beside it is what makes "keep the ROM next to the image" work.
    if (const char* img = env("APPIMAGE")) {
        const fs::path p(img);
        if (p.has_parent_path()) dirs.push_back(p.parent_path());
    }
    // $APPDIR is the mounted image. Assets bundled at package time land here.
    if (const char* app = env("APPDIR"))
        dirs.emplace_back(fs::path(app) / "usr" / "share" / "gba-quad-link");
    return dirs;
}

}  // namespace

std::string config_dir() {
    fs::path dir;
    if (const char* d = env("XDG_CONFIG_HOME")) dir = fs::path(d) / "gba-quad-link";
    else if (const char* home = env("HOME"))
        dir = fs::path(home) / ".config" / "gba-quad-link";
    else return {};

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {};
    return dir.string();
}

std::string data_dir() {
    fs::path dir;
    if (const char* d = env("XDG_DATA_HOME"))
        dir = fs::path(d) / "gba-quad-link";
    else if (const char* home = env("HOME"))
        dir = fs::path(home) / ".local" / "share" / "gba-quad-link";
    else return {};

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {};
    return dir.string();
}

std::string save_path(const std::string& rom_path, int player) {
    if (rom_path.empty()) return {};    // no cartridge, nothing to save
    const std::string base = data_dir();
    if (base.empty()) return {};

    std::error_code ec;
    const fs::path dir = fs::path(base) / "saves";
    fs::create_directories(dir, ec);
    if (ec) return {};

    fs::path name = fs::path(rom_path).stem();
    if (name.empty()) name = "cartridge";
    name += ".p" + std::to_string(player + 1) + ".sav";
    return (dir / name).string();
}

std::string find_asset(const std::string& preferred) {
    std::error_code ec;
    if (!preferred.empty() && fs::exists(preferred, ec)) return preferred;

    const fs::path name = fs::path(preferred).filename();
    if (name.empty()) return preferred;

    for (const fs::path& dir : asset_dirs()) {
        const fs::path candidate = dir / name;
        if (fs::exists(candidate, ec)) return candidate.string();
    }
    return preferred;
}


std::string asset_search_description() {
    std::string out;
    for (const fs::path& dir : asset_dirs()) {
        if (!out.empty()) out += "\n  ";
        out += dir.string();
    }
    return out.empty() ? std::string("(nowhere — no HOME set)") : out;
}

}  // namespace gql

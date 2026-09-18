/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ap_worlds.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <map>
#include <sstream>

#include "ap_patch.h"

namespace gql {
namespace {

namespace fs = std::filesystem;

}  // namespace

namespace {

// Every key in host.yaml, with the cartridge field each one declares.
std::map<std::string, std::string> host_yaml_keys(const std::string& ap_dir) {
    std::map<std::string, std::string> out;
    std::ifstream f(fs::path(ap_dir) / "host.yaml");
    std::string line, key;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] != ' ' && line[0] != '\t' &&
            line[0] != '#' && line.back() == ':') {
            key = line.substr(0, line.size() - 1);
            continue;
        }
        if (key.empty()) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string field = line.substr(0, colon);
        field.erase(0, field.find_first_not_of(" \t"));
        if (field.find("rom") != std::string::npos && out.find(key) == out.end())
            out[key] = field;
    }
    return out;
}

// Does this world's code mention the game? Bytecode keeps string constants
// verbatim, so a plain search works on both source and .pyc.
bool mentions(const std::string& blob, const std::string& game) {
    return blob.find(game) != std::string::npos;
}

std::string read_world_blob(const fs::path& entry, std::string* folder) {
    std::error_code ec;
    if (fs::is_directory(entry, ec)) {
        *folder = entry.filename().string();
        for (const char* n : {"__init__.py", "__init__.pyc"}) {
            std::ifstream f(entry / n, std::ios::binary);
            if (!f) continue;
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
        return {};
    }
    if (entry.extension() != ".apworld") return {};
    *folder = entry.stem().string();
    std::string init;
    for (const std::string& n : zip_list(entry.string())) {
        // Each length checked against its own suffix: a name of exactly
        // twelve characters passes a single "longer than eleven" guard and
        // then subtracts thirteen from it.
        const auto ends_with = [&n](const char* suffix) {
            const std::size_t len = std::char_traits<char>::length(suffix);
            return n.size() >= len && n.compare(n.size() - len, len, suffix) == 0;
        };
        const bool is_init = ends_with("/__init__.py") || ends_with("/__init__.pyc");
        if (is_init && std::count(n.begin(), n.end(), '/') == 1) {
            *folder = n.substr(0, n.find('/'));
            init = n;
            break;
        }
    }
    if (init.empty()) return {};
    std::string src;
    return zip_read(entry.string(), init, &src) ? src : std::string{};
}

}  // namespace

bool find_ap_world(const std::string& ap_dir, const std::string& game,
                   ApWorld* out) {
    if (ap_dir.empty() || game.empty()) return false;
    std::error_code ec;
    const auto keys = host_yaml_keys(ap_dir);

    for (const char* sub : {"custom_worlds", "lib/worlds", "worlds"}) {
        fs::directory_iterator it(fs::path(ap_dir) / sub, ec);
        if (ec) { ec.clear(); continue; }
        for (const fs::directory_entry& e : it) {
            std::string folder;
            const std::string blob = read_world_blob(e.path(), &folder);
            if (blob.empty() || folder.empty() || !mentions(blob, game)) continue;

            out->game = game;
            out->folder = folder;

            // What Archipelago actually wrote, preferred over what the naming
            // rule predicts — Pokemon Emerald declares its own key and most
            // worlds do not.
            out->settings_key.clear();
            for (const std::string& guess :
                 {folder + "_options", folder + "_settings"}) {
                const auto k = keys.find(guess);
                if (k != keys.end()) {
                    out->settings_key = k->first;
                    out->rom_field = k->second;
                    break;
                }
            }
            if (out->settings_key.empty()) {
                for (const auto& [k, field] : keys) {
                    if (k.rfind(folder + "_", 0) == 0) {
                        out->settings_key = k;
                        out->rom_field = field;
                        break;
                    }
                }
            }
            if (out->settings_key.empty()) {
                // Never used, so nothing has been written for it yet. This is
                // the rule Archipelago applies when a world says nothing.
                out->settings_key = folder + "_options";
                out->rom_field = "rom_file";
            }
            return true;
        }
    }
    return false;
}

bool set_ap_rom_path(const std::string& ap_dir, const ApWorld& world,
                     const std::string& rom, std::string* err) {
    const fs::path cfg = fs::path(ap_dir) / "host.yaml";
    std::ifstream in(cfg);
    if (!in) {
        if (err) *err = "cannot read host.yaml";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    in.close();

    // A value containing a colon and spaces has to be quoted, and a path can
    // contain a quote.
    std::string safe = rom;
    for (std::size_t i = 0; (i = safe.find('"', i)) != std::string::npos; i += 2)
        safe.insert(i, "\\");

    const std::regex existing(
        "(^" + world.settings_key + ":\\n(?:[ \\t]*#[^\\n]*\\n)*[ \\t]*" +
        world.rom_field + ": )\"[^\"]*\"",
        std::regex::multiline);
    std::smatch m;
    if (std::regex_search(s, m, existing)) {
        s = std::regex_replace(s, existing, "$1\"" + safe + "\"");
    } else {
        if (!s.empty() && s.back() != '\n') s.push_back('\n');
        s += "\n" + world.settings_key + ":\n  " + world.rom_field +
             ": \"" + safe + "\"\n";
    }

    std::ofstream out(cfg, std::ios::trunc);
    if (!out) {
        if (err) *err = "cannot write host.yaml";
        return false;
    }
    out << s;
    return static_cast<bool>(out);
}

}  // namespace gql

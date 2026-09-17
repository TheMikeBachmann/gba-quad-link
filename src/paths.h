/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// paths.h — where the app reads its assets and writes its settings.
//
// Run from the build directory, everything is relative to the repo and none of
// this matters. Packaged as an AppImage it all changes: the image is read-only,
// so nothing can be written next to the binary, and the working directory is
// wherever the user happened to launch it from, so nothing can be read relative
// to it either. Both ends need somewhere real to point at.

#pragma once

#include <string>

namespace gql {

// Directory for the files the app writes — bound controls, window layout.
// $XDG_CONFIG_HOME/gba-quad-link, or ~/.config/gba-quad-link. Created if it does not
// exist. Empty if neither variable is usable, in which case the caller should
// fall back to the current directory and accept that it may be read-only.
std::string config_dir();

// Directory for the larger things the app writes — battery saves.
// $XDG_DATA_HOME/gba-quad-link, or ~/.local/share/gba-quad-link. Created if it
// does not exist, empty if there is nowhere usable.
std::string data_dir();

// Where a player's battery save lives, for a given cartridge.
//
// Per player, not per cartridge, and that is deliberate. Four people playing
// the same four-player cable game each have their own copy of it with their
// own save on it; pointing four cores at one file would have them overwrite
// each other's progress in a way that looks like corruption rather than like a
// mistake. Empty if there is nowhere to write, in which case the game runs
// with a save that is not kept.
std::string save_path(const std::string& rom_path, int player);

// Resolve a read-only asset: the GBA BIOS image, a ROM.
//
// `preferred` is taken as given if it names something that exists, so an
// explicit --rom and the repo-relative defaults both keep working untouched.
// Otherwise the file's base name is looked for in the places a packaged build
// can reach, nearest-to-the-user first:
//
//   $GQL_DATA_DIR       an explicit override
//   $XDG_DATA_HOME/gba-quad-link  or ~/.local/share/gba-quad-link
//   alongside the AppImage  the directory holding $APPIMAGE
//   inside the AppImage     $APPDIR/usr/share/gba-quad-link
//
// Returns `preferred` unchanged when nothing is found, so the caller reports
// the path the user asked for rather than the last one searched.
std::string find_asset(const std::string& preferred);

// The places find_asset() looked, for an error message worth reading.
std::string asset_search_description();


}  // namespace gql

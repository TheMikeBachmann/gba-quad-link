/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ap_fetch.h — installing Archipelago, so nobody has to go and find it.
//
// This is the one step the rest of the Archipelago support could not do for
// you. Everything after it — the cartridge, the world's settings, the client,
// the connector — happens on its own once a patch file is chosen, but only if
// there is an Archipelago to run, and until now that meant leaving to fetch
// one by hand.
//
// It is deliberately behind a button rather than automatic. Ninety megabytes
// is a lot to spend on somebody's connection because they opened a tab to see
// what was on it, and what arrives is a program that then gets run.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace gql {

class ApInstall {
public:
    ~ApInstall();
    ApInstall(const ApInstall&) = delete;
    ApInstall& operator=(const ApInstall&) = delete;
    ApInstall() = default;

    enum class Stage { Idle, Asking, Downloading, Unpacking, Migrating,
                       Done, Failed };

    // Starts in the background. `dest` is the folder to install into; on
    // success `installed()` is the directory holding ArchipelagoBizHawkClient.
    //
    // `migrate_from` is an existing install whose user state should be
    // carried across — empty for a first install. The new copy is staged
    // beside the old one and only put in place once it has unpacked and been
    // migrated, so a failed or cancelled update leaves what was working
    // exactly where it was.
    void start(const std::string& dest, const std::string& migrate_from = {});
    void cancel();

    Stage stage() const { return stage_.load(std::memory_order_relaxed); }
    // 0 to 1, or negative while the size is still unknown.
    float progress() const { return progress_.load(std::memory_order_relaxed); }
    std::string note() const;
    std::string installed() const;
    bool busy() const;

private:
    void run(std::string dest, std::string migrate_from);

    std::atomic<Stage> stage_{Stage::Idle};
    std::atomic<float> progress_{-1.0f};
    std::atomic<bool> cancel_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string note_;
    std::string installed_;
};

// Asks, once, in the background, what the newest release is. Kept apart from
// the install so that opening the tab costs one small request and nothing
// else, and so a machine with no network simply never offers an update
// rather than stalling on one.
class ApLatest {
public:
    ~ApLatest();
    ApLatest(const ApLatest&) = delete;
    ApLatest& operator=(const ApLatest&) = delete;
    ApLatest() = default;

    // Cheap to call every frame; it only ever starts one request.
    void check();
    bool known() const { return done_.load(std::memory_order_relaxed); }
    std::string tag() const;

private:
    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> done_{false};
    mutable std::mutex mutex_;
    std::string tag_;
};

// The newest release's Linux AppImage. Separate from the class so it can be
// tested without downloading ninety megabytes. `tag` is the version it is.
bool find_ap_release(std::string* url, std::string* filename, std::string* tag,
                     std::string* err);

// What is installed, from its manifest — "0.6.7", or empty if it cannot be
// read. Comparing versions is numeric per component, because "0.6.10" is
// newer than "0.6.9" and sorts before it as text.
std::string installed_ap_version(const std::string& ap_dir);
bool version_is_newer(const std::string& candidate, const std::string& current);

// The parts of an Archipelago install that belong to whoever set it up, and
// so have to survive an update: the settings naming each world's cartridge,
// community worlds that no release ships, generated seeds, and player
// configuration. Everything else comes from the release.
void migrate_ap_state(const std::string& from, const std::string& to);

}  // namespace gql

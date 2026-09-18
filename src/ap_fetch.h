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

    enum class Stage { Idle, Asking, Downloading, Unpacking, Done, Failed };

    // Starts in the background. `dest` is the folder to install into; on
    // success `installed()` is the directory holding ArchipelagoBizHawkClient.
    void start(const std::string& dest);
    void cancel();

    Stage stage() const { return stage_.load(std::memory_order_relaxed); }
    // 0 to 1, or negative while the size is still unknown.
    float progress() const { return progress_.load(std::memory_order_relaxed); }
    std::string note() const;
    std::string installed() const;
    bool busy() const;

private:
    void run(std::string dest);

    std::atomic<Stage> stage_{Stage::Idle};
    std::atomic<float> progress_{-1.0f};
    std::atomic<bool> cancel_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string note_;
    std::string installed_;
};

// The newest release's Linux AppImage. Separate from the class so it can be
// tested without downloading ninety megabytes.
bool find_ap_release(std::string* url, std::string* filename, std::string* err);

}  // namespace gql

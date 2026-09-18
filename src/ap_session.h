/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ap_session.h — running somebody else's program on a player's behalf.
//
// An Archipelago game client is a separate process. It knows how to patch a
// cartridge, speak to a multiworld, and drive an emulator through a socket —
// all of which would be months of work to reimplement and would then be a
// second thing to keep in step with every Archipelago release. So it is run,
// hidden, one per player, and this is what holds the other end of it.
//
// Two things it must get right, because neither is forgiving. The client is
// told where to connect by writing to its standard input, because a patch
// generated locally carries no server address and --connect is ignored when a
// patch file is given. And it must be killed as a process group: it starts
// children of its own, and orphans hold the ports that the next run needs.

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

namespace gql {

class ApSession {
public:
    ApSession() = default;
    ~ApSession();
    ApSession(const ApSession&) = delete;
    ApSession& operator=(const ApSession&) = delete;

    // `ap_dir` holds ArchipelagoBizHawkClient. `server` may be empty, in which
    // case whatever the patch carries is used. False with a reason in `err`.
    // `expected_rom` is where the client will write the patched cartridge —
    // derivable from the patch's name and the manifest's result ending, so it
    // does not have to be scraped out of the log.
    bool start(const std::string& ap_dir, const std::string& patch,
               const std::string& server, const std::string& expected_rom,
               std::string* err);
    void stop();
    bool running() const;

    // Where the client wrote the patched cartridge, once it has. Empty until
    // then, and empty if patching failed.
    std::string produced_rom() const;

    // A short description of what the client is doing, for the status gutter,
    // and the recent output behind it for the menu. Deliberately coarse: this
    // is scraped from log lines, which are not an interface and change between
    // releases.
    std::string status() const;

    // Recent output, each line tagged with the order it arrived in. The tag is
    // process-wide, so several players' logs can be put back into the order
    // things actually happened — which is the only way a line about one player
    // connecting makes sense next to a line about another.
    struct Line { unsigned long long seq; std::string text; };
    std::vector<Line> recent() const;

private:
    void pump();   // reads the client's output

    int pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> quit_{false};

    std::string patch_;
    mutable std::mutex mutex_;
    std::string status_ = "not started";
    std::string rom_;
    std::deque<Line> recent_;
};

}  // namespace gql

/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ap_connector.h — one machine's end of an Archipelago game client.
//
// Archipelago's GBA games do not talk to an emulator; they talk to a connector
// socket, scanning 127.0.0.1 ports 43055 upwards for one that answers. Five
// ports, four machines — so each quadrant has a port of its own and four game
// clients attach to four guests without any of them knowing the others exist.
//
// Which is why a connector never opens or reclaims its own port. Nothing in
// that scan tells a client which player it belongs to; it simply takes the
// lowest port that answers. Two connectors listening at the same time is
// therefore a race between two clients for the lower one, so the decision to
// listen belongs to whoever can see all four machines, and is made one at a
// time. See the coordinator in main.cpp.
//
// Threading follows the rule the rest of the program runs on: a core belongs
// to the thread driving it. The socket lives on its own thread and never
// touches a guest. It parks a batch of requests where the machine can see
// them, the machine executes the whole batch at one point in emulated time —
// which is what makes a guarded read atomic — and the socket thread wakes and
// replies.

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "minijson.h"

namespace gql {

class GbaInstance;

// What the client scans. Five, which is one more than we can use.
constexpr int kApPortFirst = 43055;
constexpr int kApPortCount = 5;

class ApConnector {
public:
    ~ApConnector();
    ApConnector(const ApConnector&) = delete;
    ApConnector& operator=(const ApConnector&) = delete;
    ApConnector() = default;

    // `player` is only used for logging. Returns false if the port is taken,
    // which on this range usually means a real BizHawk is running.
    bool open(int port, int player);
    void close();

    // From the machine's own thread, once a frame, after run_frame(). Executes
    // whatever the socket thread has parked, or returns immediately.
    void serve(GbaInstance& gba);

    bool listening() const { return fd_.load(std::memory_order_relaxed) >= 0; }
    bool client_connected() const { return client_.load(std::memory_order_relaxed) >= 0; }
    int port() const { return port_; }

    // Whether the guest should be held still. A client may ask for emulation
    // to stop while it makes several requests; see the note in the
    // implementation for why that is honoured only sometimes.
    bool locked() const;

    // The last thing a game client asked to be shown, for the status gutter.
    std::string message() const;

    // Set when a request could not be served for a reason that is this
    // program's gap rather than the game's fault — a memory region no world
    // has needed until now. Sticky, because the client retries and the first
    // occurrence is the informative one, and because the symptom otherwise is
    // a slot that simply never progresses.
    std::string fault() const;

private:
    void run();                                   // socket thread
    std::string handle(const std::string& line);  // one request line

    int port_ = 0;
    int player_ = 0;
    std::atomic<int> fd_{-1};           // listening socket, -1 when not listening
    std::atomic<int> client_{-1};       // accepted socket, -1 when none
    std::atomic<bool> quit_{false};
    std::thread thread_;

    // The hand-off. `pending` is set by the socket thread and cleared by the
    // machine; `done` goes the other way.
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    const json::Value* pending_ = nullptr;
    json::Value reply_;
    bool done_ = false;

    std::atomic<bool> lock_requested_{false};
    std::atomic<long long> lock_deadline_ms_{0};
    mutable std::mutex message_mutex_;
    std::string message_;
    std::string fault_;
};

}  // namespace gql

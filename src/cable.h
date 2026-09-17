/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// cable.h — the link cable between our own machines.
//
// Case three of four: four GBAs plugged into each other rather than into a
// GameCube. mGBA already has the hard part, a lockstep coordinator that keeps
// several cores in step with one another; what it does not have is a way to
// drive it from threads we own rather than from mGBA's own mCoreThread.
//
// The coordinator lives here, shared by every machine on the cable. Each
// machine's driver and its sleep/wake plumbing live in its GbaInstance,
// because they belong to the core.

#pragma once

namespace gql {

// At most one cable per machine, which is the most that can be needed: four
// people can form at most four groups, and that is when nobody is playing with
// anybody.
constexpr int kMaxCables = 4;

class CableGroup {
public:
    CableGroup();
    ~CableGroup();
    CableGroup(const CableGroup&) = delete;
    CableGroup& operator=(const CableGroup&) = delete;

    // How many machines are currently on this cable.
    int attached() const;

    // Opaque to everyone but GbaInstance, which needs the real type.
    void* raw();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace gql

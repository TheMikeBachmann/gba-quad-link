/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "cable.h"

#include <mgba/internal/gba/sio/lockstep.h>

namespace gql {

struct CableGroup::Impl {
    struct GBASIOLockstepCoordinator coordinator;
};

CableGroup::CableGroup() : impl_(new Impl()) {
    GBASIOLockstepCoordinatorInit(&impl_->coordinator);
}

CableGroup::~CableGroup() {
    if (!impl_) return;
    GBASIOLockstepCoordinatorDeinit(&impl_->coordinator);
    delete impl_;
    impl_ = nullptr;
}

int CableGroup::attached() const {
    if (!impl_) return 0;
    return static_cast<int>(GBASIOLockstepCoordinatorAttached(
        const_cast<struct GBASIOLockstepCoordinator*>(&impl_->coordinator)));
}

void* CableGroup::raw() { return impl_ ? &impl_->coordinator : nullptr; }

}  // namespace gql

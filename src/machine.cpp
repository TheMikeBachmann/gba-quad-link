/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "machine.h"

namespace gql {

const char* Machine::stage_name(Machine::ApStage s) {
    switch (s) {
        case Machine::ApStage::Idle:        return "";
        case Machine::ApStage::Searching:   return "finding cartridge";
        case Machine::ApStage::Configuring: return "preparing";
        case Machine::ApStage::Starting:    return "starting client";
        case Machine::ApStage::Ready:       return "archipelago";
        case Machine::ApStage::Failed:      return "archipelago failed";
    }
    return "";
}

const char* link_state_name(LinkState s) {
    switch (s) {
        case LinkState::Off:      return "no link";
        case LinkState::Dialling: return "dialling";
        case LinkState::Waiting:  return "waiting";
        case LinkState::Linked:   return "linked";
        case LinkState::Lost:     return "lost";
        case LinkState::Cable:    return "cable";
    }
    return "?";
}

}  // namespace gql

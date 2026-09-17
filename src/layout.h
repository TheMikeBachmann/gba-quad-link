/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// layout.h — where the four screens and the two status columns go.
//
// Four 240x160 screens tile to 480x320, which is 3:2. A television is 16:9,
// so something has to give, and what gives is about eleven percent of the
// width at each side. The previous project did not have this problem because
// its recompiler rendered 284 columns of real scene per machine; libmgba
// renders what a Game Boy Advance renders, and there is no honest way to widen
// it.
//
// Rather than black the margins out, they carry a status column per side —
// which quadrant is which, whether Dolphin is talking to it, which controller
// drives it. For an application whose failure modes are mostly "one of the
// four is not connected", that is a better use of the space than nothing.

#pragma once

namespace gql {

struct Rect { int x, y, w, h; };

struct Layout {
    Rect quadrant[4];     // player order: top-left, top-right, bottom-left, bottom-right
    Rect gutter[2];       // left, right; w == 0 when there is no room for them
    bool gutters = false;
};

// `integer_scale` keeps the guest image unresampled at the cost of a border.
//
// Below this many pixels a status column cannot hold a legible line of text,
// so it is not drawn and the screens take the space instead. Roughly the width
// of "P1 linked" at the font size used.
constexpr int kMinGutterWidth = 96;

Layout compute_layout(int win_w, int win_h, bool integer_scale);

}  // namespace gql

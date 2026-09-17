/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// layout.h — where the screens and the status columns go.
//
// How many screens there are is not fixed. Four people start an evening and
// three of them go to bed, and the one still playing should not be left in a
// quarter of a television because of who they started with. So the arrangement
// follows the number of machines actually in use:
//
//   one    the whole window
//   two    side by side, or stacked, whichever gives each person more pixels
//   three  the four-way grid with a corner empty
//   four   the four-way grid
//
// Two is the only interesting case. Two screens side by side make a canvas
// three times wider than it is tall, and stacked they make one that is taller
// than it is wide; which of those wastes less depends entirely on the shape of
// the display. On every widescreen it is side by side, and on 4:3 it is
// stacked, so rather than pick one and be wrong somewhere, both are measured.
//
// Four 240x160 screens tile to 3:2, so a 16:9 display has about eleven percent
// spare at each side. That carries a status column per side — player, link
// state, frame rate, pad — because what goes wrong here is mostly one of the
// four quietly not being connected, and that is otherwise invisible. The
// smaller arrangements do not have that margin to spend, and a single player
// asked for the whole window, so the columns belong to the grid alone.

#pragma once

namespace gql {

struct Rect { int x, y, w, h; };

struct Layout {
    Rect screen[4];       // in player order; only `count` are filled
    int count = 0;
    Rect gutter[2];       // left, right
    bool gutters = false;
};

// Below this a status column cannot hold a legible line of text, so it is not
// drawn and the screens take the space instead.
constexpr int kMinGutterWidth = 96;

// `count` is how many machines are in use, clamped to 1..4.
Layout compute_layout(int win_w, int win_h, int count, bool integer_scale);

}  // namespace gql

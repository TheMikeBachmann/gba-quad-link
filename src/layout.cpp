/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "layout.h"

#include <algorithm>

#include "gba_instance.h"

namespace gql {
namespace {

constexpr int kW = GbaInstance::kWidth;
constexpr int kH = GbaInstance::kHeight;

// Lay `count` screens out on a cols x rows grid and return the area each one
// gets, or zero if the grid cannot hold them.
long grid_area(int win_w, int win_h, int cols, int rows) {
    if (win_w <= 0 || win_h <= 0) return 0;
    const double fit = std::min(static_cast<double>(win_w) / (kW * cols),
                                static_cast<double>(win_h) / (kH * rows));
    const long w = static_cast<long>(kW * fit);
    const long h = static_cast<long>(kH * fit);
    return w * h;
}

void tile(Layout* out, int win_w, int win_h, int cols, int rows, int count,
          bool integer_scale) {
    const int canvas_w = kW * cols;
    const int canvas_h = kH * rows;

    int draw_w, draw_h;
    if (integer_scale) {
        const int units = std::max(1, std::min(win_w / canvas_w,
                                               win_h / canvas_h));
        draw_w = canvas_w * units;
        draw_h = canvas_h * units;
    } else {
        const double fit = std::min(static_cast<double>(win_w) / canvas_w,
                                    static_cast<double>(win_h) / canvas_h);
        draw_w = static_cast<int>(canvas_w * fit);
        draw_h = static_cast<int>(canvas_h * fit);
    }

    const int ox = (win_w - draw_w) / 2;
    const int oy = (win_h - draw_h) / 2;

    // Shared edges rather than a width each: at a fractional scale a column is
    // an odd number of pixels, and independently rounded rectangles leave a
    // seam down the middle of the screen.
    int ex[3], ey[3];
    for (int i = 0; i <= cols; ++i) ex[i] = ox + draw_w * i / cols;
    for (int i = 0; i <= rows; ++i) ey[i] = oy + draw_h * i / rows;

    out->count = count;
    for (int i = 0; i < count; ++i) {
        const int c = i % cols, r = i / cols;
        out->screen[i] = Rect{ex[c], ey[r], ex[c + 1] - ex[c],
                              ey[r + 1] - ey[r]};
    }

    // Only the four-way grid has margin to spare; the others either asked for
    // the whole window or are constrained the other way round.
    if (cols == 2 && rows == 2 && ox >= kMinGutterWidth) {
        out->gutters = true;
        out->gutter[0] = Rect{0, oy, ox, draw_h};
        out->gutter[1] = Rect{ox + draw_w, oy, win_w - (ox + draw_w), draw_h};
    }
}

}  // namespace

Layout compute_layout(int win_w, int win_h, int count, bool integer_scale) {
    Layout out{};
    if (win_w <= 0 || win_h <= 0) return out;
    count = std::clamp(count, 1, 4);

    int cols = 2, rows = 2;
    if (count == 1) {
        cols = rows = 1;
    } else if (count == 2) {
        // Measured, not assumed: side by side wins on every widescreen shape
        // and stacked wins on 4:3, so the display decides.
        cols = grid_area(win_w, win_h, 2, 1) >= grid_area(win_w, win_h, 1, 2)
                   ? 2 : 1;
        rows = 3 - cols;
    }
    tile(&out, win_w, win_h, cols, rows, count, integer_scale);
    return out;
}

}  // namespace gql

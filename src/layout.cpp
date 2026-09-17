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

constexpr int kCanvasW = GbaInstance::kWidth * 2;    // 480
constexpr int kCanvasH = GbaInstance::kHeight * 2;   // 320

}  // namespace

Layout compute_layout(int win_w, int win_h, bool integer_scale) {
    Layout out{};
    if (win_w <= 0 || win_h <= 0) return out;

    int draw_w, draw_h;
    if (integer_scale) {
        const int units = std::max(1, std::min(win_w / kCanvasW,
                                               win_h / kCanvasH));
        draw_w = kCanvasW * units;
        draw_h = kCanvasH * units;
    } else {
        const double fit = std::min(static_cast<double>(win_w) / kCanvasW,
                                    static_cast<double>(win_h) / kCanvasH);
        draw_w = static_cast<int>(kCanvasW * fit);
        draw_h = static_cast<int>(kCanvasH * fit);
    }

    const int ox = (win_w - draw_w) / 2;
    const int oy = (win_h - draw_h) / 2;

    // Quadrant edges rather than a per-quadrant width: at a fractional scale
    // half the canvas is an odd number of pixels, and four independently
    // rounded rectangles leave a one-pixel seam down the middle of the screen.
    const int qx[3] = {ox, ox + draw_w / 2, ox + draw_w};
    const int qy[3] = {oy, oy + draw_h / 2, oy + draw_h};
    for (int i = 0; i < 4; ++i) {
        out.quadrant[i] = Rect{qx[i % 2], qy[i / 2],
                               qx[i % 2 + 1] - qx[i % 2],
                               qy[i / 2 + 1] - qy[i / 2]};
    }

    const int margin = ox;
    if (margin >= kMinGutterWidth) {
        out.gutters = true;
        out.gutter[0] = Rect{0, oy, margin, draw_h};
        out.gutter[1] = Rect{ox + draw_w, oy, win_w - (ox + draw_w), draw_h};
    }
    return out;
}

}  // namespace gql

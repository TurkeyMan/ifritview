#pragma once

// The rectangle a selection drag describes, in whole image pixels.

#include <algorithm>
#include <cmath>

enum class SelectionLock
{
    None,
    Square,      // Shift
    ImageRatio,  // Ctrl: the image's own width:height
};

struct SelectionRect
{
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// `ax, ay` is where the drag started and `px, py` where the pointer is now,
// both in image pixels and already inside [0, w] x [0, h].
inline SelectionRect selection_rect(double ax, double ay, double px, double py, int w, int h, SelectionLock lock)
{
    SelectionRect r;
    if (lock == SelectionLock::None || w < 1 || h < 1)
    {
        // Every pixel the drag touches.
        r.x0 = std::floor(std::min(ax, px));
        r.y0 = std::floor(std::min(ay, py));
        r.x1 = std::ceil(std::max(ax, px));
        r.y1 = std::ceil(std::max(ay, py));
        return r;
    }

    // A locked rectangle grows from a pixel corner so its size is whole
    // pixels, and towards whichever quadrant the pointer is in.
    const double cx = std::round(ax), cy = std::round(ay);
    const bool right = px >= cx, down = py >= cy;
    const double ratio = lock == SelectionLock::Square ? 1.0 : double(w) / h;
    // Room before the image edge in the drag's direction: the rectangle
    // shrinks, keeping its shape, rather than run off the image.
    const double room_x = right ? w - cx : cx, room_y = down ? h - cy : cy;

    // Big enough to reach the pointer along both axes, as other editors do.
    double sw = std::max(std::fabs(px - cx), std::fabs(py - cy) * ratio);
    sw = std::min({sw, room_x, room_y * ratio});
    const double sh = sw / ratio;

    // Whole pixels can't always hold the ratio exactly: of the sizes within a
    // pixel of the ideal one that still fit, take the one whose shape is
    // closest, then the one closest in size.
    double best_w = 0, best_h = 0, best_err = 0, best_dist = 0;
    auto consider = [&](double cw, double ch)
    {
        if (cw < 1 || ch < 1 || cw > room_x || ch > room_y)
            return;
        const double err = std::fabs(cw / ch / ratio - 1.0);
        const double dist = std::fabs(cw - sw) + std::fabs(ch - sh);
        if (best_w == 0 || err < best_err - 1e-12 || (err < best_err + 1e-12 && dist < best_dist))
        {
            best_w = cw;
            best_h = ch;
            best_err = err;
            best_dist = dist;
        }
    };
    for (const double cw : {std::floor(sw), std::ceil(sw)})
        consider(cw, std::round(cw / ratio));
    for (const double ch : {std::floor(sh), std::ceil(sh)})
        consider(std::round(ch * ratio), ch);

    r.x0 = right ? cx : cx - best_w;
    r.x1 = right ? cx + best_w : cx;
    r.y0 = down ? cy : cy - best_h;
    r.y1 = down ? cy + best_h : cy;
    return r;
}

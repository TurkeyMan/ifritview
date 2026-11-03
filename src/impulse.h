#pragma once

// Hot and cold photosites taken out of a raw's mosaic before the demosaic
// spreads them into coloured specks and steers its edges with them: a
// photosite far above (or below) all eight of its colour around it, by more
// than they differ among themselves and by a floor, is a defect or a spike
// of noise (no detail the lens drew is one photosite out of step with its
// colour's neighbours), and becomes their median. On a colour sensor it must
// also be the only such peak where it is: a glint, a point of light lighting
// a photosite of each colour, makes its nearest of the other colours peaks
// of theirs too, and is kept. Bayer's neighbours of a colour are two away; a
// monochrome sensor's, next to it.
// The GPU's develop does the same (develop.hlsl's IMPULSE); this is the
// CPU's, for LibRaw's.

#include "parallel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

constexpr float kImpulseSpread = 1;      // beyond the neighbours by more than their own spread
constexpr float kImpulseFloor = 0.005f;  // and by at least this much of black to white
constexpr int kImpulseShared = 2;        // kept if this many of its nearest are peaks of their colour too

// `data`: rows of `pitch` values, `width` x `height`, a Bayer mosaic or (with
// `mono`) a monochrome sensor's; `range`: white less black, data units.
// Neighbours of a colour share its black, so it needn't come off. Returns how
// many were replaced.
inline size_t remove_impulses(uint16_t* data, int width, int height, size_t pitch, float range, bool mono)
{
    const int step = mono ? 1 : 2;
    if (width < 2 * step + 1 || height < 2 * step + 1)
        return 0;
    const std::vector<uint16_t> in(data, data + pitch * size_t(height));
    const float floor = kImpulseFloor * range;
    // Past an edge, mirrored: the same colour, as the parity is kept.
    const auto at = [&](int x, int y)
    {
        x = x < 0 ? -x : x >= width ? 2 * (width - 1) - x : x;
        y = y < 0 ? -y : y >= height ? 2 * (height - 1) - y : y;
        return float(in[size_t(y) * pitch + size_t(x)]);
    };
    std::vector<size_t> counts(size_t(height), 0);
    parallel(size_t(height), [&](size_t row)
    {
        const int y = int(row), s = step;
        for (int x = 0; x < width; ++x)
        {
            const float v = float(in[row * pitch + size_t(x)]);
            float n[8] = {at(x - s, y - s), at(x, y - s), at(x + s, y - s), at(x - s, y),
                          at(x + s, y),     at(x - s, y + s), at(x, y + s), at(x + s, y + s)};
            const auto [lo, hi] = std::minmax_element(n, n + 8);
            const float t = std::max(kImpulseSpread * (*hi - *lo), floor);
            const bool hot = v - *hi > t, cold = *lo - v > t;
            if (!hot && !cold)
                continue;
            if (!mono)
            {
                // Its nearest of the other colours: peaks of theirs too, a glint.
                const auto beyond = [&](int px, int py)
                {
                    const float c = at(px, py);
                    float m[8] = {at(px - 2, py - 2), at(px, py - 2), at(px + 2, py - 2), at(px - 2, py),
                                  at(px + 2, py),     at(px - 2, py + 2), at(px, py + 2), at(px + 2, py + 2)};
                    const auto [mlo, mhi] = std::minmax_element(m, m + 8);
                    return hot ? c - *mhi : *mlo - c;
                };
                int peaks = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i)
                        if ((i || j) && beyond(x + i, y + j) > 0)
                            ++peaks;
                if (peaks >= kImpulseShared)
                    continue;
            }
            std::sort(n, n + 8);
            data[row * pitch + size_t(x)] = uint16_t((n[3] + n[4]) / 2 + 0.5f);
            ++counts[row];
        }
    });
    size_t total = 0;
    for (size_t c : counts)
        total += c;
    return total;
}

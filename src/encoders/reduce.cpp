#include "reduce.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace
{

// The nearest of `bits` levels to v, expanded back to 0..255.
inline int quantise(int v, int bits)
{
    if (bits >= 8)
        return v;
    const int levels = (1 << bits) - 1;
    const int q = (std::clamp(v, 0, 255) * levels + 127) / 255;
    return (q * 255 + levels / 2) / levels;
}

// Floyd–Steinberg over `channels` interleaved channels (stride 4), each
// mapped by `map(c, value) -> value`, spreading the error right and down.
template <class Map>
void diffuse(std::vector<uint8_t>& px, int w, int h, int channels, bool dither, Map map)
{
    std::vector<int> err_cur(size_t(w + 2) * 4, 0), err_next(size_t(w + 2) * 4, 0);
    for (int y = 0; y < h; ++y)
    {
        std::fill(err_next.begin(), err_next.end(), 0);
        uint8_t* row = px.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < channels; ++c)
            {
                // Colour only: dithered alpha would stipple cut-out edges.
                const bool spread = dither && c < 3;
                const int want = row[x * 4 + c] + (spread ? err_cur[size_t(x + 1) * 4 + c] / 16 : 0);
                const int got = map(c, std::clamp(want, 0, 255));
                row[x * 4 + c] = uint8_t(got);
                if (!spread)
                    continue;
                const int e = want - got;
                err_cur[size_t(x + 2) * 4 + c] += e * 7;
                err_next[size_t(x) * 4 + c] += e * 3;
                err_next[size_t(x + 1) * 4 + c] += e * 5;
                err_next[size_t(x + 2) * 4 + c] += e;
            }
        std::swap(err_cur, err_next);
    }
}

struct Box
{
    std::vector<uint32_t> colours;  // packed 0x00RRGGBB, with repeats
    int range = 0, channel = 0;     // its widest channel
};

void measure(Box& b)
{
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (uint32_t c : b.colours)
        for (int k = 0; k < 3; ++k)
        {
            const int v = int(c >> (16 - 8 * k)) & 255;
            lo[k] = std::min(lo[k], v);
            hi[k] = std::max(hi[k], v);
        }
    b.range = -1;
    for (int k = 0; k < 3; ++k)
        if (hi[k] - lo[k] > b.range)
            b.range = hi[k] - lo[k], b.channel = k;
}

}  // namespace

void reduce_channels(std::vector<uint8_t>& rgba, int width, int height, int rbits, int gbits, int bbits, int abits, bool dither)
{
    const int bits[4] = {rbits, gbits, bbits, abits};
    diffuse(rgba, width, height, 4, dither, [&](int c, int v) { return c == 3 && bits[3] == 0 ? 255 : quantise(v, bits[c]); });
}

Palettised palettise(const std::vector<uint8_t>& rgba, int width, int height, bool dither)
{
    // Median cut on a sample of the pixels (every one up to ~1M).
    const size_t count = size_t(width) * height;
    const size_t step = std::max<size_t>(1, count / (1 << 20));
    std::vector<Box> boxes(1);
    for (size_t i = 0; i < count; i += step)
        boxes[0].colours.push_back(uint32_t(rgba[i * 4]) << 16 | uint32_t(rgba[i * 4 + 1]) << 8 | rgba[i * 4 + 2]);
    measure(boxes[0]);
    while (boxes.size() < 256)
    {
        auto widest = std::max_element(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.range < b.range; });
        if (widest->range <= 0 || widest->colours.size() < 2)
            break;
        const int shift = 16 - 8 * widest->channel;
        auto& cs = widest->colours;
        auto mid = cs.begin() + std::ptrdiff_t(cs.size() / 2);
        std::nth_element(cs.begin(), mid, cs.end(), [shift](uint32_t a, uint32_t b) { return ((a >> shift) & 255) < ((b >> shift) & 255); });
        Box upper;
        upper.colours.assign(mid, cs.end());
        cs.erase(mid, cs.end());
        measure(*widest);
        measure(upper);
        boxes.push_back(std::move(upper));
    }
    Palettised out;
    for (const Box& b : boxes)
    {
        long sum[3] = {0, 0, 0};
        for (uint32_t c : b.colours)
            for (int k = 0; k < 3; ++k)
                sum[k] += long(c >> (16 - 8 * k)) & 255;
        for (int k = 0; k < 3; ++k)
            out.palette.push_back(uint8_t(b.colours.empty() ? 0 : sum[k] / long(b.colours.size())));
    }
    const int n = int(out.palette.size() / 3);

    // Nearest palette entry, cached on 5:6:5 so a big image stays quick.
    std::vector<int16_t> cache(1 << 16, -1);
    const auto nearest = [&](int r, int g, int b)
    {
        const int key = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
        if (cache[size_t(key)] >= 0)
            return int(cache[size_t(key)]);
        int best = 0, best_d = 1 << 30;
        for (int i = 0; i < n; ++i)
        {
            const int dr = r - out.palette[size_t(i) * 3], dg = g - out.palette[size_t(i) * 3 + 1], db = b - out.palette[size_t(i) * 3 + 2];
            const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (d < best_d)
                best_d = d, best = i;
        }
        cache[size_t(key)] = int16_t(best);
        return best;
    };

    // Map every pixel (diffusing the error), then read the indices back.
    std::vector<uint8_t> px = rgba;
    out.indices.resize(count);
    std::vector<int> err_cur(size_t(width + 2) * 3, 0), err_next(size_t(width + 2) * 3, 0);
    for (int y = 0; y < height; ++y)
    {
        std::fill(err_next.begin(), err_next.end(), 0);
        for (int x = 0; x < width; ++x)
        {
            const size_t i = size_t(y) * width + x;
            int want[3];
            for (int k = 0; k < 3; ++k)
                want[k] = std::clamp(int(px[i * 4 + k]) + (dither ? err_cur[size_t(x + 1) * 3 + k] / 16 : 0), 0, 255);
            const int idx = nearest(want[0], want[1], want[2]);
            out.indices[i] = uint8_t(idx);
            if (!dither)
                continue;
            for (int k = 0; k < 3; ++k)
            {
                const int e = want[k] - out.palette[size_t(idx) * 3 + k];
                err_cur[size_t(x + 2) * 3 + k] += e * 7;
                err_next[size_t(x) * 3 + k] += e * 3;
                err_next[size_t(x + 1) * 3 + k] += e * 5;
                err_next[size_t(x + 2) * 3 + k] += e;
            }
        }
        std::swap(err_cur, err_next);
    }
    return out;
}

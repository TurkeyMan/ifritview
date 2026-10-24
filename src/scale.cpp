#include "scale.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

constexpr int kWeightBits = 14;  // weights sum to 1 << kWeightBits

// For one axis: which source pixels each destination pixel covers, and how
// much (fixed point, summing exactly to 1 << kWeightBits).
struct Span
{
    int first, count;
    int weights[8];  // reductions here are at most ~2x after mip selection, so few taps
};

// GCC 16.2's IPA-CP, under LTO, compiles this as if every call asked for
// one output pixel (dst == 1) while the callers pass the real size: the
// first zoomed-out paint then reads past the spans. Kept out of
// interprocedural optimisation, for whoever builds with LTO.
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noipa]]
#endif
std::vector<Span> spans(int src, int dst)
{
    std::vector<Span> out(dst);
    const double ratio = double(src) / dst;
    for (int i = 0; i < dst; ++i)
    {
        const double a = i * ratio, b = std::min(double(src), (i + 1) * ratio);
        const int first = int(std::floor(a)), last = std::min(src - 1, int(std::ceil(b)) - 1);
        Span& s = out[i];
        s.first = first;
        s.count = std::min(8, last - first + 1);
        double cover[8];
        double total = 0;
        for (int k = 0; k < s.count; ++k)
        {
            const double lo = std::max(a, double(first + k)), hi = std::min(b, double(first + k + 1));
            cover[k] = std::max(0.0, hi - lo);
            total += cover[k];
        }
        int sum = 0;
        for (int k = 0; k < s.count; ++k)
        {
            s.weights[k] = int(std::lround(cover[k] / total * (1 << kWeightBits)));
            sum += s.weights[k];
        }
        s.weights[0] += (1 << kWeightBits) - sum;  // rounding goes to the first tap
    }
    return out;
}

// One source row filtered horizontally: 4 channels per output column,
// scaled by 1 << kWeightBits.
void filter_row(const uint8_t* line, const std::vector<Span>& xs, uint32_t* out)
{
    for (const Span& sx : xs)
    {
        uint32_t b = 0, g = 0, r = 0, a = 0;
        const uint8_t* p = line + size_t(sx.first) * 4;
        for (int j = 0; j < sx.count; ++j, p += 4)
        {
            const uint32_t w = uint32_t(sx.weights[j]);
            b += p[0] * w;
            g += p[1] * w;
            r += p[2] * w;
            a += p[3] * w;
        }
        out[0] = b;
        out[1] = g;
        out[2] = r;
        out[3] = a;
        out += 4;
    }
}

}  // namespace

PixelBuffer downscale(const Image& image, int w, int h)
{
    w = std::max(1, w);
    h = std::max(1, h);
    // The smallest level that's still at least the target size.
    const uint8_t* src = image.pixels.data();
    int sw = image.width, sh = image.height;
    size_t stride = image.stride;
    for (const Image::Mip& m : image.mips)
    {
        if (m.width < w || m.height < h)
            break;
        src = m.pixels.data();
        sw = m.width;
        sh = m.height;
        stride = size_t(m.width) * 4;
    }
    const std::vector<Span> xs = spans(sw, w), ys = spans(sh, h);
    PixelBuffer out(size_t(w) * h * 4);

    // Bands of output rows on separate threads. Each band filters a source
    // row horizontally once and keeps the few it needs for the next output row.
    auto band = [&](int y0, int y1)
    {
        struct Cached
        {
            int row = -1;
            std::vector<uint32_t> px;
        };
        Cached cache[8];
        for (Cached& c : cache)
            c.px.resize(size_t(w) * 4);
        auto row = [&](int r) -> const uint32_t*
        {
            Cached& c = cache[r & 7];
            if (c.row != r)
            {
                filter_row(src + size_t(r) * stride, xs, c.px.data());
                c.row = r;
            }
            return c.px.data();
        };
        std::vector<uint64_t> acc(size_t(w) * 4);
        for (int y = y0; y < y1; ++y)
        {
            const Span& sy = ys[y];
            std::fill(acc.begin(), acc.end(), 0);
            for (int k = 0; k < sy.count; ++k)
            {
                const uint32_t* line = row(sy.first + k);
                const uint64_t wy = uint64_t(sy.weights[k]);
                for (size_t i = 0; i < acc.size(); ++i)
                    acc[i] += line[i] * wy;
            }
            uint8_t* dst = out.data() + size_t(y) * w * 4;
            constexpr int shift = 2 * kWeightBits;
            constexpr uint64_t half = uint64_t(1) << (shift - 1);
            for (size_t i = 0; i < acc.size(); ++i)
                dst[i] = uint8_t(std::min<uint64_t>(255, (acc[i] + half) >> shift));
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
    const int per = (h + threads - 1) / threads;
    if (threads == 1 || h < 64)
    {
        band(0, h);
        return out;
    }
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
    {
        const int y0 = t * per, y1 = std::min(h, y0 + per);
        if (y0 < y1)
            pool.emplace_back(band, y0, y1);
    }
    for (std::thread& t : pool)
        t.join();
    return out;
}

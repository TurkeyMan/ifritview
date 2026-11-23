#include "detail.h"

#include "parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

Detail Detail::raw_defaults()
{
    Detail d;
    d.sharpness = 40;
    d.colour = 25;
    return d;
}

namespace
{

// The B3 spline's detail at each scale, for white noise of 1: what a
// threshold at the finest scale is scaled by at the others.
constexpr float kNoiseAt[DetailFilter::kLevels] = {0.890f, 0.201f, 0.0855f, 0.0413f, 0.0205f};
constexpr float kB3[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};
constexpr int kNoiseStride = 4;  // the lattice the noise is measured on

// The noise model's fit (shaders/noise.hlsl's, step for step): blocks of
// kFitBlock x kFitBlock pixels, one every kFitLattice across and down, by
// their mean (kFitLevels steps of sqrt(mean / kFitTop)) and the log2 of
// their variance (kFitSpreads steps of 1/4 from 2^-32).
constexpr int kFitBlock = 8, kFitLattice = 16;
constexpr int kFitLevels = 64, kFitSpreads = 128;
constexpr float kFitTop = 0.25f;
constexpr float kFitQuartile = 0.84f;  // the lower quartile of plain noise's blocks, of their mean
constexpr unsigned kFitLeast = 16;     // blocks for a level to count
constexpr float kFitMost = 2000;       // blocks a level is weighed by, at most
constexpr int kForwardSize = 512;  // the averages each colour's inverse table is made from (shaders/detail.hlsli's)
constexpr float kMixtureMost = 64;  // shaders/detail.hlsli's

// Each level's variance, the lower quartile of its blocks' (texture only
// adds to a block's; scaled to their mean as plain noise has it), then
// variance = a x + b by weighted least squares, weighed by how sure each
// level is (its blocks, relative to its variance), four times over, each
// time leaving out the levels more than half as far again from the line as
// noise would be (texture, clipping). `counts` one colour's, kFitLevels x
// kFitSpreads. (The GPU's fit_group() can hold b at the masked border's;
// the CPU's data, LibRaw's develop, brings none.)
bool fit_line(const unsigned* counts, float& a, float& b)
{
    a = 0, b = 0;
    float v[kFitLevels], n[kFitLevels], w[kFitLevels];
    float top = 0;
    for (int k = 0; k < kFitLevels; ++k)
    {
        const unsigned* bins = counts + size_t(k) * kFitSpreads;
        unsigned total = 0;
        for (int i = 0; i < kFitSpreads; ++i)
            total += bins[i];
        n[k] = 0, v[k] = 0;
        if (total < kFitLeast)
            continue;
        const float want = total / 4.0f;
        unsigned seen = 0;
        for (int i = 0; i < kFitSpreads; ++i)
        {
            if (seen + bins[i] >= want)
            {
                const float at = i + (want - seen) / bins[i];  // within the bin, linearly
                v[k] = std::exp2(at / 4 - 32) / kFitQuartile;
                break;
            }
            seen += bins[i];
        }
        n[k] = std::min(float(total), kFitMost);
        top = std::max(top, v[k]);
    }
    if (top <= 0)
        return false;
    // Levels as 0..1 (x), variances over the largest (y): well within a float's range.
    for (int k = 0; k < kFitLevels; ++k)
    {
        v[k] /= top;
        w[k] = n[k] > 0 ? n[k] / (v[k] * v[k]) : 0;
    }
    float fa = 0, fb = 0, lo = 1e30f, hi = 0;
    int used = 0;
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        float S = 0, Sx = 0, Sy = 0, Sxx = 0, Sxy = 0;
        for (int k = 0; k < kFitLevels; ++k)
        {
            const float u = (k + 0.5f) / kFitLevels, x = u * u;
            S += w[k], Sx += w[k] * x, Sy += w[k] * v[k], Sxx += w[k] * x * x, Sxy += w[k] * x * v[k];
        }
        if (S <= 0 || Sxx <= 0)
            return false;
        const float det = S * Sxx - Sx * Sx;
        fa = det > 0 ? (S * Sxy - Sx * Sy) / det : 0;
        fb = det > 0 ? (Sxx * Sy - Sx * Sxy) / det : -1;
        if (fb < 0)
            fb = 0, fa = Sxy / Sxx;
        if (fa <= 0)
            return false;
        used = 0, lo = 1e30f, hi = 0;
        for (int k = 0; k < kFitLevels; ++k)
        {
            const float u = (k + 0.5f) / kFitLevels, x = u * u, m = fa * x + fb;
            const bool close = n[k] > 0 && v[k] <= 1.5f * m && v[k] * 1.5f >= m;
            w[k] = close ? n[k] / (m * m) : 0;
            if (close)
                ++used, lo = std::min(lo, x), hi = std::max(hi, x);
        }
    }
    // Enough levels, far enough apart, for a line through them.
    if (used < 4 || hi < 4 * lo)
        return false;
    a = fa * top / kFitTop, b = fb * top;
    return true;
}

}  // namespace

NoiseModel fit_noise(const float* rgb, int width, int height)
{
    NoiseModel m;
    const int across = width >= kFitBlock ? (width - kFitBlock) / kFitLattice + 1 : 0;
    const int down = height >= kFitBlock ? (height - kFitBlock) / kFitLattice + 1 : 0;
    if (across <= 0 || down <= 0)
        return m;
    // Each row of blocks counted on its own, then added up.
    std::vector<std::vector<unsigned>> rows(static_cast<size_t>(down));
    parallel(size_t(down), [&](size_t by)
    {
        std::vector<unsigned>& counts = rows[by];
        counts.assign(size_t(3) * kFitLevels * kFitSpreads, 0);
        const auto at = [&](int x, int y, int c)
        {
            x = std::clamp(x, 0, width - 1), y = std::clamp(y, 0, height - 1);
            return rgb[(size_t(y) * width + x) * 3 + c];
        };
        for (int bx = 0; bx < across; ++bx)
            for (int c = 0; c < 3; ++c)
            {
                float sum = 0, sq = 0;
                for (int y = int(by) * kFitLattice; y < int(by) * kFitLattice + kFitBlock; ++y)
                    for (int x = bx * kFitLattice; x < bx * kFitLattice + kFitBlock; ++x)
                    {
                        const float v = at(x, y, c);
                        const float d = v - (at(x + 1, y, c) + at(x - 1, y, c) + at(x, y + 1, c) + at(x, y - 1, c)) / 4;
                        sum += v, sq += d * d;
                    }
                const float n = kFitBlock * kFitBlock;
                const float mean = sum / n, variance = sq / n / 1.25f;  // 1.25: a pixel less four neighbours' mean, plain noise
                const float u = std::sqrt(std::max(mean, 0.0f) / kFitTop);
                if (u >= 1)
                    continue;
                const int level = std::min(int(u * kFitLevels), kFitLevels - 1);
                const int spread = int(std::clamp((std::log2(std::max(variance, 1e-12f)) + 32) * 4, 0.0f, kFitSpreads - 1.0f));
                ++counts[(size_t(c) * kFitLevels + level) * kFitSpreads + spread];
            }
    });
    std::vector<unsigned> counts(size_t(3) * kFitLevels * kFitSpreads, 0);
    for (const auto& r : rows)
        for (size_t i = 0; i < counts.size(); ++i)
            counts[i] += r[i];
    // Each colour fitted (to compare); the encoding by green's for all
    // three (shaders/noise.hlsl's).
    for (int c = 0; c < 3; ++c)
        if (fit_line(counts.data() + size_t(c) * kFitLevels * kFitSpreads, m.a[c], m.b[c]))
            m.fitted_colours |= 1 << c;
    m.fitted = (m.fitted_colours & 2) != 0;
    if (m.fitted)
        for (float& o : m.offset)
            o = std::max(3 * m.a[1] / 8 + m.b[1] / m.a[1], 1e-7f);
    return m;
}

DetailFilter::DetailFilter(const Detail& d)
{
    denoise = d.luminance > 0 || d.colour > 0;
    sharpen = d.sharpness > 0;
    const auto unit = [](double v) { return float(std::clamp(v, 0.0, 100.0) / 100); };
    // Luminance: up to 2.5 times the noise; Detail below 50 shrinks the two
    // finest scales harder, above less; Contrast spares the middle two; the
    // coarsest is left alone.
    const float l = 2.5f * unit(d.luminance), fine_l = 1.5f - unit(d.luminance_detail), middle = 1 - 0.8f * unit(d.luminance_contrast);
    const float weight_l[kLevels] = {fine_l, fine_l, middle, middle, 0};
    // Colour: up to 4 times, every scale; Detail spares the finest two,
    // Smoothness (at 50, as the middle) reaches into the coarsest two.
    const float c = 4 * unit(d.colour), fine_c = 1.5f - unit(d.colour_detail), coarse = 2 * unit(d.colour_smoothness);
    const float weight_c[kLevels] = {fine_c, fine_c, 1, coarse, coarse};
    for (int j = 0; j < kLevels; ++j)
    {
        luminance[j] = l * weight_l[j] * kNoiseAt[j] / kNoiseAt[0];
        colour[j] = c * weight_c[j] * kNoiseAt[j] / kNoiseAt[0];
    }
    static const bool unsupported = std::getenv("IFRIT_DETAIL_UNSUPPORTED");  // to compare
    for (int j = 0; j + 1 < kLevels && !unsupported; ++j)
        support[j] = 3 * kNoiseAt[j + 1] / kNoiseAt[0];
    static const bool unguided = std::getenv("IFRIT_DETAIL_UNGUIDED");  // to compare
    guide = denoise && !unguided ? 2.5f : 0;
    amount = float(std::clamp(d.sharpness, 0.0, 150.0) / 100 * 1.5);
    sigma = float(std::clamp(d.sharpen_radius, 0.5, 3.0));
    const float through = unit(d.sharpen_detail);
    halo = 0.002f + 0.1f * through * through;
    if (d.sharpen_masking > 0)
    {
        mask_high = 0.04f * unit(d.sharpen_masking);
        mask_low = mask_high / 2;
    }
}

namespace
{

// A plane of the encoding, a region of the image: luminance and the two
// colour differences, at x + y * width.
struct Planes
{
    int width = 0, height = 0;
    std::vector<float> c[3];
    void resize(int w, int h)
    {
        width = w, height = h;
        for (auto& p : c)
            p.assign(size_t(w) * h, 0);
    }
    float at(int k, int x, int y) const
    {
        return c[k][size_t(std::clamp(y, 0, height - 1)) * width + std::clamp(x, 0, width - 1)];
    }
};

// shaders/detail.hlsli's: the variance-stabilising transform of a colour
// (the noise model's offset), turned about 0 below it, and back.
float forward(float x, float offset)
{
    const float r = std::sqrt(offset);
    return x >= 0 ? std::sqrt(x + offset) : 2 * r - std::sqrt(offset - x);
}

float inverse(float e, float offset)
{
    const float r = std::sqrt(offset);
    return e >= r ? e * e - offset : offset - (2 * r - e) * (2 * r - e);
}

// shaders/detail.hlsli's encode: each channel stabilised, then luminance
// and the colour differences.
void encode(const NoiseModel& m, const float* rgb, float out[3])
{
    float e[3];
    for (int i = 0; i < 3; ++i)
        e[i] = forward(rgb[i], m.offset[i]);
    out[0] = (e[0] + 2 * e[1] + e[2]) / 4;
    out[1] = e[0] - e[1];
    out[2] = e[2] - e[1];
}

// The encoding's three stabilised colours.
void channels(const float in[3], float e[3])
{
    const float g = in[0] - (in[1] + in[2]) / 4;
    e[0] = in[1] + g, e[1] = g, e[2] = in[2] + g;
}

// shaders/detail.hlsli's detail_gauss(): E[f(m + s N(0, 1))].
float gauss(float m, float offset, float s)
{
    if (s <= 0)
        return forward(m, offset);
    float sum = 0, weights = 0;
    for (int k = -10; k <= 10; ++k)
    {
        const float z = k * 0.5f, w = std::exp(-0.5f * z * z);
        sum += w * forward(m + s * z, offset);
        weights += w;
    }
    return sum / weights;
}

}  // namespace

// shaders/detail.hlsli's detail_expected(): E[f(X) | y], X the Poisson-
// Gaussian mixture (a Poisson count of mean y / a, times a, plus Gaussian
// noise of variance b; below 0, the readout's noise around y).
float noise_expected(float y, float offset, float a, float b)
{
    const float s = std::sqrt(std::max(b, 0.0f));
    if (y <= 0 || a <= 0)
        return gauss(y, offset, s);
    const float lambda = y / a;
    if (lambda > kMixtureMost)
        return gauss(y, offset, std::sqrt(a * y + b));
    const int last = int(lambda + 10 * std::sqrt(lambda) + 10);
    float p = std::exp(-lambda), sum = 0;
    for (int k = 0; k <= last; ++k)
    {
        if (p > 1e-20f)
            sum += p * gauss(a * k, offset, s);
        p *= lambda / (k + 1);
    }
    return sum;
}

// shaders/noise.hlsl's: each colour's expectation inverse, by its own law
// (its own a and b, or where it has no fit of its own green's as the
// develop leaves that colour's against green's, `green_light`,
// `green_readout`; as a pixel has them: `light`, `readout`). Its averages at levels evenly spaced through the
// transform from 6 sigma of the readout's noise below 0 to kFitTop, then the
// level for each of kInverse averages evenly spaced between the first and
// the last, between the two it falls between.
void tabulate_noise(NoiseModel& m)
{
    const float offset = m.offset[1];
    for (int c = 0; c < 3; ++c)
    {
        const bool own = c == 1 || (m.fitted_colours & (1 << c));
        const float pa = (own ? m.a[c] : m.a[1] * m.green_light[c]) * m.light[c], pb = (own ? m.b[c] : m.b[1] * m.green_readout[c]) * m.readout[c];
        m.law_a[c] = pa, m.law_b[c] = pb;
        const float lo = forward(-6 * std::sqrt(pb), offset), hi = forward(kFitTop, offset);
        std::vector<float> averages(kForwardSize);
        parallel(size_t(kForwardSize), [&](size_t k)
        {
            averages[k] = noise_expected(inverse(lo + (hi - lo) * float(k) / (kForwardSize - 1), offset), offset, pa, pb);
        });
        m.from[c] = averages.front(), m.to[c] = averages.back();
        for (int i = 0; i < NoiseModel::kInverse; ++i)
        {
            const float want = m.from[c] + (m.to[c] - m.from[c]) * float(i) / (NoiseModel::kInverse - 1);
            int below = 0, above = kForwardSize - 1;
            while (above - below > 1)
            {
                const int mid = (below + above) / 2;
                (averages[size_t(mid)] <= want ? below : above) = mid;
            }
            const float t = std::clamp((want - averages[size_t(below)]) / std::max(averages[size_t(above)] - averages[size_t(below)], 1e-20f), 0.0f, 1.0f);
            m.inverse[c][i] = inverse(lo + (hi - lo) * (below + t) / (kForwardSize - 1), offset);
        }
    }
}

// shaders/detail.hlsl's expectation_inverse(): colour c's, at stabilised d.
float noise_expectation_inverse(const NoiseModel& m, float d, int c)
{
    const float offset = m.offset[c];
    const float t = (d - m.from[c]) / (m.to[c] - m.from[c]) * (NoiseModel::kInverse - 1);
    if (t <= 0)
        return inverse(d, offset) + m.inverse[c][0] - inverse(m.from[c], offset);
    if (t >= NoiseModel::kInverse - 1)
        return inverse(d, offset) + m.inverse[c][NoiseModel::kInverse - 1] - inverse(m.to[c], offset);
    const int i = int(t);
    return m.inverse[c][i] + (m.inverse[c][i + 1] - m.inverse[c][i]) * (t - i);
}

namespace
{

// shaders/detail.hlsli's kBandAutocorr (tools/band_table.py): the wavelet's
// scales as one-dimensional filters, A_ab(l) = sum_m P_a(m) P_b(m + l), for the
// pairs (j, j), j = 0..5, then (j, j + 1), j = 0..4; and kShrinkMoments
// (tools/shrink_table.py).
constexpr int kBandLags = 25;
constexpr int kSpatialBins = 256;  // shaders/detail.hlsli's: the blocks' variances, log2 in quarters from 2^-40
constexpr float kBandAutocorr[11][kBandLags] = {
    {1.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f},  // A_00
    {0.27343750f, 0.21875000f, 0.10937500f, 0.03125000f, 0.00390625f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f},  // A_11
    {0.12347412f, 0.11791992f, 0.10266113f, 0.08129883f, 0.05833435f, 0.03771973f, 0.02178955f, 0.01110840f, 0.00491333f, 0.00183105f, 0.00054932f, 0.00012207f, 0.00001526f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f},  // A_22
    {0.06035948f, 0.05969810f, 0.05775547f, 0.05465126f, 0.05057114f, 0.04574919f, 0.04044700f, 0.03493166f, 0.02945459f, 0.02423334f, 0.01943851f, 0.01518774f, 0.01154572f, 0.00852871f, 0.00611258f, 0.00424290f, 0.00284600f, 0.00183964f, 0.00114202f, 0.00067806f, 0.00038308f, 0.00020456f, 0.00010228f, 0.00004721f, 0.00001967f},  // A_33
    {0.03001472f, 0.02993302f, 0.02968920f, 0.02928706f, 0.02873280f, 0.02803489f, 0.02720383f, 0.02625192f, 0.02519295f, 0.02404189f, 0.02281459f, 0.02152738f, 0.02019679f, 0.01883920f, 0.01747048f, 0.01610575f, 0.01475909f, 0.01344330f, 0.01216977f, 0.01094829f, 0.00978705f, 0.00869259f, 0.00766977f, 0.00672192f, 0.00585083f},  // A_44
    {0.01498695f, 0.01497676f, 0.01494625f, 0.01489554f, 0.01482482f, 0.01473436f, 0.01462452f, 0.01449572f, 0.01434845f, 0.01418327f, 0.01400081f, 0.01380174f, 0.01358681f, 0.01335682f, 0.01311258f, 0.01285499f, 0.01258497f, 0.01230345f, 0.01201142f, 0.01170986f, 0.01139981f, 0.01108226f, 0.01075827f, 0.01042884f, 0.01009502f},  // A_55
    {0.37500000f, 0.25000000f, 0.06250000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f},  // A_01
    {0.15771484f, 0.14648438f, 0.11718750f, 0.08007812f, 0.04589844f, 0.02148438f, 0.00781250f, 0.00195312f, 0.00024414f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f},  // A_12
    {0.07608414f, 0.07478333f, 0.07100677f, 0.06510925f, 0.05761909f, 0.04916382f, 0.04039001f, 0.03189087f, 0.02414703f, 0.01748657f, 0.01206970f, 0.00790405f, 0.00487995f, 0.00281525f, 0.00149918f, 0.00072479f, 0.00031090f, 0.00011444f, 0.00003433f, 0.00000763f, 0.00000095f, 0.00000000f, 0.00000000f, 0.00000000f, 0.00000000f},  // A_23
    {0.03771785f, 0.03755820f, 0.03708309f, 0.03630388f, 0.03523894f, 0.03391296f, 0.03235593f, 0.03060216f, 0.02868898f, 0.02665561f, 0.02454188f, 0.02238709f, 0.02022890f, 0.01810241f, 0.01603925f, 0.01406693f, 0.01220828f, 0.01048112f, 0.00889814f, 0.00746703f, 0.00619080f, 0.00506821f, 0.00409444f, 0.00326166f, 0.00255979f},  // A_34
    {0.01881904f, 0.01879918f, 0.01873970f, 0.01864096f, 0.01850356f, 0.01832831f, 0.01811625f, 0.01786862f, 0.01758686f, 0.01727260f, 0.01692762f, 0.01655385f, 0.01615338f, 0.01572839f, 0.01528116f, 0.01481405f, 0.01432947f, 0.01382988f, 0.01331776f, 0.01279558f, 0.01226580f, 0.01173085f, 0.01119311f, 0.01065488f, 0.01011840f},  // A_45
};
constexpr float kShrinkMoments[297][4] = {
    {1.000000f, 0.000000f, 1.000000f, 1.000000f},
    {1.000000f, 0.125000f, 1.000000f, 1.000000f},
    {1.000000f, 0.250000f, 1.000000f, 1.000000f},
    {1.000000f, 0.375000f, 1.000000f, 1.000000f},
    {1.000000f, 0.500000f, 1.000000f, 1.000000f},
    {1.000000f, 0.625000f, 1.000000f, 1.000000f},
    {1.000000f, 0.750000f, 1.000000f, 1.000000f},
    {1.000000f, 0.875000f, 1.000000f, 1.000000f},
    {1.000000f, 1.000000f, 1.000000f, 1.000000f},
    {0.659415f, 0.000000f, 0.792926f, 0.511315f},
    {0.673945f, 0.064002f, 0.813927f, 0.695517f},
    {0.683931f, 0.144786f, 0.821663f, 0.750003f},
    {0.691807f, 0.232639f, 0.826374f, 0.779500f},
    {0.698394f, 0.324821f, 0.830434f, 0.798847f},
    {0.704091f, 0.420044f, 0.835053f, 0.814607f},
    {0.709127f, 0.517567f, 0.838213f, 0.826341f},
    {0.713650f, 0.616915f, 0.840563f, 0.835619f},
    {0.717760f, 0.717760f, 0.843420f, 0.843420f},
    {0.419279f, 0.000000f, 0.611786f, 0.293279f},
    {0.434246f, 0.030173f, 0.633845f, 0.429089f},
    {0.446849f, 0.077062f, 0.646934f, 0.515342f},
    {0.457559f, 0.134251f, 0.657003f, 0.567585f},
    {0.466897f, 0.198346f, 0.665067f, 0.605088f},
    {0.475199f, 0.267506f, 0.672445f, 0.633516f},
    {0.482688f, 0.340604f, 0.678797f, 0.655860f},
    {0.489518f, 0.416881f, 0.683679f, 0.673948f},
    {0.495802f, 0.495802f, 0.689159f, 0.689159f},
    {0.256504f, 0.000000f, 0.451727f, 0.167866f},
    {0.267798f, 0.013732f, 0.469653f, 0.240178f},
    {0.278744f, 0.038053f, 0.485084f, 0.320912f},
    {0.288851f, 0.072034f, 0.498342f, 0.382258f},
    {0.298123f, 0.113514f, 0.508354f, 0.427357f},
    {0.306660f, 0.160902f, 0.518366f, 0.464958f},
    {0.314560f, 0.213081f, 0.527261f, 0.495539f},
    {0.321912f, 0.269244f, 0.534503f, 0.521138f},
    {0.328787f, 0.328787f, 0.541340f, 0.541340f},
    {0.150679f, 0.000000f, 0.318497f, 0.094017f},
    {0.158118f, 0.006220f, 0.331199f, 0.128687f},
    {0.165992f, 0.017715f, 0.345745f, 0.182768f},
    {0.173861f, 0.036043f, 0.358412f, 0.235688f},
    {0.181497f, 0.060802f, 0.370356f, 0.281425f},
    {0.188821f, 0.091171f, 0.381059f, 0.320414f},
    {0.195813f, 0.126377f, 0.390989f, 0.354799f},
    {0.202480f, 0.165776f, 0.399754f, 0.382877f},
    {0.208841f, 0.208841f, 0.407897f, 0.407897f},
    {0.084832f, 0.000000f, 0.214029f, 0.051054f},
    {0.089332f, 0.002806f, 0.222133f, 0.067466f},
    {0.094351f, 0.007911f, 0.233385f, 0.096776f},
    {0.099710f, 0.016920f, 0.244697f, 0.133753f},
    {0.105215f, 0.030488f, 0.254891f, 0.170949f},
    {0.110737f, 0.048577f, 0.265454f, 0.205802f},
    {0.116201f, 0.070896f, 0.274953f, 0.237978f},
    {0.121564f, 0.097087f, 0.284344f, 0.266478f},
    {0.126802f, 0.126802f, 0.292591f, 0.292591f},
    {0.045694f, 0.000000f, 0.136863f, 0.026732f},
    {0.048241f, 0.001245f, 0.141364f, 0.034585f},
    {0.051171f, 0.003432f, 0.149232f, 0.048612f},
    {0.054460f, 0.007513f, 0.157698f, 0.070389f},
    {0.058023f, 0.014336f, 0.166338f, 0.096135f},
    {0.061769f, 0.024314f, 0.174836f, 0.123043f},
    {0.065625f, 0.037548f, 0.183688f, 0.150034f},
    {0.069535f, 0.053977f, 0.191510f, 0.175538f},
    {0.073461f, 0.073461f, 0.199349f, 0.199349f},
    {0.023510f, 0.000000f, 0.077129f, 0.013442f},
    {0.024869f, 0.000538f, 0.085448f, 0.017183f},
    {0.026462f, 0.001452f, 0.090315f, 0.023543f},
    {0.028315f, 0.003182f, 0.096155f, 0.034655f},
    {0.030418f, 0.006341f, 0.102236f, 0.050200f},
    {0.032734f, 0.011429f, 0.108813f, 0.068517f},
    {0.035219f, 0.018747f, 0.115228f, 0.088260f},
    {0.037833f, 0.028438f, 0.122056f, 0.108488f},
    {0.040542f, 0.040542f, 0.128762f, 0.128762f},
    {0.011538f, 0.000000f, 0.044156f, 0.006482f},
    {0.012223f, 0.000225f, 0.047926f, 0.008212f},
    {0.013036f, 0.000598f, 0.051036f, 0.011021f},
    {0.014006f, 0.001295f, 0.054716f, 0.016106f},
    {0.015150f, 0.002650f, 0.058910f, 0.024443f},
    {0.016466f, 0.005048f, 0.063299f, 0.035358f},
    {0.017940f, 0.008814f, 0.068245f, 0.048820f},
    {0.019553f, 0.014178f, 0.073202f, 0.063263f},
    {0.021283f, 0.021283f, 0.078274f, 0.078274f},
    {0.005393f, 0.000000f, 0.023970f, 0.002987f},
    {0.005721f, 0.000091f, 0.025678f, 0.003757f},
    {0.006113f, 0.000238f, 0.027489f, 0.004974f},
    {0.006588f, 0.000508f, 0.029659f, 0.007179f},
    {0.007165f, 0.001050f, 0.031967f, 0.011055f},
    {0.007856f, 0.002097f, 0.034811f, 0.017039f},
    {0.008665f, 0.003899f, 0.037976f, 0.024920f},
    {0.009587f, 0.006680f, 0.041446f, 0.034527f},
    {0.010615f, 0.010615f, 0.045104f, 0.045104f},
    {0.002399f, 0.000000f, 0.012328f, 0.001312f},
    {0.002547f, 0.000035f, 0.013069f, 0.001643f},
    {0.002725f, 0.000092f, 0.014027f, 0.002155f},
    {0.002944f, 0.000192f, 0.015131f, 0.003053f},
    {0.003216f, 0.000397f, 0.016461f, 0.004752f},
    {0.003553f, 0.000821f, 0.018095f, 0.007644f},
    {0.003965f, 0.001623f, 0.019957f, 0.011976f},
    {0.004455f, 0.002971f, 0.022108f, 0.017660f},
    {0.005024f, 0.005024f, 0.024478f, 0.024478f},
    {0.001014f, 0.000000f, 0.006003f, 0.000549f},
    {0.001077f, 0.000013f, 0.006309f, 0.000685f},
    {0.001154f, 0.000034f, 0.006766f, 0.000892f},
    {0.001249f, 0.000070f, 0.007300f, 0.001243f},
    {0.001369f, 0.000143f, 0.007962f, 0.001922f},
    {0.001523f, 0.000304f, 0.008857f, 0.003222f},
    {0.001717f, 0.000635f, 0.009860f, 0.005362f},
    {0.001960f, 0.001246f, 0.011085f, 0.008492f},
    {0.002254f, 0.002254f, 0.012501f, 0.012501f},
    {0.000407f, 0.000000f, 0.002766f, 0.000218f},
    {0.000433f, 0.000005f, 0.002878f, 0.000272f},
    {0.000464f, 0.000012f, 0.003077f, 0.000352f},
    {0.000503f, 0.000025f, 0.003335f, 0.000485f},
    {0.000553f, 0.000050f, 0.003649f, 0.000742f},
    {0.000618f, 0.000106f, 0.004071f, 0.001266f},
    {0.000704f, 0.000234f, 0.004606f, 0.002252f},
    {0.000816f, 0.000493f, 0.005252f, 0.003791f},
    {0.000958f, 0.000958f, 0.006020f, 0.006020f},
    {0.000155f, 0.000000f, 0.001205f, 0.000082f},
    {0.000165f, 0.000002f, 0.001245f, 0.000102f},
    {0.000177f, 0.000004f, 0.001329f, 0.000132f},
    {0.000192f, 0.000008f, 0.001434f, 0.000181f},
    {0.000211f, 0.000016f, 0.001580f, 0.000273f},
    {0.000237f, 0.000035f, 0.001764f, 0.000470f},
    {0.000273f, 0.000081f, 0.002007f, 0.000874f},
    {0.000321f, 0.000184f, 0.002339f, 0.001605f},
    {0.000385f, 0.000385f, 0.002737f, 0.002737f},
    {0.000056f, 0.000000f, 0.000435f, 0.000030f},
    {0.000059f, 0.000001f, 0.000508f, 0.000037f},
    {0.000064f, 0.000001f, 0.000540f, 0.000047f},
    {0.000069f, 0.000003f, 0.000584f, 0.000064f},
    {0.000077f, 0.000005f, 0.000642f, 0.000096f},
    {0.000086f, 0.000011f, 0.000719f, 0.000164f},
    {0.000100f, 0.000026f, 0.000827f, 0.000319f},
    {0.000119f, 0.000064f, 0.000974f, 0.000630f},
    {0.000147f, 0.000147f, 0.001168f, 0.001168f},
    {0.000019f, 0.000000f, 0.000168f, 0.000010f},
    {0.000020f, 0.000000f, 0.000186f, 0.000012f},
    {0.000022f, 0.000000f, 0.000201f, 0.000016f},
    {0.000024f, 0.000001f, 0.000219f, 0.000022f},
    {0.000026f, 0.000002f, 0.000242f, 0.000032f},
    {0.000030f, 0.000003f, 0.000274f, 0.000054f},
    {0.000035f, 0.000008f, 0.000316f, 0.000109f},
    {0.000042f, 0.000021f, 0.000381f, 0.000233f},
    {0.000053f, 0.000053f, 0.000468f, 0.000468f},
    {0.000006f, 0.000000f, 0.000061f, 0.000003f},
    {0.000007f, 0.000000f, 0.000066f, 0.000004f},
    {0.000007f, 0.000000f, 0.000072f, 0.000005f},
    {0.000008f, 0.000000f, 0.000078f, 0.000007f},
    {0.000009f, 0.000000f, 0.000087f, 0.000010f},
    {0.000010f, 0.000001f, 0.000098f, 0.000017f},
    {0.000011f, 0.000002f, 0.000115f, 0.000035f},
    {0.000014f, 0.000007f, 0.000139f, 0.000080f},
    {0.000018f, 0.000018f, 0.000175f, 0.000175f},
    {0.000002f, 0.000000f, 0.000021f, 0.000001f},
    {0.000002f, 0.000000f, 0.000022f, 0.000001f},
    {0.000002f, 0.000000f, 0.000024f, 0.000002f},
    {0.000002f, 0.000000f, 0.000026f, 0.000002f},
    {0.000003f, 0.000000f, 0.000029f, 0.000003f},
    {0.000003f, 0.000000f, 0.000033f, 0.000005f},
    {0.000004f, 0.000001f, 0.000039f, 0.000011f},
    {0.000004f, 0.000002f, 0.000048f, 0.000026f},
    {0.000006f, 0.000006f, 0.000063f, 0.000063f},
    {0.000001f, 0.000000f, 0.000007f, 0.000000f},
    {0.000001f, 0.000000f, 0.000007f, 0.000000f},
    {0.000001f, 0.000000f, 0.000008f, 0.000000f},
    {0.000001f, 0.000000f, 0.000008f, 0.000001f},
    {0.000001f, 0.000000f, 0.000009f, 0.000001f},
    {0.000001f, 0.000000f, 0.000011f, 0.000001f},
    {0.000001f, 0.000000f, 0.000013f, 0.000003f},
    {0.000001f, 0.000001f, 0.000016f, 0.000008f},
    {0.000002f, 0.000002f, 0.000021f, 0.000021f},
    {0.000000f, 0.000000f, 0.000002f, 0.000000f},
    {0.000000f, 0.000000f, 0.000002f, 0.000000f},
    {0.000000f, 0.000000f, 0.000002f, 0.000000f},
    {0.000000f, 0.000000f, 0.000003f, 0.000000f},
    {0.000000f, 0.000000f, 0.000003f, 0.000000f},
    {0.000000f, 0.000000f, 0.000003f, 0.000000f},
    {0.000000f, 0.000000f, 0.000004f, 0.000001f},
    {0.000000f, 0.000000f, 0.000005f, 0.000002f},
    {0.000000f, 0.000000f, 0.000007f, 0.000007f},
    {0.000000f, 0.000000f, 0.000001f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000001f},
    {0.000000f, 0.000000f, 0.000002f, 0.000002f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000001f, 0.000001f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f},
    {0.000000f, 0.000000f, 0.000000f, 0.000000f}};

using Mat3 = std::array<std::array<float, 3>, 3>;

void shrink_moments(float tau, float kappa, float out[4])
{
    const float t = std::clamp(tau / 0.25f, 0.0f, 32.0f), k = std::clamp(kappa, 0.0f, 1.0f) * 8;
    const int ti = std::min(int(t), 31), ki = std::min(int(k), 7);
    const float ft = t - ti, fk = k - ki;
    for (int i = 0; i < 4; ++i)
    {
        const float a = kShrinkMoments[ti * 9 + ki][i] + fk * (kShrinkMoments[ti * 9 + ki + 1][i] - kShrinkMoments[ti * 9 + ki][i]);
        const float b = kShrinkMoments[(ti + 1) * 9 + ki][i] + fk * (kShrinkMoments[(ti + 1) * 9 + ki + 1][i] - kShrinkMoments[(ti + 1) * 9 + ki][i]);
        out[i] = a + ft * (b - a);
    }
}

float tail(float t)
{
    const float x = t / std::sqrt(2.0f), u = 1 / (1 + 0.3275911f * x);
    const float e = u * (0.254829592f + u * (-0.284496736f + u * (1.421413741f + u * (-1.453152027f + u * 1.061405429f)))) * std::exp(-x * x);
    return 0.5f * e;
}

}  // namespace

// shaders/detail.hlsli's detail_retained(). `gain`, when asked: the
// shrinkers' mean Jacobian (Stein's lemma), luminance's slope and the colour
// pair's, as a 3 x 3.
std::array<std::array<float, 3>, 3> noise_retained(const std::array<std::array<float, 3>, 3>& s, float tl, float tc, std::array<std::array<float, 3>, 3>* gain)
{
    Mat3 r{};
    float slope = 1;
    if (gain)
        *gain = Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    if (s[0][0] > 0)
    {
        const float tau = tl / std::sqrt(s[0][0]), q = tail(tau), phi = std::exp(-tau * tau / 2) / 2.5066283f;
        r[0][0] = s[0][0] * 2 * ((1 + tau * tau) * q - tau * phi);
        slope = 2 * q;
    }
    const float half = (s[1][1] + s[2][2]) / 2, d = (s[1][1] - s[2][2]) / 2, root = std::sqrt(d * d + s[1][2] * s[1][2]);
    const float l1 = half + root, l2 = std::max(half - root, 0.0f);
    if (l1 > 0)
    {
        const float angle = 0.5f * std::atan2(2 * s[1][2], s[1][1] - s[2][2]);
        const float v1[2] = {std::cos(angle), std::sin(angle)}, v2[2] = {-v1[1], v1[0]};
        float m[4];
        shrink_moments(tc / std::sqrt(l1), l2 / l1, m);
        float kept[2][2], jac[2][2];
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
            {
                kept[i][j] = l1 * (m[0] * v1[i] * v1[j] + m[1] * v2[i] * v2[j]);
                jac[i][j] = m[2] * v1[i] * v1[j] + m[3] * v2[i] * v2[j];
            }
        r[1][1] = kept[0][0], r[1][2] = r[2][1] = kept[0][1], r[2][2] = kept[1][1];
        for (int i = 0; i < 2; ++i)
            r[0][1 + i] = r[1 + i][0] = slope * (jac[i][0] * s[0][1] + jac[i][1] * s[0][2]);
        if (gain)
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    (*gain)[1 + i][1 + j] = jac[i][j];
    }
    else
        r[0][1] = r[1][0] = slope * s[0][1], r[0][2] = r[2][0] = slope * s[0][2];
    if (gain)
        (*gain)[0][0] = slope;
    return r;
}

void spatial_lag(int i, int& dx, int& dy)
{
    constexpr int r = NoiseModel::kSpatialReach, row = 2 * r + 1;
    if (i <= r)
        dx = i, dy = 0;
    else
        dy = 1 + (i - r - 1) / row, dx = (i - r - 1) % row - r;
}

int spatial_series(int c, int d)
{
    static constexpr int series[3][3] = {{0, 3, 5}, {4, 1, 7}, {6, 8, 2}};
    return series[c][d];
}

namespace
{

constexpr int kLags = NoiseModel::kSpatialLags;

int lag_index(int dx, int dy)
{
    constexpr int r = NoiseModel::kSpatialReach;
    return dy == 0 ? dx : r + 1 + (dy - 1) * (2 * r + 1) + dx + r;
}

// The measured correlation of colour c at a pixel and colour d (dx, dy) on,
// from the half plane it's kept in.
float cross_at(const NoiseModel& m, int c, int d, int dx, int dy)
{
    if (dy < 0 || (dy == 0 && dx < 0))
        dx = -dx, dy = -dy, std::swap(c, d);
    return m.rho[spatial_series(c, d)][lag_index(dx, dy)];
}

// kBandAutocorr's pair t at lag l (it's even).
float band(int t, int l)
{
    l = std::abs(l);
    return l < kBandLags ? kBandAutocorr[t][l] : 0;
}

}  // namespace

// The measure, as shaders/noise.hlsl's modes 4 to 6: blocks of 16 x 16
// pixels, one every 32 across and down: the half whose luminance varies
// least in the encoding (flat; texture only adds); in each, each colour
// against each (white balanced, linear) at every lag within the reach (of
// its pixels against those that far on, about the block's own means, over
// their own deviations), averaged over the blocks. A block's own mean takes
// some of the noise with it, the more the more neighbours are alike (by S /
// n, S the correlation summed over the lags, n its pixels): undone, S from
// the correction itself, three times over. Then each colour's pixel
// variance against the fit's measure (a pixel less its four neighbours'
// mean, over 1.25): 1.25 over that measure's variance, by the correlation at
// the lags it spans.
void measure_spatial(NoiseModel& m, const float* rgb, int width, int height)
{
    constexpr int kBlock = 16, kStep = 32, r = NoiseModel::kSpatialReach, S = NoiseModel::kSpatialSeries;
    constexpr int tw = kBlock + 2 * r, th = kBlock + r;
    struct Block
    {
        int x, y;
        float var;
    };
    std::vector<Block> blocks;
    for (int by = 0; by + th <= height; by += kStep)
        for (int bx = r; bx + kBlock + r <= width; bx += kStep)
            blocks.push_back({bx, by, 0});
    m.spatial = false;
    if (blocks.size() < 16)
        return;
    parallel(blocks.size(), [&](size_t i)
    {
        Block& b = blocks[i];
        double s = 0, q = 0;
        for (int y = 0; y < kBlock; ++y)
            for (int x = 0; x < kBlock; ++x)
            {
                float e[3];
                encode(m, rgb + (size_t(b.y + y) * width + b.x + x) * 3, e);
                s += e[0], q += double(e[0]) * e[0];
            }
        b.var = float(q / (kBlock * kBlock) - (s / (kBlock * kBlock)) * (s / (kBlock * kBlock)));
    });
    // The median by a histogram of log2 of the variance in quarters, as the GPU has it.
    const auto bin = [](float var) { return std::clamp(int(std::floor((std::log2(std::max(var, 1e-30f)) + 40) * 4)), 0, kSpatialBins - 1); };
    std::vector<int> counts(kSpatialBins, 0);
    for (const Block& b : blocks)
        ++counts[size_t(bin(b.var))];
    int median = 0;
    for (int seen = 0; median < kSpatialBins; ++median)
        if ((seen += counts[size_t(median)]) * 2 >= int(blocks.size()))
            break;
    std::vector<const Block*> flat;
    for (const Block& b : blocks)
        if (bin(b.var) <= median)
            flat.push_back(&b);
    std::vector<std::array<double, S * kLags>> each(flat.size());
    parallel(flat.size(), [&](size_t i)
    {
        const Block& b = *flat[i];
        std::vector<std::array<float, 3>> v(size_t(tw) * th);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x)
                for (int c = 0; c < 3; ++c)
                    v[size_t(y) * tw + x][size_t(c)] = rgb[(size_t(b.y + y) * width + b.x - r + x) * 3 + c];
        double mean[3] = {};
        for (int y = 0; y < kBlock; ++y)
            for (int x = 0; x < kBlock; ++x)
                for (int c = 0; c < 3; ++c)
                    mean[c] += v[size_t(y) * tw + x + r][size_t(c)] / double(kBlock * kBlock);
        for (auto& p : v)
            for (int c = 0; c < 3; ++c)
                p[size_t(c)] -= float(mean[c]);
        double zero[3] = {};
        for (int y = 0; y < kBlock; ++y)
            for (int x = 0; x < kBlock; ++x)
                for (int c = 0; c < 3; ++c)
                    zero[c] += double(v[size_t(y) * tw + x + r][size_t(c)]) * v[size_t(y) * tw + x + r][size_t(c)];
        auto& out = each[i];
        for (int l = 0; l < kLags; ++l)
        {
            int dx, dy;
            spatial_lag(l, dx, dy);
            double sum[3][3] = {};
            for (int y = 0; y < kBlock; ++y)
                for (int x = 0; x < kBlock; ++x)
                {
                    const auto& a = v[size_t(y) * tw + x + r];
                    const auto& e = v[size_t(y + dy) * tw + x + r + dx];
                    for (int c = 0; c < 3; ++c)
                        for (int d = 0; d < 3; ++d)
                            sum[c][d] += double(a[size_t(c)]) * e[size_t(d)];
                }
            for (int c = 0; c < 3; ++c)
                for (int d = 0; d < 3; ++d)
                {
                    const double den = std::sqrt(zero[c] * zero[d]);
                    out[size_t(spatial_series(c, d) * kLags + l)] = den > 0 ? sum[c][d] / den : 0;
                }
        }
    });
    for (int k = 0; k < S; ++k)
        for (int l = 0; l < kLags; ++l)
        {
            double t = 0;
            for (const auto& e : each)
                t += e[size_t(k * kLags + l)];
            m.rho[k][l] = float(t / double(each.size()));
        }
    // The blocks' own means undone, each series by its own sum over the lags
    // (the whole window: a cross series' other half is its mirror's).
    float measured[S][kLags];
    for (int k = 0; k < S; ++k)
        std::copy(m.rho[k], m.rho[k] + kLags, measured[k]);
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        float sums[S];
        for (int c = 0; c < 3; ++c)
            for (int d = 0; d < 3; ++d)
            {
                float t = 0;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        t += cross_at(m, c, d, dx, dy);
                sums[spatial_series(c, d)] = t;
            }
        for (int k = 0; k < S; ++k)
        {
            const float f = std::clamp(sums[k], 0.0f, 64.0f) / (kBlock * kBlock);
            for (int l = 0; l < kLags; ++l)
                m.rho[k][l] = measured[k][l] * (1 - f) + f;
        }
    }
    for (int c = 0; c < 3; ++c)
        m.rho[c][0] = 1;
    for (int c = 0; c < 3; ++c)
    {
        const auto at = [&](int dx, int dy) { return cross_at(m, c, c, dx, dy); };
        const float v = 1 + (4 + 2 * (at(2, 0) + at(0, 2) + 2 * at(1, 1) + 2 * at(1, -1))) / 16 - (at(1, 0) + at(0, 1));
        m.pixel[c] = std::clamp(1.25f / std::max(v, 1e-3f), 0.5f, 8.0f);
    }
    m.spatial = true;
}

// shaders/detail.hlsl's detail_shaping(). Noise of the measured
// correlations R_cd (colour c at a pixel against colour d a lag on: each
// colour's own, and each against each, which an mRAW's luma shared by all
// three at short range and its colour filled over blocks at long range make
// unlike) has, per unit of covariance at a pixel, in scale j's detail of
// each pair of colours the sum over the lags of R_cd times the scale's
// autocorrelation: c_j - c_j+1 is in two dimensions P_j x P_j - P_j+1 x
// P_j+1 (the coarsest P_5 x P_5), so a sum of products of kBandAutocorr's.
// The sharpening's high pass, d = (1 - G x G) of luminance (G the
// Gaussian's taps), and luminance against it, alike with each A convolved
// with G (and G with itself). Each per unit of the pair's deviations' product
// (k, d, x: the correlation itself summed, a pair's at a pixel or a lag on
// alike, never over its value at a pixel). And the whole high pass's, of the pixel's own (D, X): the
// sharpening of noise nothing was taken out of. Unmeasured: independent
// noise, each pair alike per unit of its covariance at a pixel.
NoiseShaping noise_shaping(const NoiseModel* m, const DetailFilter* f)
{
    NoiseShaping out;
    constexpr int r = NoiseModel::kSpatialReach;
    const bool spatial = m && m->spatial;
    out.measured = spatial;
    static constexpr int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
    if (spatial)
        for (int p = 0; p < 3; ++p)
            out.correlation[p] = (cross_at(*m, pairs[p][0], pairs[p][1], 0, 0) + cross_at(*m, pairs[p][1], pairs[p][0], 0, 0)) / 2;
    std::vector<float> g{1}, gg{1};
    int half = 0;
    if (f && f->sharpen)
    {
        out.amount = f->amount, out.halo = f->halo;
        half = int(std::ceil(3 * f->sigma));
        g.assign(size_t(2 * half + 1), 0);
        float total = 0;
        for (int k = -half; k <= half; ++k)
            total += g[size_t(k + half)] = std::exp(-0.5f * k * k / (f->sigma * f->sigma));
        for (float& v : g)
            v /= total;
        gg.assign(size_t(4 * half + 1), 0);
        for (int a = 0; a <= 2 * half; ++a)
            for (int b = 0; b <= 2 * half; ++b)
                gg[size_t(a + b)] += g[size_t(a)] * g[size_t(b)];
    }
    // Each pair's A, G * A and G * G * A at lags -r..r.
    float A[11][2 * r + 1], GA[11][2 * r + 1], GGA[11][2 * r + 1];
    for (int t = 0; t < 11; ++t)
        for (int l = -r; l <= r; ++l)
        {
            float ga = 0, gga = 0;
            for (int k = -half; k <= half; ++k)
                ga += g[size_t(k + half)] * band(t, l - k);
            for (int k = -2 * half; k <= 2 * half; ++k)
                gga += gg[size_t(k + 2 * half)] * band(t, l - k);
            A[t][l + r] = band(t, l), GA[t][l + r] = ga, GGA[t][l + r] = gga;
        }
    const auto gat = [&](int l) { return std::abs(l) <= half ? g[size_t(l + half)] : 0.0f; };
    const auto ggat = [&](int l) { return std::abs(l) <= 2 * half ? gg[size_t(l + 2 * half)] : 0.0f; };
    for (int c = 0; c < 3; ++c)
        for (int e = c; e < 3; ++e)
        {
            const auto rho = [&](int dx, int dy) -> double
            { return spatial ? (cross_at(*m, c, e, dx, dy) + cross_at(*m, e, c, dx, dy)) / 2 : (dx == 0 && dy == 0); };
            double total_d = 0, total_x = 0;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                {
                    const double rh = rho(dx, dy), one = dx == 0 && dy == 0, blur = gat(dx) * gat(dy);
                    total_d += rh * (one - 2 * blur + ggat(dx) * ggat(dy)), total_x += rh * (one - blur);
                }
            out.D[c][e] = out.D[e][c] = float(total_d), out.X[c][e] = out.X[e][c] = float(total_x);
            for (int j = 0; j < 6; ++j)
            {
                // The terms: (pair, coefficient).
                const int terms[3][2] = {{j, 1}, {6 + j, -2}, {j + 1, 1}};
                const int count = j < 5 ? 3 : 1;
                double w = 0, q = 0, qq = 0;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        const double rh = rho(dx, dy);
                        for (int u = 0; u < count; ++u)
                        {
                            const int t = terms[u][0], k = terms[u][1];
                            w += k * rh * A[t][dx + r] * A[t][dy + r];
                            q += k * rh * GA[t][dx + r] * GA[t][dy + r];
                            qq += k * rh * GGA[t][dx + r] * GGA[t][dy + r];
                        }
                    }
                out.k[j][c][e] = out.k[j][e][c] = float(w), out.d[j][c][e] = out.d[j][e][c] = float(w - 2 * q + qq),
                out.x[j][c][e] = out.x[j][e][c] = float(w - q);
            }
        }
    return out;
}

// A symmetric 3 x 3's eigenvalues (w) and eigenvectors (v's columns), by
// Jacobi's rotations (shaders/detail.hlsli's detail_eigen()).
static void symmetric_eigen(const Mat3& m, Mat3& v, float w[3])
{
    double a[3][3], u[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            a[i][j] = (double(m[i][j]) + m[j][i]) / 2;
    for (int sweep = 0; sweep < 8; ++sweep)
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (a[p][q] == 0)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1)), c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k)
                {
                    const double kp = a[k][p], kq = a[k][q];
                    a[k][p] = c * kp - s * kq, a[k][q] = s * kp + c * kq;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double pk = a[p][k], qk = a[q][k];
                    a[p][k] = c * pk - s * qk, a[q][k] = s * pk + c * qk;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double kp = u[k][p], kq = u[k][q];
                    u[k][p] = c * kp - s * kq, u[k][q] = s * kp + c * kq;
                }
            }
    for (int i = 0; i < 3; ++i)
    {
        w[i] = float(a[i][i]);
        for (int j = 0; j < 3; ++j)
            v[i][j] = float(u[i][j]);
    }
}

// A symmetric 3 x 3 to a power, its eigenvalues below nothing taken as
// nothing (power 1: the nearest positive semi-definite); for a negative
// power, those under a millionth of the greatest as that.
static Mat3 symmetric_power(const Mat3& m, float power)
{
    Mat3 v;
    float w[3];
    symmetric_eigen(m, v, w);
    const float top = std::max({w[0], w[1], w[2], 0.0f});
    Mat3 out{};
    if (top <= 0)
        return out;
    for (int k = 0; k < 3; ++k)
    {
        const float x = power < 0 ? std::max(w[k], 1e-6f * top) : std::max(w[k], 0.0f);
        const float f = power == 1 ? x : std::pow(x, power);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[i][j] += v[i][k] * f * v[j][k];
    }
    return out;
}

static Mat3 transposed(const Mat3& m)
{
    Mat3 t{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            t[i][j] = m[j][i];
    return t;
}

static Mat3 product(const Mat3& a, const Mat3& b)
{
    Mat3 out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                out[i][j] += a[i][k] * b[k][j];
    return out;
}


// Symmetric transport M B M = S without squaring its condition number.
// B=L L', S=R R': if F=L'R, then M=L'^-1 sqrt(F F') L^-1.
// A one-sided Jacobi SVD works on F itself: forming F F' first loses a
// quiet colour when its variance is ten thousand times smaller. The tiny
// Cholesky floor covers roundoff at a semidefinite endpoint, not a colour's
// noise share. shaders/detail.hlsli uses the same float operations.
static Mat3 transport(const Mat3& b, const Mat3& s)
{
    const auto chol = [](const Mat3& a)
    {
        Mat3 l{};
        const float top = std::max({a[0][0], a[1][1], a[2][2], 1e-30f});
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j <= i; ++j)
            {
                float x = (a[i][j] + a[j][i]) * .5f;
                for (int k = 0; k < j; ++k)
                    x -= l[i][k] * l[j][k];
                l[i][j] = i == j ? std::sqrt(std::max(x, top * 1e-12f)) : x / l[j][j];
            }
        return l;
    };
    const Mat3 l = chol(b), r = chol(s);
    Mat3 f = product(transposed(l), r);
    // Orthogonalise columns directly; no F F' eigenproblem loses a quiet axis.
    for (int sweep = 0; sweep < 12; ++sweep)
        for (int pair = 0; pair < 3; ++pair)
        {
            const int p = pair == 2 ? 1 : 0, q = pair == 0 ? 1 : 2;
            float aa = 0, bb = 0, ab = 0;
            for (int k = 0; k < 3; ++k)
                aa += f[k][p]*f[k][p], bb += f[k][q]*f[k][q], ab += f[k][p]*f[k][q];
            if (std::fabs(ab) <= 1e-7f * std::sqrt(aa * bb))
                continue;
            const float tau = (bb-aa)/(2*ab);
            const float t = (tau >= 0 ? 1.f : -1.f)/(std::fabs(tau)+std::sqrt(1+tau*tau));
            const float c = 1/std::sqrt(1+t*t), sn = c*t;
            for (int k = 0; k < 3; ++k)
            {
                const float a = f[k][p], d = f[k][q];
                f[k][p] = c*a-sn*d, f[k][q] = sn*a+c*d;
            }
        }
    Mat3 root{};
    for (int k = 0; k < 3; ++k)
    {
        float length = 0;
        for (int i = 0; i < 3; ++i)
            length += f[i][k] * f[i][k];
        const float inv = length > 0 ? 1/std::sqrt(length) : 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                root[i][j] += f[i][k] * f[j][k] * inv;
    }
    Mat3 inv{};
    for (int j = 0; j < 3; ++j)
        for (int i = j; i < 3; ++i)
        {
            float x = i == j ? 1.f : 0.f;
            for (int k = j; k < i; ++k)
                x -= l[i][k]*inv[k][j];
            inv[i][j] = x/l[i][i];
        }
    const Mat3 m = product(product(transposed(inv), root), inv);
    Mat3 out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = (m[i][j] + m[j][i]) * .5f;
    return out;
}

// shaders/detail.hlsli's detail_kept(): each colour's share of its
// transformed noise kept, from the colours' covariance there (`e`), each
// scale's thresholds and what the scales and the sharpening make of each
// pair of colours (`shaping`). Scale j's coefficients have the covariance,
// each pair's unit times its share at that scale, in luminance and the
// colour differences; each scale shrunk (noise_retained()), the coarsest
// left, and the pixel's covariance taken through the share of the scales'
// sum kept (a congruence: positive semi-definite). Then the sharpening, luminance plus amount times d
// h / (h + |d|), d its high pass of what's kept (normal): the whole high
// pass's variance and covariances of the pixel's own noise (D, X), as much
// of them as the scales' shares through it keep (luminance's by the share of
// their sum kept; the colour differences' less what the scales take out, at
// the same scale). Nothing taken out, that's the kernel's own propagation,
// however the colours correlate a pixel or a lag on. It adds, by Stein's lemma, 2 amount
// E[h^2 / (h + |d|)^2] cov(luminance, d) and amount^2 E[d^2 h^2 / (h +
// |d|)^2] to luminance's variance, and amount times that first expectation
// cov(d, colour) to its covariance with each colour difference.
void noise_kept_of(const std::array<std::array<float, 3>, 3>& e, const float tl[5], const float tc[5], const NoiseShaping& shaping, float out[3])
{
    static constexpr float to[3][3] = {{0.25f, 0.5f, 0.25f}, {1, -1, 0}, {0, -1, 1}};
    static constexpr float back[3][3] = {{1, 0.75f, -0.25f}, {1, -0.25f, -0.25f}, {1, -0.25f, 0.75f}};
    const auto into = [&](const Mat3& m)
    {
        Mat3 s{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    for (int l = 0; l < 3; ++l)
                        s[i][j] += to[i][k] * m[k][l] * to[j][l];
        return s;
    };
    // Luminance against luminance and against each colour difference, of a
    // covariance between the colours.
    const auto with = [&](const Mat3& m, int row)
    {
        float t = 0;
        for (int k = 0; k < 3; ++k)
            for (int l = 0; l < 3; ++l)
                t += to[0][k] * m[k][l] * to[row][l];
        return t;
    };
    const Mat3 s = into(e);
    // Each pair's unit: measured, its deviations' product (the shaping holds
    // its correlation, at a pixel and a lag on alike); else its covariance at
    // a pixel. Each scale's coefficients' covariance (b_j, the field's through
    // the scale's filter: positive semi-definite) and what its shrinkers keep
    // (r_j): the shrinkers' mean Jacobian G_j (Stein's lemma: luminance's
    // slope, the colour pair's 2 x 2) carries b_j to G_j b_j G_j', and what
    // they keep past that, N_j = r_j - G_j b_j G_j', is luminance's and the
    // colour pair's apart, each positive semi-definite (Cauchy-Schwarz: a
    // shrunk coefficient's covariance with itself is at least as much as its
    // covariance with the coefficient makes it). At the pixel: each scale's
    // share of its covariance S, s_j = M b_j M (M the one symmetric positive
    // definite M B M = S, B the scales' sum: the shares sum to S, and where S
    // and B share their axes each axis's own ratio, no colour or luminance
    // put first), through G_j, and N_j through M: what's kept, the sum of
    // G_j s_j G_j' + M N_j M, positive semi-definite scale by scale, S with
    // nothing taken out, nothing with it all.
    Mat3 unit{};
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            unit[a][b] = shaping.measured ? std::sqrt(std::max(e[a][a], 0.0f) * std::max(e[b][b], 0.0f)) : e[a][b];
    // The sharpening's high pass: the whole high pass's of the pixel's own
    // noise (D, X), as much as the scales' shares through it keep; the colour
    // differences' shares spread as luminance's makes up the whole.
    Mat3 sum_b{}, scale_b[6], scale_g[6], scale_r[6];
    double d_all = 0, d_kept = 0, x_all = 0, x_kept = 0, xl[6], xc[2][6];
    float kc[2][6];
    for (int j = 0; j < 6; ++j)
    {
        Mat3 ej{}, ed{}, ex{};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                ej[a][b] = unit[a][b] * shaping.k[j][a][b], ed[a][b] = unit[a][b] * shaping.d[j][a][b], ex[a][b] = unit[a][b] * shaping.x[j][a][b];
        const Mat3 bj = into(ej);
        Mat3 gj{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        const Mat3 rj = j < 5 ? noise_retained(bj, tl[j], tc[j], &gj) : bj;
        scale_b[j] = bj, scale_g[j] = gj, scale_r[j] = rj;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                sum_b[a][b] += bj[a][b];
        const float kl = bj[0][0] > 0 ? rj[0][0] / bj[0][0] : 1;
        const double dl = with(ed, 0);
        xl[j] = with(ex, 0);
        d_all += dl, d_kept += kl * dl, x_all += xl[j], x_kept += kl * xl[j];
        for (int c = 0; c < 2; ++c)
        {
            kc[c][j] = std::fabs(bj[0][1 + c]) > 1e-30f ? rj[0][1 + c] / bj[0][1 + c] : 1;
            xc[c][j] = with(ex, 1 + c);
        }
    }
    // M: the one symmetric positive definite M B M = S, B^-1/2 (B^1/2 S
    // B^1/2)^1/2 B^-1/2: no colour or luminance put first; where S and B
    // share their axes, each axis's own ratio.
    const Mat3 m = transport(sum_b, s);
    Mat3 kept{};
    for (int j = 0; j < 6; ++j)
    {
        const Mat3 share = product(product(m, scale_b[j]), transposed(m));
        const Mat3 linear = product(product(scale_g[j], share), transposed(scale_g[j]));
        const Mat3 through = product(product(scale_g[j], scale_b[j]), transposed(scale_g[j]));
        Mat3 past{};  // N_j: luminance's, and the colour pair's
        past[0][0] = scale_r[j][0][0] - through[0][0];
        for (int a = 1; a < 3; ++a)
            for (int b = 1; b < 3; ++b)
                past[a][b] = scale_r[j][a][b] - through[a][b];
        past = symmetric_power(past, 1);  // the moments' tables' rounding: not below nothing
        const Mat3 rest = product(product(m, past), m);
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                kept[a][b] += linear[a][b] + rest[a][b];
    }
    Mat3 whole_d{}, whole_x{};
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            whole_d[a][b] = unit[a][b] * shaping.D[a][b], whole_x[a][b] = unit[a][b] * shaping.X[a][b];
    const double var_d = with(whole_d, 0) * (d_all > 0 ? d_kept / d_all : 1), cov_ld = with(whole_x, 0) * (x_all > 0 ? x_kept / x_all : 1);
    const double scale_x = x_all > 0 ? with(whole_x, 0) / x_all : 1;
    double cov_dc[2] = {};
    for (int c = 0; c < 2; ++c)
    {
        double sum = 0;
        for (int j = 0; j < 6; ++j)
            sum += scale_x * xc[c][j];
        for (int j = 0; j < 6; ++j)
            cov_dc[c] += kc[c][j] * (scale_x * xc[c][j] + (x_all > 0 ? xl[j] / x_all : 1.0 / 6) * (with(whole_x, 1 + c) - sum));
    }
    if (shaping.amount > 0 && var_d > 0)
    {
        // E over d of h^2 / (h + |d|)^2 and d^2 h^2 / (h + |d|)^2.
        const double sd = std::sqrt(var_d), h = shaping.halo;
        double e1 = 0, e2 = 0, total = 0;
        for (int i = -20; i <= 20; ++i)
        {
            const double z = i * 0.25, p = std::exp(-0.5 * z * z), d = std::fabs(z * sd), f = h / (h + d);
            e1 += p * f * f, e2 += p * d * d * f * f, total += p;
        }
        e1 /= total, e2 /= total;
        const double A = shaping.amount;
        kept[0][0] += float(2 * A * e1 * cov_ld + A * A * e2);
        for (int c = 0; c < 2; ++c)
            kept[0][1 + c] += float(A * e1 * cov_dc[c]), kept[1 + c][0] = kept[0][1 + c];
        // What the sharpening's model adds kept a covariance: no direction of
        // it below nothing.
        kept = symmetric_power(kept, 1);
    }
    for (int c = 0; c < 3; ++c)
    {
        float before = 0, after = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                before += back[c][i] * s[i][j] * back[c][j], after += back[c][i] * kept[i][j] * back[c][j];
        out[c] = before > 0 ? after / before : 1;
    }
}

// At each colour's level y, by the model's pixel law through the
// transform's slope there (squared, either side of 0), the colours
// correlated as measured, else as the develop leaves them (the light's and
// the readout's).
void noise_kept(const NoiseModel& m, const float y[3], const float tl[5], const float tc[5], const NoiseShaping& shaping, float out[3])
{
    float va[3], vb[3];
    for (int c = 0; c < 3; ++c)
    {
        const float slope = 1 / (4 * (std::fabs(y[c]) + m.offset[c]));
        va[c] = m.law_a[c] * std::max(y[c], 0.0f) * slope, vb[c] = m.law_b[c] * slope;
    }
    Mat3 e{};
    static constexpr int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
    for (int c = 0; c < 3; ++c)
        e[c][c] = va[c] + vb[c];
    for (int p = 0; p < 3; ++p)
    {
        const int i = pairs[p][0], j = pairs[p][1];
        e[i][j] = e[j][i] = shaping.measured ? shaping.correlation[p] * std::sqrt(e[i][i] * e[j][j])
                                             : m.correlation_light[p] * std::sqrt(va[i] * va[j]) + m.correlation_readout[p] * std::sqrt(vb[i] * vb[j]);
    }
    noise_kept_of(e, tl, tc, shaping, out);
}

// shaders/detail.hlsli's detail_unbiased(): f's own inverse, moved by the
// share of the noise taken out to the expectation inverse (kept over 1, as
// the sharpening can leave it, moves it the other way).
float noise_decode(const NoiseModel& m, float d, int c, float kept)
{
    const float own = inverse(d, m.offset[c]);
    return own + (1 - kept) * (noise_expectation_inverse(m, d, c) - own);
}

// shaders/detail.hlsl's DETAIL_TABLE: at each inverse table entry's
// average, how far the decode moves f's own inverse, the share kept taken
// at that entry's level for all three colours.
void noise_decode_table(const NoiseModel& m, const float tl[5], const float tc[5], const NoiseShaping& shaping, float shift[3][NoiseModel::kInverse])
{
    for (int i = 0; i < NoiseModel::kInverse; ++i)
    {
        float y[3], kept[3];
        for (int c = 0; c < 3; ++c)
            y[c] = inverse(m.from[c] + (m.to[c] - m.from[c]) * float(i) / (NoiseModel::kInverse - 1), m.offset[c]);
        for (int c = 0; c < 3; ++c)
        {
            const float at[3] = {y[c], y[c], y[c]};
            noise_kept(m, at, tl, tc, shaping, kept);
            shift[c][i] = (1 - kept[c]) * (m.inverse[c][i] - y[c]);
        }
    }
}

// shaders/detail.hlsl's decode by that table: f's own inverse of d, moved by
// the table's shift there (between entries linearly, past its ends its last).
float noise_decode_by(const NoiseModel& m, const float shift[3][NoiseModel::kInverse], float d, int c)
{
    const float t = std::clamp((d - m.from[c]) / (m.to[c] - m.from[c]) * (NoiseModel::kInverse - 1), 0.0f, float(NoiseModel::kInverse - 1));
    const int i = std::min(int(t), NoiseModel::kInverse - 2);
    return inverse(d, m.offset[c]) + shift[c][i] + (shift[c][i + 1] - shift[c][i]) * (t - i);
}

namespace
{

void decode(const NoiseModel& m, const float in[3], float rgb[3])
{
    float e[3];
    channels(in, e);
    for (int i = 0; i < 3; ++i)
        rgb[i] = inverse(e[i], m.offset[i]);
}

// The finest scale's noise in luminance and colour: the median absolute
// detail on a lattice across the whole image, as a standard deviation, in
// the encoding.
void measure(const NoiseModel& m, const float* rgb, int width, int height, float& luminance, float& colour)
{
    std::vector<float> l, c;
    const auto encoded = [&](int x, int y, float e[3])
    {
        x = std::clamp(x, 0, width - 1), y = std::clamp(y, 0, height - 1);
        encode(m, rgb + (size_t(y) * width + x) * 3, e);
    };
    for (int y = 0; y < height; y += kNoiseStride)
        for (int x = 0; x < width; x += kNoiseStride)
        {
            float centre[3], smooth[3] = {}, e[3];
            encoded(x, y, centre);
            for (int v = -2; v <= 2; ++v)
                for (int u = -2; u <= 2; ++u)
                {
                    encoded(x + u, y + v, e);
                    for (int k = 0; k < 3; ++k)
                        smooth[k] += kB3[u + 2] * kB3[v + 2] * e[k];
                }
            l.push_back(std::abs(centre[0] - smooth[0]));
            c.push_back(std::abs(centre[1] - smooth[1]));
            c.push_back(std::abs(centre[2] - smooth[2]));
        }
    const auto median = [](std::vector<float>& v)
    {
        if (v.empty())
            return 0.0f;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2] / 0.6745f;
    };
    luminance = median(l);
    colour = median(c);
}

// A soft threshold of a luminance detail, and of a colour difference's
// detail as a vector (its direction, the hue, kept).
float shrink(float d, float t)
{
    return d > t ? d - t : d < -t ? d + t : 0;
}
void shrink(float& a, float& b, float t)
{
    const float m = std::sqrt(a * a + b * b);
    const float k = m > t ? (m - t) / m : 0;
    a *= k, b *= k;
}

}  // namespace

void apply_detail(const DetailFilter& f, const float* rgb, int width, int height, float* out)
{
    float noise_l = 0, noise_c = 0;
    NoiseModel model;
    if (f.denoise && f.model)
    {
        model = fit_noise(rgb, width, height);
        if (model.fitted)
        {
            // LibRaw's develop isn't ours to know: each colour's pixel noise
            // against the fit's measure from the noise's own correlation.
            measure_spatial(model, rgb, width, height);
            for (int c = 0; c < 3; ++c)
                model.light[c] = model.readout[c] = model.pixel[c];
            tabulate_noise(model);
        }
    }
    if (f.denoise && f.model && std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "detail: noise %s: a R %.3g G %.3g B %.3g, b R %.3g G %.3g B %.3g (colours fitted %d); offset %.3g; pixel over measure %.3g %.3g %.3g\n",
                     model.fitted ? "fitted" : "not fitted (too few flat areas)", model.a[0], model.a[1], model.a[2], model.b[0], model.b[1], model.b[2],
                     model.fitted_colours, model.offset[1], model.pixel[0], model.pixel[1], model.pixel[2]);
    if (f.denoise)
        measure(model, rgb, width, height, noise_l, noise_c);
    // Each scale's thresholds, in the transformed domain (the decode's model).
    float tl[DetailFilter::kLevels], tc[DetailFilter::kLevels];
    for (int j = 0; j < DetailFilter::kLevels; ++j)
        tl[j] = f.luminance[j] * noise_l, tc[j] = f.colour[j] * noise_c;
    // The decode's shift by level, once (each pixel then looks it up).
    float shift[3][NoiseModel::kInverse] = {};
    if (f.denoise && model.fitted)
        noise_decode_table(model, tl, tc, noise_shaping(&model, &f), shift);
    // Tiles of 256 with the filter's reach around each, clamped at its
    // region's edges (the image's, at the image's): what the GPU does.
    constexpr int kTile = 256;
    const int across = (width + kTile - 1) / kTile, down = (height + kTile - 1) / kTile;
    const int half = int(std::ceil(3 * f.sigma));
    std::vector<float> gauss(size_t(2 * half + 1));
    for (int k = -half; k <= half; ++k)
        gauss[size_t(k + half)] = std::exp(-0.5f * k * k / (f.sigma * f.sigma));
    parallel(size_t(across) * down, [&](size_t t)
    {
        const int ox = int(t % across) * kTile, oy = int(t / across) * kTile;
        const int ow = std::min(kTile, width - ox), oh = std::min(kTile, height - oy);
        const int ix = std::max(0, ox - DetailFilter::kReach), iy = std::max(0, oy - DetailFilter::kReach);
        const int iw = std::min(width, ox + ow + DetailFilter::kReach) - ix, ih = std::min(height, oy + oh + DetailFilter::kReach) - iy;
        Planes cur, next, smoothed, sum;
        // The B3 spline along (dx, dy), `step` apart: luminance plainly,
        // colour guided by it (shaders/detail.hlsl's DETAIL_SMOOTH).
        const float sigma_l = f.guide * noise_l, inv = sigma_l > 0 ? 1 / (2 * sigma_l * sigma_l) : 0;
        const auto smooth = [&](const Planes& in, Planes& out, int step, int dx, int dy)
        {
            for (int y = 0; y < in.height; ++y)
                for (int x = 0; x < in.width; ++x)
                {
                    const float centre = in.at(0, x, y);
                    float l = 0, a = 0, b = 0, w = 0;
                    for (int k = -2; k <= 2; ++k)
                    {
                        const int px = x + k * step * dx, py = y + k * step * dy;
                        const float lum = in.at(0, px, py), d = lum - centre;
                        const float g = kB3[k + 2] * (inv > 0 ? std::exp(-d * d * inv) : 1);
                        l += kB3[k + 2] * lum;
                        a += g * in.at(1, px, py), b += g * in.at(2, px, py), w += g;
                    }
                    const size_t i = size_t(y) * in.width + x;
                    out.c[0][i] = l, out.c[1][i] = a / w, out.c[2][i] = b / w;
                }
        };
        cur.resize(iw, ih);
        for (int y = 0; y < ih; ++y)
            for (int x = 0; x < iw; ++x)
            {
                float e[3];
                encode(model, rgb + (size_t(iy + y) * width + ix + x) * 3, e);
                for (int k = 0; k < 3; ++k)
                    cur.c[k][size_t(y) * iw + x] = e[k];
            }
        if (f.denoise)
        {
            next.resize(iw, ih), smoothed.resize(iw, ih), sum.resize(iw, ih);
            for (int j = 0, step = 1; j < DetailFilter::kLevels; ++j, step *= 2)
            {
                smooth(cur, smoothed, step, 1, 0);
                smooth(smoothed, next, step, 0, 1);
                const int gap = 1 << j;
                for (size_t i = 0; i < size_t(iw) * ih; ++i)
                {
                    float l = cur.c[0][i] - next.c[0][i], a = cur.c[1][i] - next.c[1][i], b = cur.c[2][i] - next.c[2][i];
                    float t = f.luminance[j] * noise_l;
                    if (f.support[j] > 0)
                    {
                        // The next scale's luminance gradient here, against its noise.
                        const int x = int(i % size_t(iw)), y = int(i / size_t(iw));
                        const float gx = next.at(0, x + gap, y) - next.at(0, x - gap, y);
                        const float gy = next.at(0, x, y + gap) - next.at(0, x, y - gap);
                        const float r = std::sqrt(gx * gx + gy * gy) / 2 / (f.support[j] * noise_l);
                        t /= 1 + r * r;
                    }
                    l = shrink(l, t);
                    shrink(a, b, f.colour[j] * noise_c);
                    sum.c[0][i] += l, sum.c[1][i] += a, sum.c[2][i] += b;
                }
                std::swap(cur, next);
            }
            for (int k = 0; k < 3; ++k)
                for (size_t i = 0; i < size_t(iw) * ih; ++i)
                    cur.c[k][i] += sum.c[k][i];
        }
        // The sharpening Gaussian of the luminance, across then down.
        std::vector<float> blur_across, blurred;
        if (f.sharpen)
        {
            blur_across.resize(size_t(iw) * ih), blurred.resize(size_t(iw) * ih);
            for (int y = 0; y < ih; ++y)
                for (int x = 0; x < iw; ++x)
                {
                    float s = 0, w = 0;
                    for (int k = -half; k <= half; ++k)
                        s += gauss[size_t(k + half)] * cur.at(0, x + k, y), w += gauss[size_t(k + half)];
                    blur_across[size_t(y) * iw + x] = s / w;
                }
            for (int y = 0; y < ih; ++y)
                for (int x = 0; x < iw; ++x)
                {
                    float s = 0, w = 0;
                    for (int k = -half; k <= half; ++k)
                        s += gauss[size_t(k + half)] * blur_across[size_t(std::clamp(y + k, 0, ih - 1)) * iw + x], w += gauss[size_t(k + half)];
                    blurred[size_t(y) * iw + x] = s / w;
                }
        }
        const auto blur = [&](int x, int y) { return blurred[size_t(std::clamp(y, 0, ih - 1)) * iw + std::clamp(x, 0, iw - 1)]; };
        for (int y = oy - iy; y < oy - iy + oh; ++y)
            for (int x = ox - ix; x < ox - ix + ow; ++x)
            {
                float e[3] = {cur.at(0, x, y), cur.at(1, x, y), cur.at(2, x, y)};
                if (f.sharpen)
                {
                    const float d = e[0] - blur(x, y);
                    float mask = 1;
                    if (f.mask_high > 0)
                    {
                        const float gx = (blur(x + 1, y) - blur(x - 1, y)) / 2, gy = (blur(x, y + 1) - blur(x, y - 1)) / 2;
                        const float g = std::sqrt(gx * gx + gy * gy), s = std::clamp((g - f.mask_low) / (f.mask_high - f.mask_low), 0.0f, 1.0f);
                        mask = s * s * (3 - 2 * s);
                    }
                    e[0] += f.amount * mask * d * f.halo / (f.halo + std::abs(d));
                }
                float* o = out + (size_t(iy + y) * width + ix + x) * 3;
                decode(model, e, o);
                if (f.denoise && model.fitted)
                {
                    // By the model: each colour by the share of its noise taken out.
                    float ch[3];
                    channels(e, ch);
                    for (int k = 0; k < 3; ++k)
                        o[k] = noise_decode_by(model, shift, ch[k], k);
                }
            }
    });
}

namespace
{

// The transform in doubles, and E[f(X) | y] over the Poisson-Gaussian
// mixture by brute force (every count to 30 standard deviations, 401
// points of each Gaussian): what the decode is checked against.
double forward_d(double x, double c)
{
    const double r = std::sqrt(c);
    return x >= 0 ? std::sqrt(x + c) : 2 * r - std::sqrt(c - x);
}

double brute_expected(double y, double c, double a, double b)
{
    const double s = std::sqrt(b);
    const auto gauss = [&](double m)
    {
        if (s <= 0)
            return forward_d(m, c);
        double sum = 0, weights = 0;
        for (int k = -200; k <= 200; ++k)
        {
            const double z = k * 0.04, w = std::exp(-0.5 * z * z);
            sum += w * forward_d(m + s * z, c), weights += w;
        }
        return sum / weights;
    };
    if (y <= 0)
        return gauss(y);
    const double lambda = y / a;
    double p = std::exp(-lambda), sum = 0;
    for (int k = 0; k <= int(lambda + 30 * std::sqrt(lambda) + 30); ++k)
    {
        sum += p * gauss(a * k);
        p *= lambda / (k + 1);
    }
    return sum;
}

// A model of each colour's a and b as given (in a pixel's terms), the
// encoding by green's, its inverse tables made.
NoiseModel given_model(const float a[3], const float b[3])
{
    NoiseModel m;
    m.fitted = true, m.fitted_colours = 7;
    for (int c = 0; c < 3; ++c)
        m.a[c] = a[c], m.b[c] = b[c], m.offset[c] = std::max(3 * a[1] / 8 + b[1] / a[1], 1e-7f);
    tabulate_noise(m);
    return m;
}

// Linear RGB, 1024 x 512, levels rising across it from 0 to under kFitTop,
// each colour's noise Gaussian of variance a y + b on its own (a seeded
// Box-Muller, the same everywhere).
std::vector<float> gradient(float a, float b)
{
    constexpr int w = 1024, h = 512;
    std::vector<float> rgb(size_t(w) * h * 3);
    uint64_t state = 12345;
    const auto uniform = [&]
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return (double(state >> 11) + 0.5) / 9007199254740992.0;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const double t = x / double(w - 1), level = 0.24 * t * t;
            for (int c = 0; c < 3; ++c)
            {
                const double n = std::sqrt(-2 * std::log(uniform())) * std::cos(6.283185307179586 * uniform());
                rgb[(size_t(y) * w + x) * 3 + c] = float(level + std::sqrt(a * level + b) * n);
            }
        }
    return rgb;
}

// Grey, a Poisson-Gaussian mixture per colour (a Poisson count of mean
// level / a, times a, plus Gaussian noise of variance b, by inversion):
// the first kFlatsGradient columns a gradient from 0 to under kFitTop, for
// the fit; right of it three flat stripes across, at `levels`.
constexpr int kFlatsSize = 1024, kFlatsGradient = 384;
std::vector<float> poisson_flats(const float a[3], const float b[3], const double levels[3])
{
    constexpr int w = kFlatsSize, h = kFlatsSize;
    std::vector<float> rgb(size_t(w) * h * 3);
    uint64_t state = 4242;
    const auto uniform = [&]
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return (double(state >> 11) + 0.5) / 9007199254740992.0;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const double t = x / double(kFlatsGradient - 1);
            const double level = x < kFlatsGradient ? 0.24 * t * t : levels[std::min(y / (h / 3), 2)];
            for (int c = 0; c < 3; ++c)
            {
                const double lambda = level / a[c];
                double u = uniform(), p = std::exp(-lambda), f = p;
                int k = 0;
                while (u > f && k < 100000)
                    ++k, p *= lambda / k, f += p;
                const double n = std::sqrt(-2 * std::log(uniform())) * std::cos(6.283185307179586 * uniform());
                rgb[(size_t(y) * w + x) * 3 + c] = float(a[c] * k + std::sqrt(b[c]) * n);
            }
        }
    return rgb;
}

// As an mRAW has it, the same stripes: camera RGB at each pixel, one
// sensor's Poisson-Gaussian noise (a, b) under each colour, floored at 0 as
// the camera holds it; through the camera's scaling (a 3-tap kernel each
// way, 1/3 to the neighbours: the noise of a pixel 0.54 like its
// neighbour's, as a Canon mRAW's luma is); to luma and colour differences,
// the colour averaged over 2 x 2 pixels and filled from the 5 x 5 blocks
// around (as the develop's guided fill does where it's flat); back to RGB,
// white balanced by `mul`.
std::vector<float> ycc_flats(float a, float b, const float mul[3], const double levels[3])
{
    constexpr int w = kFlatsSize, h = kFlatsSize;
    uint64_t state = 2424;
    const auto uniform = [&]
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return (double(state >> 11) + 0.5) / 9007199254740992.0;
    };
    std::vector<double> cam(size_t(w) * h * 3), across(cam.size()), scaled(cam.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const double t = x / double(kFlatsGradient - 1);
            const double level = x < kFlatsGradient ? 0.24 * t * t : levels[std::min(y / (h / 3), 2)];
            for (int c = 0; c < 3; ++c)
            {
                const double lambda = level / mul[c] / a;
                double u = uniform(), p = std::exp(-lambda), f = p;
                int k = 0;
                while (u > f && k < 100000)
                    ++k, p *= lambda / k, f += p;
                const double n = std::sqrt(-2 * std::log(uniform())) * std::cos(6.283185307179586 * uniform());
                cam[(size_t(y) * w + x) * 3 + c] = std::max(a * k + std::sqrt(b) * n, 0.0);
            }
        }
    constexpr double k = 1.0 / 3;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const auto at = [&](int xx) { return cam[(size_t(y) * w + std::clamp(xx, 0, w - 1)) * 3 + c]; };
                across[(size_t(y) * w + x) * 3 + c] = (k * at(x - 1) + at(x) + k * at(x + 1)) / (1 + 2 * k);
            }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const auto at = [&](int yy) { return across[(size_t(std::clamp(yy, 0, h - 1)) * w + x) * 3 + c]; };
                scaled[(size_t(y) * w + x) * 3 + c] = (k * at(y - 1) + at(y) + k * at(y + 1)) / (1 + 2 * k);
            }
    std::vector<double> luma(size_t(w) * h), cr(size_t(w / 2) * (h / 2)), cb(cr.size());
    for (size_t i = 0; i < luma.size(); ++i)
        luma[i] = 0.25 * scaled[i * 3] + 0.5 * scaled[i * 3 + 1] + 0.25 * scaled[i * 3 + 2];
    for (int by = 0; by < h / 2; ++by)
        for (int bx = 0; bx < w / 2; ++bx)
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                {
                    const size_t q = size_t(2 * by + j) * w + 2 * bx + i;
                    cr[size_t(by) * (w / 2) + bx] += (scaled[q * 3] - luma[q]) / 4, cb[size_t(by) * (w / 2) + bx] += (scaled[q * 3 + 2] - luma[q]) / 4;
                }
    std::vector<float> rgb(size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            double r = 0, bl = 0;
            for (int j = -2; j <= 2; ++j)
                for (int i = -2; i <= 2; ++i)
                {
                    const size_t q = size_t(std::clamp(y / 2 + j, 0, h / 2 - 1)) * (w / 2) + std::clamp(x / 2 + i, 0, w / 2 - 1);
                    r += cr[q] / 25, bl += cb[q] / 25;
                }
            const double l = luma[size_t(y) * w + x], out[3] = {l + r, l - 0.5 * r - 0.5 * bl, l + bl};
            for (int c = 0; c < 3; ++c)
                rgb[(size_t(y) * w + x) * 3 + c] = float(out[c] * mul[c]);
        }
    return rgb;
}

// One coefficient's share kept per colour, by the model (noise_retained)
// and by drawing `n` triples of the colours' transformed noise of
// covariance `e` through the shrinkers themselves (detail.hlsl's: soft
// luminance, the colour pair by its length), the pair's threshold `tc`
// times its noise as the filter measures it (the median absolute value of
// both, pooled, over 0.6745).
void kept_both_ways(const float e[3][3], float tau_l, float tau_c, int n, float model[3], float drawn[3])
{
    // Cholesky.
    double l[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j <= i; ++j)
        {
            double sum = e[i][j];
            for (int k = 0; k < j; ++k)
                sum -= l[i][k] * l[j][k];
            l[i][j] = i == j ? std::sqrt(std::max(sum, 0.0)) : (l[j][j] > 0 ? sum / l[j][j] : 0);
        }
    uint64_t state = 99;
    const auto normal = [&]
    {
        const auto uniform = [&]
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return (double(state >> 11) + 0.5) / 9007199254740992.0;
        };
        return std::sqrt(-2 * std::log(uniform())) * std::cos(6.283185307179586 * uniform());
    };
    std::vector<std::array<double, 3>> lcc(static_cast<size_t>(n));
    std::vector<double> pooled;
    pooled.reserve(size_t(2) * n);
    for (auto& v : lcc)
    {
        const double z[3] = {normal(), normal(), normal()};
        double c[3];
        for (int i = 0; i < 3; ++i)
            c[i] = l[i][0] * z[0] + l[i][1] * z[1] + l[i][2] * z[2];
        v = {(c[0] + 2 * c[1] + c[2]) / 4, c[0] - c[1], c[2] - c[1]};
        pooled.push_back(std::fabs(v[1])), pooled.push_back(std::fabs(v[2]));
    }
    std::nth_element(pooled.begin(), pooled.begin() + pooled.size() / 2, pooled.end());
    const double sigma_c = pooled[pooled.size() / 2] / 0.6745;
    const double var_l = (e[0][0] + 4 * e[1][1] + e[2][2] + 4 * e[0][1] + 2 * e[0][2] + 4 * e[1][2]) / 16;
    const double tl = tau_l * std::sqrt(var_l), tc = tau_c * sigma_c;
    double before[3] = {}, after[3] = {};
    for (const auto& v : lcc)
    {
        const double sl = v[0] > tl ? v[0] - tl : v[0] < -tl ? v[0] + tl : 0;
        const double m = std::hypot(v[1], v[2]), k = m > tc ? (m - tc) / m : 0;
        const double in[3] = {v[0] + (3 * v[1] - v[2]) / 4, v[0] - (v[1] + v[2]) / 4, v[0] + (3 * v[2] - v[1]) / 4};
        const double cr = v[1] * k, cb = v[2] * k;
        const double out[3] = {sl + (3 * cr - cb) / 4, sl - (cr + cb) / 4, sl + (3 * cb - cr) / 4};
        for (int c = 0; c < 3; ++c)
            before[c] += in[c] * in[c], after[c] += out[c] * out[c];
    }
    for (int c = 0; c < 3; ++c)
        drawn[c] = float(after[c] / before[c]);
    // The model, one coefficient: L, Cr, Cb's covariance, its retained.
    static constexpr float to[3][3] = {{0.25f, 0.5f, 0.25f}, {1, -1, 0}, {0, -1, 1}};
    static constexpr float back[3][3] = {{1, 0.75f, -0.25f}, {1, -0.25f, -0.25f}, {1, -0.25f, 0.75f}};
    std::array<std::array<float, 3>, 3> sm{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                for (int q = 0; q < 3; ++q)
                    sm[i][j] += to[i][k] * e[k][q] * to[j][q];
    const auto r = noise_retained(sm, float(tl), float(tc));
    for (int c = 0; c < 3; ++c)
    {
        float b = 0, a = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                b += back[c][i] * sm[i][j] * back[c][j], a += back[c][i] * r[i][j] * back[c][j];
        model[c] = a / b;
    }
}

}  // namespace

NoiseModel shaping_case(bool shifted)
{
    NoiseModel m;
    m.fitted = true, m.fitted_colours = 7;
    for (int c = 0; c < 3; ++c)
        m.a[c] = 1e-4f, m.b[c] = 1e-6f, m.offset[c] = 0.002f;
    tabulate_noise(m);
    m.spatial = true;
    for (int c = 0; c < 3; ++c)
        m.rho[c][0] = 1;
    if (shifted)
        for (int i = 0; i < NoiseModel::kSpatialLags; ++i)
        {
            int dx, dy;
            spatial_lag(i, dx, dy);
            if (dx == 1 && dy == 0)
                m.rho[spatial_series(0, 1)][i] = 1;
        }
    return m;
}

void sharpened_exactly(bool shifted, float amount, float sigma, double out[3])
{
    // Each colour plus amount times luminance less its blur, as weights on
    // the sources (Z_0 at each offset, Z_1, Z_2), each of variance 1.
    const int half = int(std::ceil(3 * sigma));
    std::vector<double> g(size_t(2 * half + 1));
    double total = 0;
    for (int k = -half; k <= half; ++k)
        total += g[size_t(k + half)] = std::exp(-0.5 * k * k / (sigma * sigma));
    for (double& v : g)
        v /= total;
    static constexpr double l[3] = {0.25, 0.5, 0.25};
    for (int c = 0; c < 3; ++c)
    {
        std::map<std::array<int, 3>, double> weights;  // source, dx, dy
        const auto source = [&](int k, int dx, int dy, double w)
        {
            const bool green_of_red = shifted && k == 1;
            weights[{green_of_red ? 0 : k, dx + (green_of_red ? 1 : 0), dy}] += w;
        };
        for (int k = 0; k < 3; ++k)
        {
            source(k, 0, 0, amount * l[k]);
            for (int dy = -half; dy <= half; ++dy)
                for (int dx = -half; dx <= half; ++dx)
                    source(k, dx, dy, -amount * l[k] * g[size_t(dx + half)] * g[size_t(dy + half)]);
        }
        source(c, 0, 0, 1);
        out[c] = 0;
        for (const auto& [at, w] : weights)
            out[c] += w * w;
    }
}

NoiseModel quiet_axis_case()
{
    NoiseModel m;
    m.fitted = m.spatial = true;
    m.fitted_colours = 7;
    for (int c = 0; c < 3; ++c)
    {
        m.a[c] = c == 2 ? 1e-7f : .001f;
        m.offset[c] = 3 * .001f / 8;
        m.rho[c][0] = 1;
    }
    tabulate_noise(m);
    return m;
}

int check_detail()
{
    int failed = 0;
    const auto report = [&](const char* what, double got, double want, double tolerance)
    {
        const bool ok = std::fabs(got - want) <= tolerance;
        std::printf("%s: %s: %.8g, want %.8g (within %.3g)\n", ok ? "ok" : "FAILED", what, got, want, tolerance);
        failed += !ok;
    };
    const float ga[3] = {0.001f, 0.001f, 0.001f}, gb[3] = {1e-5f, 1e-5f, 1e-5f};
    const NoiseModel m = given_model(ga, gb);
    const double c = m.offset[1];
    // Nothing taken out: f's own inverse, exactly what was encoded.
    for (const float y : {-0.002f, 0.0f, 0.0005f, 0.01f, 0.2f})
    {
        char what[96];
        std::snprintf(what, sizeof what, "nothing smoothed, level %g decodes as itself", y);
        report(what, noise_decode(m, float(forward_d(y, c)), 1, 1), y, 1e-6);
    }
    // All of it: the average of the original noise through the transform, back to its level.
    for (const float y : {0.0001f, 0.001f, 0.01f, 0.1f})
    {
        char what[96];
        std::snprintf(what, sizeof what, "all smoothed, E[f(X) | %g] decodes as it", y);
        report(what, noise_decode(m, float(brute_expected(y, c, ga[1], gb[1])), 1, 0), y, 2e-3 * std::sqrt(ga[1] * y + gb[1]));
    }
    // Each colour by its own law: red with twice green's a and four times its b, through green's curve.
    {
        const float a[3] = {0.002f, 0.001f, 0.001f}, b[3] = {4e-5f, 1e-5f, 1e-5f};
        const NoiseModel own = given_model(a, b);
        const double oc = own.offset[1];
        report("red by its own law, all smoothed, level 0.01", noise_decode(own, float(brute_expected(0.01, oc, a[0], b[0])), 0, 0), 0.01, 1e-5);
        report("green by its own law, all smoothed, level 0.01", noise_decode(own, float(brute_expected(0.01, oc, a[1], b[1])), 1, 0), 0.01, 1e-5);
    }
    // The pure-Poisson limit: 0.05 counts expected.
    {
        const float a[3] = {0.002f, 0.002f, 0.002f}, b[3] = {0, 0, 0};
        const NoiseModel poisson = given_model(a, b);
        report("pure Poisson, a 0.002, level 0.0001, all smoothed",
               noise_decode(poisson, float(brute_expected(0.0001, poisson.offset[1], 0.002, 0)), 1, 0), 0.0001, 2e-6);
    }
    // Linear RGB, its noise each colour's own (no demosaic: a pixel's is
    // the fit's measure): fitted, then the level 0.01 all smoothed.
    const std::vector<float> rgb = gradient(0.001f, 1e-5f);
    {
        NoiseModel fitted = fit_noise(rgb.data(), 1024, 512);
        if (!fitted.fitted)
        {
            std::printf("FAILED: linear RGB: no fit\n");
            ++failed;
        }
        else
        {
            tabulate_noise(fitted);
            std::printf("     linear RGB fitted: a %.4g %.4g %.4g, b %.4g %.4g %.4g (made with a 0.001, b 1e-05)\n", fitted.a[0], fitted.a[1], fitted.a[2],
                        fitted.b[0], fitted.b[1], fitted.b[2]);
            report("linear RGB, fitted, level 0.01 all smoothed", noise_decode(fitted, float(brute_expected(0.01, fitted.offset[1], 0.001, 1e-5)), 1, 0),
                   0.01, 5e-5);
        }
    }
    // Continuous in the sliders: Luminance 0 and 0.000001 alike (Color 25,
    // no sharpening), the shadows' mean (the first 64 columns).
    {
        Detail d;
        d.colour = 25;
        double means[2] = {};
        for (int i = 0; i < 2; ++i)
        {
            d.luminance = i ? 0.000001 : 0;
            DetailFilter f(d);
            f.model = true;
            std::vector<float> out(rgb.size());
            apply_detail(f, rgb.data(), 1024, 512, out.data());
            double sum = 0;
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 64; ++x)
                    for (int k = 0; k < 3; ++k)
                        sum += out[(size_t(y) * 1024 + x) * 3 + k];
            means[i] = sum / (512.0 * 64 * 3);
        }
        report("shadow mean, Luminance 0.000001 against 0", means[1], means[0], 1e-7);
    }
    // The shrinkage's model against the shrinkers themselves, one
    // coefficient, 160000 draws: unequal and correlated colours, the
    // thresholds from none to all of it. Each shrinker's own moments are
    // exact (to the draws and the table's steps, 0.005); luminance's covariance with the colour
    // pair, both shrunk, is Stein's lemma's first order in it, which with
    // the thresholds between (0.5 to 1.5 sigma) is off by up to 0.021 of
    // the share kept: within 0.025.
    {
        const float laws[4][3][3] = {{{4, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                                     {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                                     {{4, 1.2f, 1.2f}, {1.2f, 1, 0.6f}, {1.2f, 0.6f, 1}},
                                     {{1, 0.95f, 0.95f}, {0.95f, 1, 0.95f}, {0.95f, 0.95f, 1}}};
        const char* names[4] = {"variances 4 1 1", "variances 1 1 1", "4 1 1 correlated 0.6", "equal, correlated 0.95"};
        for (int law = 0; law < 4; ++law)
        {
            double worst = 0;
            float worst_l = 0, worst_c = 0;
            for (const float tau_l : {0.0f, 0.5f, 1.5f, 1000.0f})
                for (const float tau_c : {0.0f, 0.5f, 1.0f, 2.0f})
                {
                    float model[3], drawn[3];
                    kept_both_ways(laws[law], tau_l, tau_c, 160000, model, drawn);
                    for (int c = 0; c < 3; ++c)
                        if (std::fabs(double(model[c]) - drawn[c]) > worst)
                            worst = std::fabs(double(model[c]) - drawn[c]), worst_l = tau_l, worst_c = tau_c;
                    if (law == 0 && tau_l > 100 && tau_c == 0)
                        for (int c = 0; c < 3; ++c)
                        {
                            static const double analytic[3] = {0.640625, 0.5625, 1.0625};
                            char what[96];
                            std::snprintf(what, sizeof what, "variances 4 1 1, all of luminance taken, colour left: colour %d kept", c);
                            report(what, model[c], analytic[c], 1e-4);
                        }
                    if (law == 0 && tau_l == 0 && tau_c == 1)
                        std::printf("     %s, luminance left, colour at 1: model %.4f %.4f %.4f, drawn %.4f %.4f %.4f\n", names[law], model[0], model[1],
                                    model[2], drawn[0], drawn[1], drawn[2]);
                }
            char what[128];
            std::snprintf(what, sizeof what, "shrinkage model against the shrinkers, %s: worst share kept (at tau L %g, C %g)", names[law], worst_l, worst_c);
            report(what, worst, 0, 0.025);
        }
    }
    // The decode as each pixel has it, a lookup (noise_decode_table()),
    // against the model at the pixel's own levels (noise_kept() and
    // noise_decode()), in each colour's noise there: fitted from flats
    // under unequal noise, at strengths from colour alone to both at 100.
    // Grey, densely (between the table's entries): its steps, to 0.00003
    // sigma. Coloured (each colour from below black to near white, apart),
    // where the table's neutral pixel isn't the pixel's: to 0.0009 sigma
    // for colours within 4 times each other, 0.0035 for any (a deep red
    // against black: the colours' shares then differ most).
    {
        const float laws[2][2][3] = {{{0.002f, 0.001f, 0.001f}, {4e-5f, 1e-5f, 1e-5f}}, {{0.001f, 0.001f, 0.0015f}, {1e-5f, 1e-5f, 2.25e-5f}}};
        const double levels[3] = {0.0003, 0.001, 0.01};
        const float grid[] = {-0.002f, -0.0005f, 0, 1e-4f, 3e-4f, 1e-3f, 3e-3f, 0.01f, 0.03f, 0.1f, 0.24f};
        double worst[3] = {};
        for (const auto& law : laws)
        {
            const std::vector<float> pg = poisson_flats(law[0], law[1], levels);
            NoiseModel fitted = fit_noise(pg.data(), kFlatsSize, kFlatsSize);
            if (!fitted.fitted)
            {
                std::printf("FAILED: flats: no fit\n");
                ++failed;
                continue;
            }
            tabulate_noise(fitted);
            float noise_l, noise_c;
            measure(fitted, pg.data(), kFlatsSize, kFlatsSize, noise_l, noise_c);
            for (const auto& strength : {std::array<double, 2>{0, 25}, {20, 25}, {60, 25}, {60, 75}, {100, 100}})
            {
                Detail d;
                d.luminance = strength[0], d.colour = strength[1], d.sharpness = 0;
                const DetailFilter f(d);
                float tl[5], tc[5], shift[3][NoiseModel::kInverse];
                for (int j = 0; j < 5; ++j)
                    tl[j] = f.luminance[j] * noise_l, tc[j] = f.colour[j] * noise_c;
                const NoiseShaping shaping = noise_shaping(&fitted, &f);
                noise_decode_table(fitted, tl, tc, shaping, shift);
                const auto off = [&](const float y[3], int c)
                {
                    float kept[3];
                    noise_kept(fitted, y, tl, tc, shaping, kept);
                    const float e = forward(y[c], fitted.offset[c]);
                    return std::fabs(double(noise_decode_by(fitted, shift, e, c)) - noise_decode(fitted, e, c, kept[c])) /
                           std::sqrt(fitted.law_a[c] * std::max(y[c], 0.0f) + fitted.law_b[c]);
                };
                for (int i = 0; i <= 2000; ++i)
                {
                    const float u = i / 2000.0f, v = u < 0.05f ? -6 * std::sqrt(fitted.law_b[1]) * (1 - u / 0.05f) : 0.25f * std::pow((u - 0.05f) / 0.95f, 4.0f);
                    const float y[3] = {v, v, v};
                    for (int c = 0; c < 3; ++c)
                        worst[0] = std::max(worst[0], off(y, c));
                }
                for (const float r : grid)
                    for (const float g : grid)
                        for (const float b : grid)
                        {
                            const float y[3] = {r, g, b};
                            const float lo = std::min({r, g, b}), hi = std::max({r, g, b});
                            const bool near = lo > 0 && hi <= 4 * lo;
                            for (int c = 0; c < 3; ++c)
                            {
                                const double e = off(y, c);
                                worst[2] = std::max(worst[2], e);
                                if (near)
                                    worst[1] = std::max(worst[1], e);
                            }
                        }
            }
        }
        report("decode table against the model, grey between its entries, worst in sigma", worst[0], 0, 0.0001);
        report("decode table against the model, colours within 4 times each other, worst in sigma", worst[1], 0, 0.0012);
        report("decode table against the model, any colours, worst in sigma", worst[2], 0, 0.004);
    }
    // Flat grey under unequal noise (each colour its own Poisson-Gaussian
    // law, red's or blue's the noisier, as a white balance makes them)
    // through the CPU's filter, fitted from the image, at intermediate
    // strengths: what it moves each colour's mean by (output less input,
    // over each stripe less the filter's reach), in that colour's noise (a
    // pixel's standard deviation there), and red and blue against green, in
    // green's. The decode's model is of the noise's second moments, so its
    // residual is what that leaves out: the scales' coefficients taken as
    // independent, the edge support and the colour guide left out, and the
    // noise's skew (a Poisson count of a few photons is still lopsided after
    // the transform, and the shrinkers move its mean), which is most at a
    // few photons a pixel. Measured over 8 draws: within 0.003 sigma a
    // colour and 0.0045 between them at 0.0003 and 0.001 (well under a
    // photon a pixel to one; the readout's noise most of it), to 0.019 and
    // 0.031 at 0.01 (5 photons a pixel in red). The tolerances are those
    // with one draw's spread on top.
    {
        struct Law
        {
            const char* name;
            float a[3], b[3];
        };
        const Law laws[2] = {{"red noisier", {0.002f, 0.001f, 0.001f}, {4e-5f, 1e-5f, 1e-5f}},
                             {"blue noisier", {0.001f, 0.001f, 0.0015f}, {1e-5f, 1e-5f, 2.25e-5f}}};
        const double levels[3] = {0.0003, 0.001, 0.01};
        const double own_tolerance[3] = {0.005, 0.005, 0.025}, between_tolerance[3] = {0.006, 0.006, 0.04};
        constexpr int w = kFlatsSize, h = kFlatsSize, reach = 72;
        for (const Law& law : laws)
        {
            const std::vector<float> pg = poisson_flats(law.a, law.b, levels);
            std::vector<float> out(pg.size());
            for (const auto& strength : {std::array<double, 2>{0, 25}, {20, 25}, {60, 25}, {60, 75}})
            {
                Detail d;
                d.luminance = strength[0], d.colour = strength[1], d.sharpness = 0;
                DetailFilter f(d);
                f.model = true;
                apply_detail(f, pg.data(), w, h, out.data());
                for (int s = 0; s < 3; ++s)
                {
                    double moved[3] = {}, sigma[3];
                    long n = 0;
                    for (int y = s * (h / 3) + reach; y < (s + 1) * (h / 3) - reach; ++y)
                        for (int x = kFlatsGradient + reach; x < w - reach; ++x, ++n)
                            for (int c = 0; c < 3; ++c)
                            {
                                const size_t i = (size_t(y) * w + x) * 3 + c;
                                moved[c] += double(out[i]) - pg[i];
                            }
                    for (int c = 0; c < 3; ++c)
                        moved[c] /= double(n), sigma[c] = std::sqrt(law.a[c] * levels[s] + law.b[c]);
                    for (int c = 0; c < 3; ++c)
                    {
                        char what[128];
                        std::snprintf(what, sizeof what, "flat, %s, L %.0f C %.0f, level %g: colour %d moved, in its sigma", law.name, strength[0], strength[1],
                                      levels[s], c);
                        report(what, moved[c] / sigma[c], 0, own_tolerance[s]);
                    }
                    for (const int c : {0, 2})
                    {
                        char what[128];
                        std::snprintf(what, sizeof what, "flat, %s, L %.0f C %.0f, level %g: colour %d against green, in green's sigma", law.name, strength[0],
                                      strength[1], levels[s], c);
                        report(what, (moved[c] - moved[1]) / sigma[1], 0, between_tolerance[s]);
                    }
                }
            }
        }
    }
    // Flats as an mRAW has them (ycc_flats(): neighbours alike, the colour
    // shared over blocks, the camera's floor at 0), at the viewer's default
    // (Color 25) and at 60, both sharpened at its default (40): what each
    // colour's mean moves by, in its noise, and red and blue against green
    // (relative). Without the noise's measured correlation, or without the
    // sharpening in the decode, green rises by 0.05 of its noise at Color
    // 25 and red and blue fall by 0.03 at 60, red against green by up to
    // 1.2%. With them, within 0.004 of the ratio, and within 0.03 of each
    // colour's noise, except where the camera's floor bites (a photon a
    // pixel: 0.0005 here, all three colours alike, to 0.035; left out of the
    // model, as it's smeared by the scaling before the data has it).
    {
        const float mul[3] = {2.0f, 1.0f, 1.6f};
        const double levels[3] = {0.0005, 0.0015, 0.005};
        const std::vector<float> ycc = ycc_flats(4e-4f, 4e-7f, mul, levels);
        std::vector<float> out(ycc.size());
        constexpr int w = kFlatsSize, h = kFlatsSize, reach = 72;
        for (const auto& strength : {std::array<double, 2>{0, 25}, {60, 60}})
        {
            Detail d;
            d.luminance = strength[0], d.colour = strength[1], d.sharpness = 40;
            DetailFilter f(d);
            f.model = true;
            apply_detail(f, ycc.data(), w, h, out.data());
            for (int s = 0; s < 3; ++s)
            {
                double in[3] = {}, moved[3] = {}, q[3] = {};
                long n = 0;
                for (int y = s * (h / 3) + reach; y < (s + 1) * (h / 3) - reach; ++y)
                    for (int x = kFlatsGradient + reach; x < w - reach; ++x, ++n)
                        for (int c = 0; c < 3; ++c)
                        {
                            const size_t i = (size_t(y) * w + x) * 3 + c;
                            in[c] += ycc[i], moved[c] += double(out[i]) - ycc[i], q[c] += double(ycc[i]) * ycc[i];
                        }
                for (int c = 0; c < 3; ++c)
                {
                    const double sigma = std::sqrt(q[c] / n - (in[c] / n) * (in[c] / n));
                    char what[128];
                    std::snprintf(what, sizeof what, "mRAW-like flat, L %.0f C %.0f sharpened, level %g: colour %d moved, in its sigma", strength[0], strength[1],
                                  levels[s], c);
                    report(what, moved[c] / n / sigma, 0, s == 0 ? 0.045 : 0.03);
                }
                for (const int c : {0, 2})
                {
                    char what[128];
                    std::snprintf(what, sizeof what, "mRAW-like flat, L %.0f C %.0f sharpened, level %g: colour %d over green, moved (relative)", strength[0],
                                  strength[1], levels[s], c);
                    report(what, ((in[c] + moved[c]) / in[c]) / ((in[1] + moved[1]) / in[1]) - 1, 0, 0.004);
                }
            }
        }
    }
    // The sharpening of noise nothing is taken out of, against the kernel's
    // exact propagation: independent colours, and green red's a pixel on
    // (R(x) = Z(x), G(x) = Z(x + 1), B its own), whose colours don't
    // correlate at a pixel but do a lag on. The viewer's sharpening (0.6,
    // sigma 1, halo 0.00825) of noise far below the halo, so linear.
    for (const bool shifted : {false, true})
    {
        NoiseModel m = shaping_case(shifted);
        Detail d;
        d.sharpness = 40;
        DetailFilter f(d);
        f.sharpen = true, f.amount = 0.6f, f.sigma = 1, f.halo = 0.00825f;
        const NoiseShaping shaping = noise_shaping(&m, &f);
        std::array<std::array<float, 3>, 3> e{};
        for (int c = 0; c < 3; ++c)
            e[c][c] = 1e-12f;
        const float none[5] = {};
        float kept[3];
        noise_kept_of(e, none, none, shaping, kept);
        double exact[3];
        sharpened_exactly(shifted, f.amount, f.sigma, exact);
        for (int c = 0; c < 3; ++c)
        {
            char what[128];
            std::snprintf(what, sizeof what, "sharpened, nothing taken out, %s: colour %d's variance against the kernel's", shifted ? "green red's a pixel on" : "independent", c);
            report(what, kept[c] - exact[c], 0, 2e-4);
        }
    }
    // The production active-endpoint case: blue has 1/10000 the shot
    // noise. Forming B^1/2 S B^1/2 in float lost that axis and decoded it
    // 0.28 sigma away from a double-precision transport control.
    {
        const NoiseModel m = quiet_axis_case();
        const NoiseShaping shaping = noise_shaping(&m, nullptr);
        Detail d;
        d.luminance = d.colour = 1;
        const DetailFilter f(d);
        float tl[5], tc[5], kept[3], shift[3][NoiseModel::kInverse];
        for (int j = 0; j < 5; ++j)
            tl[j] = f.luminance[j] * .01f, tc[j] = f.colour[j] * .02f;
        const float y[3] = {.0003f, .0003f, .0003f};
        noise_kept(m, y, tl, tc, shaping, kept);
        noise_decode_table(m, tl, tc, shaping, shift);
        const float decoded = noise_decode_by(m, shift, std::sqrt(y[2] + m.offset[2]), 2);
        // Independent double M control, same production float shrinker and
        // inverse table, in units of this quiet colour's own noise.
        const double control = .000299325009109;
        const double sigma = std::sqrt(double(m.law_a[2]) * y[2] + m.law_b[2]);
        report("quiet blue active endpoint, decode against double transport in sigma", (decoded - control) / sigma, 0, .005);
        report("quiet blue active endpoint, variance against double transport", kept[2], 60.8873596, .05);
    }
    // What's kept is a variance, never below nothing, for any field: noise
    // made by short filters (along rows) of two independent white sources,
    // its lag covariances exact. Codex's field (one that came out at -0.126
    // in blue at L 0 C 25), then 400 random ones, each at Luminance and Color
    // 0 to 100, unsharpened and at the viewer's sharpening.
    {
        const auto field = [](const float (&k)[3][2][5], int taps, NoiseModel& m, std::array<std::array<float, 3>, 3>& e)
        {
            m = NoiseModel{};
            m.spatial = true;
            for (int c = 0; c < 3; ++c)
                for (int d = 0; d < 3; ++d)
                    for (int l = 0; l < NoiseModel::kSpatialLags; ++l)
                    {
                        int x, y;
                        spatial_lag(l, x, y);
                        float v = 0;
                        if (y == 0)
                            for (int z = 0; z < 2; ++z)
                                for (int j = 0; j < taps; ++j)
                                    if (j - x >= 0 && j - x < taps)
                                        v += k[c][z][j] * k[d][z][j - x];
                        m.rho[spatial_series(c, d)][l] = v;
                        if (l == 0)
                            e[c][d] = v;
                    }
        };
        const auto least_kept = [&](const NoiseModel& m, const std::array<std::array<float, 3>, 3>& e)
        {
            float least = 1e9f;
            Detail sharp;
            sharp.sharpness = 40;
            const DetailFilter sharpening(sharp);
            for (const DetailFilter* s : {(const DetailFilter*)nullptr, &sharpening})
            {
                const NoiseShaping shaping = noise_shaping(&m, s);
                for (const int L : {0, 20, 60, 100})
                    for (const int C : {0, 25, 60, 100})
                    {
                        Detail d;
                        d.luminance = L, d.colour = C;
                        const DetailFilter f(d);
                        float tl[5], tc[5], kept[3];
                        for (int j = 0; j < 5; ++j)
                            tl[j] = f.luminance[j] * 0.5f, tc[j] = f.colour[j] * 1.2f;
                        noise_kept_of(e, tl, tc, shaping, kept);
                        least = std::min({least, kept[0], kept[1], kept[2]});
                    }
            }
            return least;
        };
        static constexpr float codex[3][2][5] = {
            {{-0.292479396f, -0.290360332f, 0.0921152905f, -0.437809229f, -0.0207346398f}, {0.425677329f, 0.231504858f, 0.503271997f, -0.309252113f, 0.214101285f}},
            {{-0.190319031f, 0.126380399f, -0.700101793f, 0.256545156f, -0.146933705f}, {0.22335121f, -0.30618006f, -0.102574728f, -0.464574754f, -0.0166126285f}},
            {{-0.25286907f, 0.21024619f, -0.156126216f, 0.30961737f, 0.091876924f}, {0.387454689f, -0.0569246933f, 0.019110769f, -0.375257909f, 0.68456471f}}};
        NoiseModel m;
        std::array<std::array<float, 3>, 3> e{};
        field(codex, 5, m, e);
        float least = least_kept(m, e);
        char what[128];
        std::snprintf(what, sizeof what, "filtered field (Codex's): least share kept %.4g, below 0 by", least);
        report(what, std::min(least, 0.0f), 0, 0);
        std::mt19937 random(827);
        std::normal_distribution<float> normal;
        least = 1e9f;
        for (int t = 0; t < 400; ++t)
        {
            const int taps = 2 + t % 4;
            float k[3][2][5] = {};
            for (int c = 0; c < 3; ++c)
            {
                double q = 0;
                for (int z = 0; z < 2; ++z)
                    for (int x = 0; x < taps; ++x)
                        k[c][z][x] = normal(random), q += double(k[c][z][x]) * k[c][z][x];
                for (int z = 0; z < 2; ++z)
                    for (int x = 0; x < taps; ++x)
                        k[c][z][x] /= float(std::sqrt(q));
            }
            field(k, taps, m, e);
            least = std::min(least, least_kept(m, e));
        }
        std::snprintf(what, sizeof what, "400 random filtered fields: least share kept %.4g, below 0 by", least);
        report(what, std::min(least, 0.0f), 0, 0);
    }
    std::printf("%s\n", failed ? "check-detail: FAILED" : "check-detail: ok");
    return failed ? 1 : 0;
}

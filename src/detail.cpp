#include "detail.h"

#include "parallel.h"

#include <algorithm>
#include <cmath>
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

}  // namespace

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

// shaders/detail.hlsli's encode: each channel's square root, then
// luminance and the colour differences.
void encode(const float* rgb, float out[3])
{
    float e[3];
    for (int i = 0; i < 3; ++i)
        e[i] = std::sqrt(std::max(rgb[i], 0.0f) + DetailFilter::kFloor);
    out[0] = (e[0] + 2 * e[1] + e[2]) / 4;
    out[1] = e[0] - e[1];
    out[2] = e[2] - e[1];
}

void decode(const float in[3], float rgb[3])
{
    const float g = in[0] - (in[1] + in[2]) / 4;
    const float e[3] = {in[1] + g, g, in[2] + g};
    for (int i = 0; i < 3; ++i)
        rgb[i] = std::max(e[i], 0.0f) * std::max(e[i], 0.0f) - DetailFilter::kFloor;
}

// The finest scale's noise in luminance and colour: the median absolute
// detail on a lattice across the whole image, as a standard deviation.
void measure(const float* rgb, int width, int height, float& luminance, float& colour)
{
    std::vector<float> l, c;
    const auto encoded = [&](int x, int y, float e[3])
    {
        x = std::clamp(x, 0, width - 1), y = std::clamp(y, 0, height - 1);
        encode(rgb + (size_t(y) * width + x) * 3, e);
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
    if (f.denoise)
        measure(rgb, width, height, noise_l, noise_c);
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
        cur.resize(iw, ih);
        for (int y = 0; y < ih; ++y)
            for (int x = 0; x < iw; ++x)
            {
                float e[3];
                encode(rgb + (size_t(iy + y) * width + ix + x) * 3, e);
                for (int k = 0; k < 3; ++k)
                    cur.c[k][size_t(y) * iw + x] = e[k];
            }
        if (f.denoise)
        {
            next.resize(iw, ih), smoothed.resize(iw, ih), sum.resize(iw, ih);
            for (int j = 0, step = 1; j < DetailFilter::kLevels; ++j, step *= 2)
            {
                for (int k = 0; k < 3; ++k)
                    for (int y = 0; y < ih; ++y)
                        for (int x = 0; x < iw; ++x)
                        {
                            float s = 0;
                            for (int u = -2; u <= 2; ++u)
                                s += kB3[u + 2] * cur.at(k, x + u * step, y);
                            smoothed.c[k][size_t(y) * iw + x] = s;
                        }
                for (int k = 0; k < 3; ++k)
                    for (int y = 0; y < ih; ++y)
                        for (int x = 0; x < iw; ++x)
                        {
                            float s = 0;
                            for (int v = -2; v <= 2; ++v)
                                s += kB3[v + 2] * smoothed.at(k, x, y + v * step);
                            next.c[k][size_t(y) * iw + x] = s;
                        }
                for (size_t i = 0; i < size_t(iw) * ih; ++i)
                {
                    float l = cur.c[0][i] - next.c[0][i], a = cur.c[1][i] - next.c[1][i], b = cur.c[2][i] - next.c[2][i];
                    l = shrink(l, f.luminance[j] * noise_l);
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
                decode(e, out + (size_t(iy + y) * width + ix + x) * 3);
            }
    });
}

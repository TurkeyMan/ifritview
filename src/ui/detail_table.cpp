#include "detail_table.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
// noise.hlsl/detail.hlsli's word layout. Keep the end checked against the
// allocation shared by the CPU and GPU; no fit histogram is needed here.
constexpr size_t kSigma = 2 * 1024;
constexpr size_t kModel = kSigma + 2, kLaw = kModel + 17;
constexpr size_t kInverse = kModel + 32 + 3 * 64 * 128;
constexpr size_t kRho = kInverse + 3 * NoiseModel::kInverse + 256 + 1
    + NoiseModel::kSpatialSeries * NoiseModel::kSpatialLags;
constexpr size_t kPixel = kRho + NoiseModel::kSpatialSeries * NoiseModel::kSpatialLags;
static_assert(kPixel + 4 == DetailFilter::kNoiseWords);

bool finite(const float* p, size_t n)
{
    return std::all_of(p, p + n, [](float x) { return std::isfinite(x); });
}
}

std::optional<DetailNoise> read_detail_noise(std::span<const std::byte> words)
{
    if (words.size() < size_t(DetailFilter::kNoiseWords) * sizeof(float))
        return std::nullopt;
    const auto copy = [&](size_t word, float* to, size_t count)
    {
        std::memcpy(to, words.data() + word * sizeof(float), count * sizeof(float));
    };
    const auto integer = [&](size_t word)
    {
        uint32_t x;
        std::memcpy(&x, words.data() + word * sizeof(float), sizeof x);
        return x;
    };
    const uint32_t fitted = integer(kModel + 9);
    if (fitted & ~7u)
        return std::nullopt;
    DetailNoise out;
    copy(kSigma, &out.sigma_luminance, 1);
    copy(kSigma + 1, &out.sigma_colour, 1);
    if (!std::isfinite(out.sigma_luminance) || out.sigma_luminance < 0
        || !std::isfinite(out.sigma_colour) || out.sigma_colour < 0)
        return std::nullopt;
    auto& m = out.model;
    m.fitted_colours = int(fitted);
    m.fitted = (fitted & 2) != 0;
    if (!m.fitted)
        return out;  // DETAIL_TABLE leaves every shift at zero in this case.
    copy(kModel, m.offset, 3);
    copy(kModel + 3, m.a, 3);
    copy(kModel + 6, m.b, 3);
    copy(kModel + 11, m.from, 3);
    copy(kModel + 14, m.to, 3);
    copy(kLaw, m.law_a, 3);
    copy(kLaw + 3, m.law_b, 3);
    copy(kLaw + 6, m.correlation_light, 3);
    copy(kLaw + 9, m.correlation_readout, 3);
    for (int c = 0; c < 3; ++c)
    {
        copy(kInverse + c * NoiseModel::kInverse, m.inverse[c], NoiseModel::kInverse);
        if (!finite(m.inverse[c], NoiseModel::kInverse) || !(m.to[c] > m.from[c])
            || !(m.offset[c] > 0) || m.law_a[c] < 0 || m.law_b[c] < 0)
            return std::nullopt;
    }
    m.spatial = integer(kPixel + 3) != 0;
    copy(kPixel, m.pixel, 3);
    for (int s = 0; s < NoiseModel::kSpatialSeries; ++s)
    {
        copy(kRho + s * NoiseModel::kSpatialLags, m.rho[s], NoiseModel::kSpatialLags);
        if (m.spatial && !finite(m.rho[s], NoiseModel::kSpatialLags))
            return std::nullopt;
    }
    for (const float* p : {m.offset, m.a, m.b, m.from, m.to, m.law_a, m.law_b,
                          m.correlation_light, m.correlation_readout, m.pixel})
        if (!finite(p, 3))
            return std::nullopt;
    return out;
}

std::vector<float> make_detail_table(const DetailNoise& noise, const Detail& settings)
{
    std::vector<float> rgba(size_t(NoiseModel::kInverse) * 4, 0);
    if (!noise.model.fitted || !(noise.model.fitted_colours & 2))
        return rgba;
    const DetailFilter filter(settings);
    const NoiseShaping shaping = noise_shaping(&noise.model, &filter);
    float tl[5], tc[5], shift[3][NoiseModel::kInverse];
    for (int j = 0; j < 5; ++j)
        tl[j] = filter.luminance[j] * noise.sigma_luminance,
        tc[j] = filter.colour[j] * noise.sigma_colour;
    noise_decode_table(noise.model, tl, tc, shaping, shift);
    for (int i = 0; i < NoiseModel::kInverse; ++i)
        for (int c = 0; c < 3; ++c)
            rgba[size_t(i) * 4 + c] = shift[c][i];
    return rgba;
}

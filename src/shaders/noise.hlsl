// The image's noise for noise reduction (detail.h), measured from level 0
// itself (detail.cpp's measure(), the CPU's), in seven dispatches:
//  0. a raw's noise model: blocks of kFitBlock x kFitBlock pixels on a
//     lattice (kFitLattice) across the image, each colour's mean and variance (of what's left after its four
//     neighbours' mean: smooth shading barely counts), counted by mean and
//     variance;
//  1. one group: the model fitted to those (fit_group()), variance = a x + b at
//     level x, b the readout's noise where the sensor's masked border
//     measured it, and the encoding green's gives (detail.hlsli); without
//     a fit, the encoding as it was (kDetailFloor);
//  2. the finest wavelet scale's absolute detail in that encoding, on a
//     lattice, counted into a histogram (luminance, colour);
//  3. one thread: each one's median as a standard deviation;
//  4. the noise's spatial correlation (detail.cpp's measure_spatial()):
//     blocks on a lattice, each one's luminance variance in the encoding
//     counted;
//  5. a group a block, the half that varies least (flat): each colour
//     against each at each lag within the reach, summed;
//  6. one group: the correlation from those, each colour's pixel variance
//     against the fit's measure; then each colour's law and its
//     expectation inverse.
#include "detail.hlsli"
#include "clip_layout.hlsli"

cbuffer Params : register(b0)
{
    int mode;
    int2 size;       // level 0's
    int fitting;     // 1: a raw's: fit the model (else the encoding as it was)
    float4 as_shot;  // what level 0 is white balanced with (its data / these)
    int4 read;       // x: `clips` holds the readout's noise as the masked border measured it (a Bayer mosaic's); y: the law by the measured correlation (an sRAW's, an mRAW's)
    float4 light;    // each colour's: a pixel's noise against the blocks' measure through this develop, the light's (a)
    float4 readout;  // and the readout's (b) (gpu_view.cpp's develop_noise())
    float4 green_light, green_readout;  // each colour's measure against green's, the light's and the readout's: a colour with no fit of its own takes green's by it
    float4 measure[3];  // a Bayer mosaic's V: the masked border's photosite noise (R G B) to the blocks' measure, rows R G B
    float4 correlation_light;    // the colours' noise correlations after the develop, R-G R-B G-B: the light's
    float4 correlation_readout;  // and the readout's (develop_noise())
};

Texture2D<float4> source : register(t0);
RWByteAddressBuffer clips : register(u1);  // clip.hlsl's (kClipReadNoise), when read.x
RWByteAddressBuffer noise : register(u0);

float3 balanced(int2 p)
{
    return source.Load(int3(clamp(p, 0, size - 1), 0)).rgb * as_shot.rgb;
}

float3 encoded(int2 p, float3 offset)
{
    return detail_encode(balanced(p), offset);
}

void count(uint bin_base, float v)
{
    noise.InterlockedAdd((bin_base + min(uint(v / kNoiseRange * kNoiseBins), kNoiseBins - 1)) * 4, 1u);
}

float median(uint bin_base)
{
    uint total = 0;
    for (uint i = 0; i < kNoiseBins; ++i)
        total += noise.Load((bin_base + i) * 4);
    uint seen = 0;
    for (uint b = 0; b < kNoiseBins; ++b)
    {
        seen += noise.Load((bin_base + b) * 4);
        if (seen * 2 >= total)
            return (b + 0.5) * kNoiseRange / kNoiseBins;
    }
    return 0;
}

// The fit, as detail.cpp's fit_noise(), by one group of threads, one for
// each colour's level: each level's variance, the lower quartile of its
// blocks' (texture only adds to a block's; scaled to their mean as plain
// noise has it), then variance = a x + b by weighted least squares, weighed
// by how sure each level is (its blocks, relative to its variance), four
// times over, each time leaving out the levels more than half as far again
// from the line as noise would be (texture, clipping); the sums over the
// levels as the group's reductions. The readout's noise, when measured,
// fixes b (only a fitted). In level 0's units, as the blocks measure it.
static const float kFitQuartile = 0.84;  // the lower quartile of plain noise's blocks, of their mean
static const uint kFitLeast = 16;        // blocks for a level to count
static const float kFitMost = 2000;      // blocks a level is weighed by, at most
static const uint kFitThreads = 3 * kFitLevels;

// A colour's sums over its levels: S, Sx, Sy, Sxx, Sxy, levels used, the
// least and the most level used. Each thread its level's, then halved in
// place, a step at a time; the colour's total at its level 0.
groupshared float fit_sums[8][kFitThreads];

void fit_reduce(uint t)
{
    const uint k = t % kFitLevels;
    for (uint half_ = kFitLevels / 2; half_ > 0; half_ /= 2)
    {
        GroupMemoryBarrierWithGroupSync();
        if (t < kFitThreads && k < half_)
        {
            [unroll] for (uint q = 0; q < 6; ++q)
                fit_sums[q][t] += fit_sums[q][t + half_];
            fit_sums[6][t] = min(fit_sums[6][t], fit_sums[6][t + half_]);
            fit_sums[7][t] = max(fit_sums[7][t], fit_sums[7][t + half_]);
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

groupshared float fit_a[3], fit_b[3];
groupshared float fit_forward[3][kForwardSize];  // each colour's averages through the transform (mode 1)
groupshared uint fit_failed[3];

// The spatial correlation's measure (modes 4 to 6, detail.cpp's
// measure_spatial()): blocks of kSpatialBlock pixels square, one every
// kSpatialStep across and down, each with its pixels within the reach of it
// (a half plane: those below, and either side).
static const int kSpatialBlock = 16;
static const int kSpatialStep = 32;
static const int kSpatialTileWidth = kSpatialBlock + 2 * kSpatialReach;
static const uint kSpatialTile = kSpatialTileWidth * (kSpatialBlock + kSpatialReach);
groupshared float spatial_tile[3][kSpatialTile];
groupshared float spatial_sums[3];
groupshared float spatial_jobs[kSpatialSeries * kSpatialLags];
groupshared float spatial_rho[2][kSpatialSeries * kSpatialLags];  // mode 6's: as measured, and the means undone
groupshared uint spatial_flat;

// Thread `t` of the group (all of them call it): leaves fit_a, fit_b and
// fit_failed (nonzero: no fit) for each colour.
void fit_group(uint t, float3 fixed_b)
{
    const uint c = min(t / kFitLevels, 2u), k = t % kFitLevels;
    const bool mine = t < kFitThreads;
    // This level's lower quartile.
    float v = 0, n = 0;
    if (mine && fitting)
    {
        const uint base = kNoiseFit + (c * kFitLevels + k) * kFitSpreads;
        uint total = 0;
        for (uint s = 0; s < kFitSpreads; ++s)
            total += noise.Load((base + s) * 4);
        if (total >= kFitLeast)
        {
            const float want = total / 4.0;
            uint seen = 0;
            for (uint s = 0; s < kFitSpreads; ++s)
            {
                const uint here = noise.Load((base + s) * 4);
                if (seen + here >= want)
                {
                    const float at = s + (want - seen) / here;  // within the bin, linearly
                    v = exp2(at / 4 - 32) / kFitQuartile;
                    break;
                }
                seen += here;
            }
            n = min(float(total), kFitMost);
        }
    }
    if (t < 3)
        fit_failed[t] = fitting ? 0 : 1, fit_a[t] = 0, fit_b[t] = 0;
    // The largest variance: levels as 0..1 (x), variances over it (y), well
    // within a float's range.
    if (mine)
    {
        [unroll] for (uint q = 0; q < 6; ++q)
            fit_sums[q][t] = 0;
        fit_sums[6][t] = 1e30;
        fit_sums[7][t] = v;
    }
    fit_reduce(t);
    const float top = fit_sums[7][c * kFitLevels];
    if (t < 3 && fit_sums[7][t * kFitLevels] <= 0)
        fit_failed[t] = 1;
    v = top > 0 ? v / top : 0;
    const float u = (k + 0.5) / kFitLevels, x = u * u;
    const float fb = fixed_b[c] >= 0 && top > 0 ? fixed_b[c] / top : -1;
    float w = n > 0 ? n / (v * v) : 0;
    bool close = n > 0;
    for (uint iteration = 0; iteration < 4; ++iteration)
    {
        GroupMemoryBarrierWithGroupSync();
        if (mine)
        {
            fit_sums[0][t] = w, fit_sums[1][t] = w * x, fit_sums[2][t] = w * v, fit_sums[3][t] = w * x * x, fit_sums[4][t] = w * x * v;
            fit_sums[5][t] = 0, fit_sums[6][t] = 1e30, fit_sums[7][t] = 0;
        }
        fit_reduce(t);
        if (mine && k == 0)
        {
            const float S = fit_sums[0][t], Sx = fit_sums[1][t], Sy = fit_sums[2][t], Sxx = fit_sums[3][t], Sxy = fit_sums[4][t];
            float fa, fbb;
            if (fb >= 0)
                fbb = fb, fa = Sxx > 0 ? (Sxy - fb * Sx) / Sxx : 0;
            else
            {
                const float det = S * Sxx - Sx * Sx;
                fa = det > 0 ? (S * Sxy - Sx * Sy) / det : 0;
                fbb = det > 0 ? (Sxx * Sy - Sx * Sxy) / det : -1;
                if (fbb < 0)
                    fbb = 0, fa = Sxx > 0 ? Sxy / Sxx : 0;
            }
            if (S <= 0 || Sxx <= 0 || fa <= 0)
                fit_failed[c] = 1;
            fit_a[c] = fa, fit_b[c] = fbb;
        }
        GroupMemoryBarrierWithGroupSync();
        const float m = fit_a[c] * x + fit_b[c];
        close = n > 0 && v <= 1.5 * m && v * 1.5 >= m;
        w = close ? n / (m * m) : 0;
    }
    // Enough levels, far enough apart, for a line through them.
    GroupMemoryBarrierWithGroupSync();
    if (mine)
    {
        [unroll] for (uint q = 0; q < 5; ++q)
            fit_sums[q][t] = 0;
        fit_sums[5][t] = close ? 1 : 0, fit_sums[6][t] = close ? x : 1e30, fit_sums[7][t] = close ? x : 0;
    }
    fit_reduce(t);
    if (mine && k == 0)
    {
        if (fit_sums[5][t] < 4 || fit_sums[7][t] < 4 * fit_sums[6][t])
            fit_failed[c] = 1;
        fit_a[c] *= top / kFitTop, fit_b[c] *= top;
    }
    GroupMemoryBarrierWithGroupSync();
}


[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    if (mode == 0)
    {
        // Its block within the image (no return before it's done: barriers
        // further on are in flow every thread takes, as FXC requires).
        const int2 o = int2(id.xy) * kFitLattice;
        if (all(o + kFitBlock <= size))
        {
            float3 sum = 0, sq = 0;
            for (int y = 0; y < kFitBlock; ++y)
                for (int x = 0; x < kFitBlock; ++x)
                {
                    const int2 p = o + int2(x, y);
                    const float3 c = balanced(p);
                    const float3 d = c - (balanced(p + int2(1, 0)) + balanced(p - int2(1, 0)) + balanced(p + int2(0, 1)) + balanced(p - int2(0, 1))) / 4;
                    sum += c;
                    sq += d * d;
                }
            const float n = kFitBlock * kFitBlock;
            const float3 mean = sum / n, variance = sq / n / 1.25;  // 1.25: a pixel less four neighbours' mean, plain noise
            for (uint c = 0; c < 3; ++c)
            {
                const float u = sqrt(max(mean[c], 0.0) / kFitTop);
                if (u >= 1)
                    continue;
                const uint level = min(uint(u * kFitLevels), kFitLevels - 1);
                const uint spread = uint(clamp((log2(max(variance[c], 1e-12)) + 32) * 4, 0.0, kFitSpreads - 1.0));
                noise.InterlockedAdd((kNoiseFit + (c * kFitLevels + level) * kFitSpreads + spread) * 4, 1u);
            }
        }
        return;
    }
    if (mode == 1)
    {
        // One group. Each colour fitted (to compare), the encoding by
        // green's, the most photosites and most of the luminance, for all
        // three: one curve keeps noise the colours share (a demosaic's, an
        // sRAW's luma) cancelling in their differences.
        // The masked border's photosite noise, each colour's, to the blocks' measure.
        const float3 border = read.x ? asfloat(clips.Load3(kClipReadNoise * 4)) : float3(0, 0, 0);
        const float3 masked = all(border > 0) ? float3(dot(measure[0].xyz, border), dot(measure[1].xyz, border), dot(measure[2].xyz, border)) : float3(0, 0, 0);
        fit_group(index, float3(masked.r > 0 ? masked.r : -1.0, masked.g > 0 ? masked.g : -1.0, masked.b > 0 ? masked.b : -1.0));
        const float3 fa = float3(fit_a[0], fit_a[1], fit_a[2]), fb = float3(fit_b[0], fit_b[1], fit_b[2]);
        const uint fitted = (fit_failed[0] ? 0u : 1u) | (fit_failed[1] ? 0u : 2u) | (fit_failed[2] ? 0u : 4u);
        float3 offset = kDetailFloor;
        if (fitted & 2)
            offset = max(3 * fa.g / 8 + fb.g / fa.g, 1e-7);
        if (index == 0)
        {
            noise.Store3(kNoiseModel * 4, asuint(offset));
            noise.Store3((kNoiseModel + 3) * 4, asuint(fa));
            noise.Store3((kNoiseModel + 6) * 4, asuint(fb));
            noise.Store((kNoiseModel + 9) * 4, fitted);
            noise.Store((kNoiseModel + 10) * 4, (fitted & 2) && masked.g > 0 ? 1u : 0u);
        }
        return;
    }
    if (mode == 4)
    {
        // A block's luminance variance in the encoding, counted.
        const int2 o = int2(kSpatialReach, 0) + int2(id.xy) * kSpatialStep;
        if (o.x + kSpatialBlock + kSpatialReach <= size.x && o.y + kSpatialBlock + kSpatialReach <= size.y)
        {
            const float3 offset = asfloat(noise.Load3(kNoiseModel * 4));
            float s = 0, q = 0;
            for (int y = 0; y < kSpatialBlock; ++y)
                for (int x = 0; x < kSpatialBlock; ++x)
                {
                    const float l = encoded(o + int2(x, y), offset).x;
                    s += l, q += l * l;
                }
            const float n = kSpatialBlock * kSpatialBlock;
            noise.InterlockedAdd((kNoiseSpatial + detail_spatial_bin(q / n - (s / n) * (s / n))) * 4, 1u);
        }
        return;
    }
    if (mode == 5)
    {
        // A group a block, its pixels and those within the reach (a half
        // plane), white balanced, linear.
        const int2 o = int2(kSpatialReach, 0) + int2(group.xy) * kSpatialStep;
        if (o.x + kSpatialBlock + kSpatialReach > size.x || o.y + kSpatialBlock + kSpatialReach > size.y)
            return;
        const float3 offset = asfloat(noise.Load3(kNoiseModel * 4));
        for (uint i = index; i < kSpatialTile; i += 256)
        {
            const float3 b = balanced(o + int2(int(i % kSpatialTileWidth) - kSpatialReach, int(i / kSpatialTileWidth)));
            spatial_tile[0][i] = b.r, spatial_tile[1][i] = b.g, spatial_tile[2][i] = b.b;
        }
        GroupMemoryBarrierWithGroupSync();
        // Flat? Its luminance variance in the encoding, its bin against the
        // median's. And each colour's mean over the block.
        if (index == 0)
        {
            float s = 0, q = 0;
            for (int y = 0; y < kSpatialBlock; ++y)
                for (int x = 0; x < kSpatialBlock; ++x)
                {
                    const uint a = y * kSpatialTileWidth + x + kSpatialReach;
                    const float l = detail_encode(float3(spatial_tile[0][a], spatial_tile[1][a], spatial_tile[2][a]), offset).x;
                    s += l, q += l * l;
                }
            const float n = kSpatialBlock * kSpatialBlock;
            uint total = 0;
            for (uint b = 0; b < kSpatialBins; ++b)
                total += noise.Load((kNoiseSpatial + b) * 4);
            uint median = 0, seen = 0;
            for (; median < kSpatialBins; ++median)
            {
                seen += noise.Load((kNoiseSpatial + median) * 4);
                if (seen * 2 >= total)
                    break;
            }
            spatial_flat = detail_spatial_bin(q / n - (s / n) * (s / n)) <= median ? 1 : 0;
        }
        if (index < 3)
        {
            float s = 0;
            for (int y = 0; y < kSpatialBlock; ++y)
                for (int x = 0; x < kSpatialBlock; ++x)
                    s += spatial_tile[index][y * kSpatialTileWidth + x + kSpatialReach];
            spatial_sums[index] = s / (kSpatialBlock * kSpatialBlock);
        }
        GroupMemoryBarrierWithGroupSync();
        if (!spatial_flat)
            return;
        const float3 means = float3(spatial_sums[0], spatial_sums[1], spatial_sums[2]);
        GroupMemoryBarrierWithGroupSync();
        for (uint i2 = index; i2 < kSpatialTile; i2 += 256)
            [unroll] for (uint k = 0; k < 3; ++k)
                spatial_tile[k][i2] -= means[k];
        GroupMemoryBarrierWithGroupSync();
        // Each colour against each at each lag (detail_series()), over the block.
        for (uint job = index; job < kSpatialSeries * kSpatialLags; job += 256)
        {
            const uint s = job / kSpatialLags, c = detail_series_from(s), d = detail_series_to(s);
            const int2 l = detail_lag(job % kSpatialLags);
            float sum = 0;
            for (int y = 0; y < kSpatialBlock; ++y)
                for (int x = 0; x < kSpatialBlock; ++x)
                    sum += spatial_tile[c][y * kSpatialTileWidth + x + kSpatialReach] * spatial_tile[d][(y + l.y) * kSpatialTileWidth + x + l.x + kSpatialReach];
            spatial_jobs[job] = sum;
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint job2 = index; job2 < kSpatialSeries * kSpatialLags; job2 += 256)
        {
            const uint s = job2 / kSpatialLags;
            const float den = sqrt(spatial_jobs[detail_series_from(s) * kSpatialLags] * spatial_jobs[detail_series_to(s) * kSpatialLags]);
            const float v = den > 0 ? spatial_jobs[job2] / den : 0;
            noise.InterlockedAdd((kNoiseSpatialSums + job2) * 4, asuint(int(round(clamp(v, -1.0, 1.0) * 4096))));
        }
        if (index == 0)
            noise.InterlockedAdd(kNoiseSpatialCount * 4, 1u);
        return;
    }
    if (mode == 6)
    {
        // One group. The spatial correlation from the sums (detail.cpp's
        // measure_spatial(): the blocks' own means undone, each series by its
        // own sum over the whole window), each colour's pixel variance
        // against the fit's measure; then each colour's law, and its
        // expectation inverse.
        const uint blocks = noise.Load(kNoiseSpatialCount * 4);
        const bool spatial = blocks >= 16;
        for (uint i = index; i < kSpatialSeries * kSpatialLags; i += 256)
        {
            const float measured = spatial ? float(int(noise.Load((kNoiseSpatialSums + i) * 4))) / (4096.0 * blocks) : 0;
            spatial_rho[0][i] = measured, spatial_rho[1][i] = measured;
        }
        GroupMemoryBarrierWithGroupSync();
        // Three times over, every thread (the barriers in flow all take);
        // unmeasured, nothing to do.
        for (uint iteration = 0; iteration < 3; ++iteration)
        {
            if (spatial && index < kSpatialSeries)
            {
                // The whole window: this series' half plane, and its mirror's but for the lag 0 they share.
                const uint s = index, mirror = detail_series(detail_series_to(s), detail_series_from(s));
                float sum = 0;
                for (uint l = 0; l < kSpatialLags; ++l)
                    sum += spatial_rho[1][s * kSpatialLags + l] + (l > 0 ? spatial_rho[1][mirror * kSpatialLags + l] : 0);
                spatial_jobs[s] = sum;
            }
            GroupMemoryBarrierWithGroupSync();
            for (uint i3 = index; spatial && i3 < kSpatialSeries * kSpatialLags; i3 += 256)
            {
                const float f = clamp(spatial_jobs[i3 / kSpatialLags], 0.0, 64.0) / (kSpatialBlock * kSpatialBlock);
                spatial_rho[1][i3] = spatial_rho[0][i3] * (1 - f) + f;
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (index < 3)
            spatial_rho[1][index * kSpatialLags] = 1;
        GroupMemoryBarrierWithGroupSync();
        for (uint i4 = index; i4 < kSpatialSeries * kSpatialLags; i4 += 256)
            noise.Store((kNoiseRho + i4) * 4, asuint(spatial ? spatial_rho[1][i4] : (i4 % kSpatialLags == 0 && i4 < 3 * kSpatialLags ? 1.0 : 0.0)));
        float3 pixel = 1;
        if (spatial)
        {
            [unroll] for (uint c = 0; c < 3; ++c)
            {
                const uint b = c * kSpatialLags;  // the colour against itself
                const float v = 1 + (4 + 2 * (spatial_rho[1][b + detail_lag_index(2, 0)] + spatial_rho[1][b + detail_lag_index(0, 2)] +
                                              2 * spatial_rho[1][b + detail_lag_index(1, 1)] + 2 * spatial_rho[1][b + detail_lag_index(1, -1)])) / 16 -
                                (spatial_rho[1][b + detail_lag_index(1, 0)] + spatial_rho[1][b + detail_lag_index(0, 1)]);
                pixel[c] = clamp(1.25 / max(v, 1e-3), 0.5, 8.0);
            }
        }
        const float3 offset = asfloat(noise.Load3(kNoiseModel * 4));
        const float3 fa = asfloat(noise.Load3((kNoiseModel + 3) * 4)), fb = asfloat(noise.Load3((kNoiseModel + 6) * 4));
        const uint fitted = noise.Load((kNoiseModel + 9) * 4);
        // Each colour's expectation inverse (detail.hlsli's
        // detail_expected()), by its own law: its own a and b (where it has
        // no fit of its own, green's as the develop leaves that colour's
        // against green's) as a pixel has them: by the develop's own
        // factors, or (read.y: LibRaw's, an sRAW's or mRAW's, whose
        // in-camera scaling isn't ours to know) by the measured
        // correlation. Its averages first, at levels evenly
        // spaced through the transform from 6 sigma of the readout's noise
        // below 0 to kFitTop; then the level for each of kInverseSize
        // averages evenly spaced between the first and the last, between the
        // two it falls between.
        const bool measured = read.y != 0 && spatial;
        const float3 law_light = measured ? pixel : light.xyz, law_readout = measured ? pixel : readout.xyz;
        const float3 cl = correlation_light.xyz, cr = correlation_readout.xyz;
        const bool own[3] = {(fitted & 1) != 0, true, (fitted & 4) != 0};
        float3 pa, pb, from = 0, to = 0;
        [unroll] for (uint c2 = 0; c2 < 3; ++c2)
        {
            pa[c2] = (own[c2] ? fa[c2] : fa.g * green_light[c2]) * law_light[c2];
            pb[c2] = (own[c2] ? fb[c2] : fb.g * green_readout[c2]) * law_readout[c2];
        }
        if (fitted & 2)
        {
            for (uint i = index; i < 3 * kForwardSize; i += 256)
            {
                const uint c = i / kForwardSize, k = i % kForwardSize;
                const float lo = detail_forward(-6 * sqrt(pb[c]), offset.g), hi = detail_forward(kFitTop, offset.g);
                const float y = detail_inverse(lo + (hi - lo) * k / (kForwardSize - 1), offset.g);
                fit_forward[c][k] = detail_expected(y, offset.g, pa[c], pb[c]);
            }
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint entry = index; (fitted & 2) && entry < kInverseSize; entry += 256)
        {
            [unroll] for (uint c = 0; c < 3; ++c)
            {
                const float lo = detail_forward(-6 * sqrt(pb[c]), offset.g), hi = detail_forward(kFitTop, offset.g);
                const float first = fit_forward[c][0], last = fit_forward[c][kForwardSize - 1];
                const float want = first + (last - first) * entry / (kInverseSize - 1);
                uint below = 0, above = kForwardSize - 1;
                while (above - below > 1)
                {
                    const uint mid = (below + above) / 2;
                    if (fit_forward[c][mid] <= want)
                        below = mid;
                    else
                        above = mid;
                }
                const float t = saturate((want - fit_forward[c][below]) / max(fit_forward[c][above] - fit_forward[c][below], 1e-20));
                const float e = lo + (hi - lo) * (below + t) / (kForwardSize - 1);
                noise.Store((kNoiseInverse + c * kInverseSize + entry) * 4, asuint(detail_inverse(e, offset.g)));
                from[c] = first, to[c] = last;
            }
        }
        if (index != 0)
            return;
        noise.Store3(kNoisePixel * 4, asuint(pixel));
        noise.Store((kNoisePixel + 3) * 4, spatial ? 1u : 0u);
        noise.Store3((kNoiseModel + 11) * 4, asuint((fitted & 2) ? from : float3(0, 0, 0)));
        noise.Store3((kNoiseModel + 14) * 4, asuint((fitted & 2) ? to : float3(0, 0, 0)));
        noise.Store3(kNoiseLaw * 4, asuint(pa));
        noise.Store3((kNoiseLaw + 3) * 4, asuint(pb));
        noise.Store3((kNoiseLaw + 6) * 4, asuint(cl));
        noise.Store3((kNoiseLaw + 9) * 4, asuint(cr));
        return;
    }
    if (mode == 3)
    {
        if (id.x == 0 && id.y == 0)
        {
            noise.Store(kNoiseSigma * 4, asuint(median(0) / 0.6745));
            noise.Store((kNoiseSigma + 1) * 4, asuint(median(kNoiseBins) / 0.6745));
        }
        return;
    }
    const int2 p = int2(id.xy) * kNoiseStride;
    if (any(p >= size))
        return;
    const float3 offset = asfloat(noise.Load3(kNoiseModel * 4));
    float3 smooth = 0;
    for (int v = -2; v <= 2; ++v)
        for (int u = -2; u <= 2; ++u)
            smooth += kB3[u + 2] * kB3[v + 2] * encoded(p + int2(u, v), offset);
    const float3 d = abs(encoded(p, offset) - smooth);
    count(0, d.x);
    count(kNoiseBins, d.y);
    count(kNoiseBins, d.z);
}

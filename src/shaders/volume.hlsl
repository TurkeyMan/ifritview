// The image volume (view.hlsli's fit_volume()): over 72 Oklab hues and 32
// lightness slices (0 to the display white's), how far the measured level's
// colours lie against the display's boundary there once the brightness curve
// has them, along each colour's projection line (edge_x(), 1 on the
// boundary). After stats.hlsl and peak.hlsl, and again whenever the
// exposure, white balance, develop edits or display's peak change
// (GpuView::measure_volume):
//  - CLEAR (one group): the counts from zero, the brightness curve's too;
//  - stats.hlsl's CURVE and TOP: the luminance as edited;
//  - CURVE (one thread): the brightness curve the measure takes the colours
//    through;
//  - the measure: per cell, the largest and second largest, the second the
//    table's (one stray pixel can't set it);
//  - PREPARE (one group): each cell's spread to its neighbours (the largest
//    of the 3 x 3 round it) and smoothed ([1 2 1] each way), round in hue,
//    and never under the cell's own.
#define COMPUTE
#include "view.hlsli"

static const uint kCells = kVolumeHues * kVolumeSlices;

#if defined(CLEAR)
[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    for (uint i = index; i < kCells * 2; i += 256)
        stats.Store((kStatsVolumeRaw + i) * 4, 0u);
    for (uint j = index; j < 1024; j += 256)
    {
        stats.Store((kStatsCurveLuminance + j) * 4, 0u);
        stats.Store((kStatsTopCounts + j) * 4, 0u);
    }
    if (index == 0)
    {
        stats.Store(kStatsCurveMost * 4, 0u);
        stats.Store(kStatsTopMost * 4, 0u);
    }
}
#elif defined(CURVE)
// The brightness curve (view.hlsli's brightness()), by histogram adjustment,
// from stats.hlsl's CURVE and TOP: the luminance brightness() is given.
//
// The top: at full resolution (TOP), past its kTopPixels brightest pixels
// only (the develop has removed impulses already, so a few is enough), the
// top edge of the bin they reach, never past the brightest itself. With
// fewer counted than that, the measured level's greatest.
//
// The curve: the knee (curve_knee()), then CURVE's histogram, each of its
// bins' counts spread over the kCurveBins equal bins in log between the knee
// and the top that it covers, and shaped (brightness_shape.hlsli).
static const uint kTopPixels = 4;
groupshared float g_count[kCurveBins], g_floor[kCurveBins];
#define CURVE_LOOP [loop]
#include "brightness_shape.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    const float most = asfloat(stats.Load(kStatsCurveMost * 4)), from = most / 8;
    float top_y = most;
    uint above = 0;
    for (int t = 1023; t >= 0 && from > 0; --t)
    {
        above += stats.Load((kStatsTopCounts + t) * 4);
        if (above >= kTopPixels)
        {
            top_y = min(from * exp2(float(t + 1) / 128), asfloat(stats.Load(kStatsTopMost * 4)));
            break;
        }
    }
    stats.Store(kStatsTop * 4, asuint(top_y));

    const float peak = log2(display_peak), m = least_slope();
    const float top = top_y > 0 ? log2(top_y) : peak;
    if (!(top > peak))
    {
        stats.Store(kStatsCurve * 4, asuint(peak));  // fits: the identity
        stats.Store((kStatsCurve + 1) * 4, asuint(peak));
        return;
    }
    const float knee = curve_knee(top, peak, m), width = (top - knee) / kCurveBins;
    stats.Store(kStatsCurve * 4, asuint(knee));
    stats.Store((kStatsCurve + 1) * 4, asuint(top));
    [loop] for (uint i = 0; i < kCurveBins; ++i)
        g_count[i] = 0;
    [loop] for (uint b = 0; b < 1024; ++b)
    {
        const float n = float(stats.Load((kStatsCurveLuminance + b) * 4));
        if (n == 0)
            continue;
        const float a = (float(b) * 32 / 1024 - 20 - knee) / width, z = (float(b + 1) * 32 / 1024 - 20 - knee) / width;
        if (z <= 0)
            continue;
        if (a >= float(kCurveBins))
        {
            g_count[kCurveBins - 1] += n;
            continue;
        }
        const float lo = max(a, 0.0), hi = min(z, float(kCurveBins)), per = n / (z - a);
        [loop] for (uint k = uint(lo); k < kCurveBins && float(k) < hi; ++k)
            g_count[k] += per * (min(hi, k + 1.0) - max(lo, float(k)));
        if (z > float(kCurveBins))
            g_count[kCurveBins - 1] += per * (z - float(kCurveBins));
    }
    curve_shape(knee, top, peak, m);
    // The bins' edges, the top exactly on the peak.
    float at = knee;
    stats.Store((kStatsCurve + 2) * 4, asuint(at));
    [loop] for (uint e = 0; e < kCurveBins; ++e)
    {
        at += g_count[e];
        stats.Store((kStatsCurve + 3 + e) * 4, asuint(e + 1 == kCurveBins ? peak : at));
    }
}
#elif defined(PREPARE)
groupshared float own[kCells], spread[kCells], out_l[kCells];

uint cell(int hue, int slice)
{
    return uint(hue + int(kVolumeHues)) % kVolumeHues * kVolumeSlices + uint(clamp(slice, 0, int(kVolumeSlices) - 1));
}

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    for (uint i = index; i < kCells; i += 256)
        own[i] = asfloat(stats.Load((kStatsVolumeRaw + i * 2 + 1) * 4));
    GroupMemoryBarrierWithGroupSync();
    for (uint i = index; i < kCells; i += 256)
    {
        const int hue = int(i / kVolumeSlices), slice = int(i % kVolumeSlices);
        float m = 0;
        for (int dh = -1; dh <= 1; ++dh)
            for (int ds = -1; ds <= 1; ++ds)
                m = max(m, own[cell(hue + dh, slice + ds)]);
        spread[i] = m;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint i = index; i < kCells; i += 256)
    {
        const int hue = int(i / kVolumeSlices), slice = int(i % kVolumeSlices);
        float sum = 0;
        for (int dh = -1; dh <= 1; ++dh)
            for (int ds = -1; ds <= 1; ++ds)
                sum += spread[cell(hue + dh, slice + ds)] * (2 - abs(dh)) * (2 - abs(ds));
        const float limit = max(own[i], sum / 16);
        stats.Store((kStatsVolume + i) * 4, asuint(limit));
        // Where the slice's own furthest colour at this hue comes out, in
        // lightness (at chroma 0 nothing moves, so the slice's own lightness).
        const float h = (hue + 0.5) * kTwoPi / kVolumeHues, L = (slice + 0.5) / kVolumeSlices * pow(display_peak, 1.0 / 3);
        const float2 dir = float2(cos(h), sin(h));
        const float edge = max_chroma(L, dir);
        out_l[i] = limit > 0 && edge > 0 ? min(L, fit_with(float3(L, dir * (edge * limit)), limit).x) : L;
    }
    GroupMemoryBarrierWithGroupSync();
    // Up each hue's slices, the floor: never under the slice's own output,
    // and its log rising at least least_slope() of the input's from the slice
    // below, so a brightening gradient stays brightening.
    if (index < kVolumeHues)
    {
        float least = 0;
        for (uint s = 0; s < kVolumeSlices; ++s)
        {
            least = s == 0 ? out_l[index * kVolumeSlices]
                           : max(out_l[index * kVolumeSlices + s], least * pow((s + 0.5) / max(s - 0.5, 0.5), least_slope()));
            stats.Store((kStatsFloor + index * kVolumeSlices + s) * 4, asuint(least));
        }
    }
}
#else
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    source.GetDimensions(w, h);
    if (id.x >= w || id.y >= h)
        return;
    const float4 c = source.Load(int3(id.xy, 0));
    if (c.a <= 0)
        return;
    g_measuring = true;
    g_volume_x = 0;
    tone_map(max(c.rgb / c.a, 0));
    if (g_volume_x <= 0)
        return;
    const uint2 at = volume_cell(g_volume_hue, g_volume_l);
    const uint i = at.x * kVolumeSlices + at.y, v = asuint(min(g_volume_x, 4.0));  // positive floats order as their bits
    uint was;
    stats.InterlockedMax((kStatsVolumeRaw + i * 2) * 4, v, was);
    stats.InterlockedMax((kStatsVolumeRaw + i * 2 + 1) * 4, min(v, was));
}
#endif

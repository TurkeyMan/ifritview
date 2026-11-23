// The image's luminance measures, once it is on the device: a histogram
// of log2 luminance for peak.hlsl, into view.hlsli's stats words. A raw uses
// a filtered level at most 2048 pixels wide; an ordinary image uses full
// resolution so a single bright detail counts. Group-local first, then
// one atomic per bin.
//
// A raw's brightness curve is made from its luminance as edited (exposure,
// white balance, develop controls: view.hlsli's edit(), what brightness()
// is given), measured again whenever those change (GpuView::measure_volume),
// after volume.hlsl's CLEAR:
//  - CURVE: a histogram of it, and its greatest, on the measured level;
//  - TOP: its true maximum, at full resolution (the develop's own output,
//    impulses already removed): each pixel from an eighth of CURVE's greatest
//    counted at 128 bins a stop, for volume.hlsl's CURVE to take the top past
//    its few brightest pixels.
#define COMPUTE
#include "view.hlsli"

// A pixel's luminance as edited, as tone_map() gives brightness() it.
float edited_luminance(float4 c)
{
    return c.a > 0 ? edit(auto_exposure ? max(c.rgb / c.a, 0) : c.rgb / c.a, exposure * auto_gain()).y : 0;
}

#if defined(CURVE)
groupshared uint local_curve[1024];
groupshared uint local_curve_most;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < 1024; i += 256)
        local_curve[i] = 0;
    if (index == 0)
        local_curve_most = 0;
    GroupMemoryBarrierWithGroupSync();

    uint w, h;
    source.GetDimensions(w, h);
    if (id.x < w && id.y < h)
    {
        const float y = edited_luminance(source.Load(int3(id.xy, 0)));
        if (y > 0)
        {
            InterlockedAdd(local_curve[uint(clamp((log2(y) + 20) * 1024 / 32, 0.0, 1023.0))], 1u);
            InterlockedMax(local_curve_most, asuint(y));  // positive floats order as their bits
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = index; j < 1024; j += 256)
    {
        if (local_curve[j])
            stats.InterlockedAdd((kStatsCurveLuminance + j) * 4, local_curve[j]);
    }
    if (index == 0 && local_curve_most)
        stats.InterlockedMax(kStatsCurveMost * 4, local_curve_most);
}
#elif defined(TOP)
groupshared uint local_top[1024];
groupshared uint local_top_most, local_top_any;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < 1024; i += 256)
        local_top[i] = 0;
    if (index == 0)
        local_top_most = local_top_any = 0;
    GroupMemoryBarrierWithGroupSync();

    uint w, h;
    source.GetDimensions(w, h);
    const float from = asfloat(stats.Load(kStatsCurveMost * 4)) / 8;
    if (id.x < w && id.y < h && from > 0)
    {
        const float y = edited_luminance(source.Load(int3(id.xy, 0)));
        if (y > from)
        {
            InterlockedAdd(local_top[uint(min(log2(y / from) * 128, 1023.0))], 1u);
            InterlockedMax(local_top_most, asuint(y));  // positive floats order as their bits
            local_top_any = 1;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (!local_top_any)
        return;
    for (uint j = index; j < 1024; j += 256)
    {
        if (local_top[j])
            stats.InterlockedAdd((kStatsTopCounts + j) * 4, local_top[j]);
    }
    if (index == 0)
        stats.InterlockedMax(kStatsTopMost * 4, local_top_most);
}
#else
groupshared uint local_luminance[1024];
groupshared uint local_count, local_opaque;
groupshared uint local_least, local_most;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < 1024; i += 256)
        local_luminance[i] = 0;
    if (index == 0)
        local_count = local_opaque = local_least = local_most = 0;
    GroupMemoryBarrierWithGroupSync();

    uint w, h;
    source.GetDimensions(w, h);
    if (id.x < w && id.y < h)
    {
        const float4 c = source.Load(int3(id.xy, 0));
        if (c.a > 0)
        {
            InterlockedAdd(local_opaque, 1u);
            const float3 rgb = c.rgb / c.a;
            const float3 xyz = mul(float3x3(input_x.xyz, input_y.xyz, input_z.xyz), rgb);
            if (xyz.y > 0)
            {
                const uint bin = uint(clamp((log2(xyz.y) + 20) * 1024 / 32, 0.0, 1023.0));
                InterlockedAdd(local_luminance[bin], 1u);
                InterlockedAdd(local_count, 1u);
                InterlockedMax(local_least, ~asuint(xyz.y));  // positive floats order as their bits
                InterlockedMax(local_most, asuint(xyz.y));

            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = index; j < 1024; j += 256)
    {
        if (local_luminance[j])
            stats.InterlockedAdd((kStatsLuminance + j) * 4, local_luminance[j]);
    }
    if (index == 0 && local_count)
    {
        stats.InterlockedAdd(kStatsCount * 4, local_count);
        stats.InterlockedMax(kStatsMinimum * 4, local_least);
        stats.InterlockedMax(kStatsMaximum * 4, local_most);
    }
    if (index == 0 && local_opaque)
        stats.InterlockedAdd(kStatsOpaque * 4, local_opaque);
}
#endif

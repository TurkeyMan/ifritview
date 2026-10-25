// The image's measures, once it's on the device (raw and HDR images), from
// a level of at most 2048 pixels: a histogram of log2 luminance (for the
// peak, peak.hlsl) and how far from white it goes in each hue direction (1 =
// the display's edge), into view.hlsli's `stats` words. Group-local first,
// then one atomic per bin.
#define COMPUTE
#include "view.hlsli"

groupshared uint local_luminance[1024];
groupshared uint local_extents[72];
groupshared uint local_count;
groupshared uint local_least, local_most;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < 1024; i += 256)
        local_luminance[i] = 0;
    if (index < 72)
        local_extents[index] = 0;
    if (index == 0)
        local_count = local_least = local_most = 0;
    GroupMemoryBarrierWithGroupSync();

    uint w, h;
    source.GetDimensions(w, h);
    if (id.x < w && id.y < h)
    {
        const float4 c = source.Load(int3(id.xy, 0));
        if (c.a > 0)
        {
            const float3 rgb = c.rgb / c.a;
            const float3 xyz = mul(kRgbToXyz709, rgb);
            if (xyz.y > 0)
            {
                const uint bin = uint(clamp((log2(xyz.y) + 20) * 1024 / 32, 0.0, 1023.0));
                InterlockedAdd(local_luminance[bin], 1);
                InterlockedAdd(local_count, 1);
                InterlockedMax(local_least, ~asuint(xyz.y));  // positive floats order as their bits
                InterlockedMax(local_most, asuint(xyz.y));
                const float2 v = uv_from_xyz(xyz) - kWhiteUv;
                const float len = length(v);
                if (xyz.y >= 1e-3 && len > 1e-6)  // too dark for its colour to count
                {
                    const float2 dir = v / len;
                    const float extent = len / edge_distance(dir, display_r, display_g, display_b);
                    const uint hue = uint((atan2(dir.y, dir.x) / kTwoPi + 1) * 72) % 72u;
                    InterlockedMax(local_extents[hue], asuint(extent));  // positive floats order as their bits
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = index; j < 1024; j += 256)
        if (local_luminance[j])
            stats.InterlockedAdd((kStatsLuminance + j) * 4, local_luminance[j]);
    if (index < 72 && local_extents[index])
        stats.InterlockedMax((kStatsExtents + index) * 4, local_extents[index]);
    if (index == 0 && local_count)
    {
        stats.InterlockedAdd(kStatsCount * 4, local_count);
        stats.InterlockedMax(kStatsMinimum * 4, local_least);
        stats.InterlockedMax(kStatsMaximum * 4, local_most);
    }
}

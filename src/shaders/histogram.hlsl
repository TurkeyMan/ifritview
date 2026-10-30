// The classic histogram of the image as it's shown: each pixel of a level
// of at most 2048 pixels through the display mapping, sRGB-encoded, counted
// in 256 bins of R, G, B and luminance (Rec. 709 luma of the encoded
// values) into `bins`: words 0..1023, R then G then B then Y.
#define COMPUTE
#include "view.hlsli"

RWByteAddressBuffer bins : register(u0);

groupshared uint local_bins[1024];


[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < 1024; i += 256)
        local_bins[i] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint w, h;
    source.GetDimensions(w, h);
    if (id.x < w && id.y < h)
    {
        const float4 c = source.Load(int3(id.xy, 0));
        if (c.a > 0)
        {
            const float3 shown = tone_map(max(c.rgb / c.a, 0));
            const float3 e = float3(srgb_encode(shown.r), srgb_encode(shown.g), srgb_encode(shown.b));
            const uint3 b = uint3(e * 255 + 0.5);
            const uint y = uint(dot(e, float3(0.2126, 0.7152, 0.0722)) * 255 + 0.5);
            InterlockedAdd(local_bins[b.r], 1);
            InterlockedAdd(local_bins[256 + b.g], 1);
            InterlockedAdd(local_bins[512 + b.b], 1);
            InterlockedAdd(local_bins[768 + min(y, 255u)], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = index; j < 1024; j += 256)
        if (local_bins[j])
            bins.InterlockedAdd(j * 4, local_bins[j]);
}

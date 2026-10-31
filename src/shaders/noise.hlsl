// The image's noise for noise reduction (detail.h): the finest wavelet
// scale's absolute detail on a lattice across level 0, counted into a
// histogram (luminance, colour); then, one thread, each one's median as a
// standard deviation (detail.cpp's measure(), the CPU's).
#include "detail.hlsli"

cbuffer Params : register(b0)
{
    int mode;        // 0: count, 1: the medians
    int2 size;       // level 0's
    int unused;
    float4 as_shot;  // what level 0 is white balanced with (its data / these)
};

Texture2D<float4> source : register(t0);
RWByteAddressBuffer noise : register(u0);

float3 encoded(int2 p)
{
    return detail_encode(source.Load(int3(clamp(p, 0, size - 1), 0)).rgb * as_shot.rgb);
}

void count(uint bin_base, float v)
{
    noise.InterlockedAdd((bin_base + min(uint(v / kNoiseRange * kNoiseBins), kNoiseBins - 1)) * 4, 1);
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

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (mode == 1)
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
    float3 smooth = 0;
    for (int v = -2; v <= 2; ++v)
        for (int u = -2; u <= 2; ++u)
            smooth += kB3[u + 2] * kB3[v + 2] * encoded(p + int2(u, v));
    const float3 d = abs(encoded(p) - smooth);
    count(0, d.x);
    count(kNoiseBins, d.y);
    count(kNoiseBins, d.z);
}

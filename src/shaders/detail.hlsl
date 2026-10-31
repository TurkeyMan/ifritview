// Noise reduction and sharpening (detail.h), a region of level 0 at a time:
// the passes GpuView::filter_detail queues, one mode each. A region's
// texels are image positions from `origin`; reads past its edges clamp to
// them (the image's own at the image's edges). Step for step what
// detail.cpp's apply_detail() does on the CPU.
#include "detail.hlsli"

#define DETAIL_ENCODE 0   // level 0 (t0) to the encoding
#define DETAIL_SMOOTH 1   // t0 smoothed by the B3 spline along `axis`, `step` apart
#define DETAIL_SHRINK 2   // the sum so far (t2), plus scale `step`'s detail (t0 less t1) shrunk
#define DETAIL_COMBINE 3  // the coarsest (t0) and the details kept (t1) together
#define DETAIL_BLUR 4     // t0's luminance by the sharpening Gaussian along `axis`
#define DETAIL_DECODE 5   // the encoding (t0), sharpened (t1 its blur), back to level 0's data (t2's alpha)

cbuffer Params : register(b0)
{
    int mode;
    int axis;
    int step;
    int sharpening;     // decode: sharpen
    int2 origin;        // the image position of the region's texel (0, 0)
    int2 size;          // the region's
    int2 out_origin;    // decode: the image position of the target's pixel (0, 0)
    int2 image_size;    // level 0's
    float4 as_shot;     // what level 0 is white balanced with (its data / these)
    float4 luminance;   // shrink: scales 0..3's thresholds, multiples of the noise
    float4 colour;
    float4 coarsest;    // scale 4's: luminance, colour
    float4 sharpen;     // amount, the Gaussian's sigma, halo
    float4 mask;        // low, high (0: none)
};

Texture2D<float4> a : register(t0);
Texture2D<float4> b : register(t1);
Texture2D<float4> c : register(t2);
ByteAddressBuffer noise : register(t8);  // noise.hlsl's

float4 at(Texture2D<float4> t, int2 p)
{
    return t.Load(int3(clamp(p, 0, size - 1), 0));
}

float shrink(float d, float t)
{
    return d > t ? d - t : d < -t ? d + t : 0;
}

// Its direction, the hue, kept.
float2 shrink(float2 d, float t)
{
    const float m = length(d);
    return m > t ? d * (m - t) / m : 0;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 p = int2(position.xy);
    const int2 dir = axis == 0 ? int2(1, 0) : int2(0, 1);
    if (mode == DETAIL_ENCODE)
        return float4(detail_encode(a.Load(int3(clamp(p + origin, 0, image_size - 1), 0)).rgb * as_shot.rgb), 0);
    if (mode == DETAIL_SMOOTH)
    {
        float4 sum = 0;
        for (int k = -2; k <= 2; ++k)
            sum += kB3[k + 2] * at(a, p + k * step * dir);
        return sum;
    }
    if (mode == DETAIL_SHRINK)
    {
        const int j = step;
        const float3 d = at(a, p).xyz - at(b, p).xyz;
        const float noise_l = asfloat(noise.Load(kNoiseSigma * 4)), noise_c = asfloat(noise.Load((kNoiseSigma + 1) * 4));
        const float tl = (j < 4 ? luminance[j] : coarsest.x) * noise_l, tc = (j < 4 ? colour[j] : coarsest.y) * noise_c;
        const float3 kept = float3(shrink(d.x, tl), shrink(d.yz, tc));
        return float4((j > 0 ? at(c, p).xyz : 0) + kept, 0);
    }
    if (mode == DETAIL_COMBINE)
        return float4(at(a, p).xyz + at(b, p).xyz, 0);
    if (mode == DETAIL_BLUR)
    {
        const int half_width = int(ceil(3 * sharpen.y));
        float sum = 0, total = 0;
        for (int k = -half_width; k <= half_width; ++k)
        {
            const float w = exp(-0.5 * k * k / (sharpen.y * sharpen.y));
            sum += w * at(a, p + k * dir).x;
            total += w;
        }
        return float4(sum / total, 0, 0, 0);
    }
    // DETAIL_DECODE
    const int2 image = p + out_origin, q = image - origin;
    float3 e = at(a, q).xyz;
    if (sharpening)
    {
        const float d = e.x - at(b, q).x;
        float m = 1;
        if (mask.y > 0)
        {
            const float2 g = float2(at(b, q + int2(1, 0)).x - at(b, q - int2(1, 0)).x, at(b, q + int2(0, 1)).x - at(b, q - int2(0, 1)).x) / 2;
            m = smoothstep(mask.x, mask.y, length(g));
        }
        e.x += sharpen.x * m * d * sharpen.z / (sharpen.z + abs(d));
    }
    return float4(detail_decode(e) / as_shot.rgb, c.Load(int3(clamp(image, 0, image_size - 1), 0)).a);
}

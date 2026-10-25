// The view's frame: the image (filtered vertically from the first pass's
// rows, or nearest from the image itself), exposed and tone-mapped, over
// the transparency checkerboard or the background, all in linear light,
// then encoded for the front buffer: sRGB (8-bit dithered, or 10-bit), PQ,
// or linear scRGB.
#include "resample.hlsli"
#include "view.hlsli"

#include "histogram.hlsli"



float srgb_encode(float v)
{
    v = saturate(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float pq_encode(float nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float y = pow(saturate(nits / 10000), m1);
    return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

float srgb_decode(float v)
{
    return v <= 0.04045 ? v / 12.92 : pow(max(v + 0.055, 0.0) / 1.055, 2.4);
}
float3 srgb_decode3(float3 c)
{
    return float3(srgb_decode(c.r), srgb_decode(c.g), srgb_decode(c.b));
}

// A pixel's own noise in -1..1, triangular: what 8-bit output adds to hide banding.
float dither(int2 p)
{
    const float a = frac(sin(dot(float2(p), float2(12.9898, 78.233))) * 43758.5453);
    const float b = frac(sin(dot(float2(p), float2(39.3468, 11.135))) * 24634.6345);
    return a + b - 1;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 p = int2(position.xy);
    // How much of this frame pixel the image covers (its edges may fall part
    // way across one).
    const float2 lo = max(float2(p), image_rect.xy), hi = min(float2(p) + 1, image_rect.zw);
    const float cover = max(0.0, hi.x - lo.x) * max(0.0, hi.y - lo.y);

    float4 c = 0;
    if (cover > 0)
    {
        if (nearest)
        {
            const int2 t = clamp(int2(floor((float2(p) + 0.5) * float2(inv_x, inv) + float2(offset_x, offset))), 0,
                                 int2(columns, rows) - 1);
            c = source.Load(int3(t, 0));
        }
        else
        {
            const float v = (p.y + 0.5) * inv + offset;
            int first, count;
            taps(filter, v, inv, first, count);
            float total = 0;
            for (int k = 0; k < count; ++k)
            {
                const int t = first + k;
                const float w = weight(filter, float(t), v, inv);
                c += w * source.Load(int3(p.x - column, clamp(t, 0, rows - 1), 0));
                total += w;
            }
            c = total != 0 ? c / total : 0;
        }
        // Negative lobes can overshoot: alpha into 0..1 and colour not below
        // nothing (it may go past alpha: highlights brighter than white).
        c.a = saturate(c.a);
        c.rgb = max(c.rgb, 0);
        // Exposed and tone-mapped as a colour, then premultiplied again.
        if (c.a > 0)
            c.rgb = tone_map(c.rgb / c.a) * c.a;
        c *= cover;
    }

    // Under it: the checkerboard where the image is (and has transparency),
    // else the background.
    float4 under = background;
    if (checker_size > 0 && cover > 0)
        under = ((uint(p.x) / uint(checker_size) + uint(p.y) / uint(checker_size)) & 1u) ? checker_a : checker_b;
    float3 rgb = c.rgb + (1 - c.a) * under.rgb;

    // The histogram panel over it (sRGB-encoded colours, blended in linear).
    if (show_histogram && p.x >= panel.x && p.x < panel.z && p.y >= panel.y && p.y < panel.w)
    {
        const float4 h = histogram_panel(float2(p) + 0.5, panel);
        rgb = lerp(rgb, srgb_decode3(h.rgb), h.a);
    }
    if (output == 3)
        return float4(rgb, 1);
    if (output == 2)
        return float4(pq_encode(rgb.r * white), pq_encode(rgb.g * white), pq_encode(rgb.b * white), 1);
    float3 e = float3(srgb_encode(rgb.r), srgb_encode(rgb.g), srgb_encode(rgb.b));
    if (output == 0)
        e += dither(p) / 255;
    return float4(e, 1);
}

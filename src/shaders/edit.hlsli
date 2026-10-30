// The edit stage's adjustments (src/adjust.h): Lightroom's develop
// controls, worked out on the CPU into two tables that this only looks up,
// as apply_adjustments() does on the CPU, step for step. In linear light,
// in the working space (linear ProPhoto: `to_working` from XYZ, D65).

#include "srgb.hlsli"

Texture2D<float4> adjust_tone : register(t4);    // 4096 x 1: master, red, green, blue, linear out, at kToneTop's steps of sRGB's encoding
Texture2D<float4> adjust_colour : register(t5);  // 72 hues x 17 saturations: hue shift (turns), saturation scale, value scale

static const int kTone = 4096;
static const float kToneTopLinear = 16;
static const int kHues = 72, kSats = 17;


// A linear value through a column of the tone table; past its top, the
// scale at the top.
float tone_lookup(float x, int column, float top_encoded)
{
    const float4 pick = float4(column == 0, column == 1, column == 2, column == 3);
    float out_value;
    if (x <= 0)  // on at slope 1 below black
        out_value = dot(adjust_tone.Load(int3(0, 0, 0)), pick) + x;
    else if (x >= kToneTopLinear)
        out_value = x * dot(adjust_tone.Load(int3(kTone - 1, 0, 0)), pick) / kToneTopLinear;
    else
    {
        const float at = srgb_encode_open(x) / top_encoded * (kTone - 1);
        const int i = min(int(at), kTone - 2);
        out_value = dot(lerp(adjust_tone.Load(int3(i, 0, 0)), adjust_tone.Load(int3(i + 1, 0, 0)), at - i), pick);
    }
    return out_value;
}

float3 to_hsv(float3 c)
{
    const float v = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), range = v - lo;
    const float s = v > 0 ? range / v : 0;
    float h = 0;
    if (range > 0)
    {
        if (v == c.r)
            h = (c.g - c.b) / range;
        else if (v == c.g)
            h = 2 + (c.b - c.r) / range;
        else
            h = 4 + (c.r - c.g) / range;
        h /= 6;
        if (h < 0)
            h += 1;
    }
    return float3(h, s, v);
}

float3 from_hsv(float h, float s, float v)
{
    h = (h - floor(h)) * 6;
    const int i = min(5, int(h));
    const float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    return i == 0 ? float3(v, t, p) : i == 1 ? float3(q, v, p) : i == 2 ? float3(p, v, t) : i == 3 ? float3(p, q, v) : i == 4 ? float3(t, p, v) : float3(v, p, q);
}

// A working-space colour through the colour map, then the tone curves;
// `luminance` its Y (the working space's row of XYZ).
float3 adjust(float3 w, float3 luminance, float top_encoded, bool channels)
{
    const float3 hsv = to_hsv(w);
    if (hsv.z > 0)
    {
        const float x = hsv.x * kHues, y = min(hsv.y, 1.0) * (kSats - 1);
        const uint x0 = uint(x) % uint(kHues), x1 = (x0 + 1u) % uint(kHues);
        const int y0 = min(int(y), kSats - 2);
        const float fx = x - floor(x), fy = y - y0;
        const float3 e = lerp(lerp(adjust_colour.Load(int3(x0, y0, 0)).xyz, adjust_colour.Load(int3(x1, y0, 0)).xyz, fx),
                              lerp(adjust_colour.Load(int3(x0, y0 + 1, 0)).xyz, adjust_colour.Load(int3(x1, y0 + 1, 0)).xyz, fx), fy);
        if (e.x != 0)
            w = from_hsv(hsv.x + e.x, hsv.y, hsv.z);
        // Chroma scaled about the colour's luminance, no further than the
        // working space's edge (where a component reaches 0).
        const float lum = dot(luminance, w);
        float k = e.y;
        [unroll] for (int i = 0; i < 3; ++i)
            if (k > 1 && w[i] < lum && w[i] >= 0)
                k = min(k, max(1.0, lum / (lum - w[i])));
        w = (lum + k * (w - lum)) * e.z;
    }
    // The largest and smallest components through the master curve, the
    // middle one kept where it was between them (so hue holds); then each
    // channel's own.
    const float hi = max(w.r, max(w.g, w.b)), lo = min(w.r, min(w.g, w.b));
    const float hi2 = tone_lookup(hi, 0, top_encoded);
    const float lo2 = tone_lookup(lo, 0, top_encoded);
    w = hi > lo ? lo2 + (hi2 - lo2) * (w - lo) / (hi - lo) : hi2;
    if (channels)
        w = float3(tone_lookup(w.r, 1, top_encoded), tone_lookup(w.g, 2, top_encoded), tone_lookup(w.b, 3, top_encoded));
    return w;
}

// Mapping a scene to the display (display-only: nothing here reaches what's
// saved), in linear light. Brightness and colour separately:
//  - luminance (Y) through ITU-R BT.2390's EETF, in PQ space: unchanged up
//    to a knee, then a Hermite roll-off putting the image's own peak (as
//    measured, after exposure) on the display's peak; nothing to do when it
//    fits. Chromaticity is kept, then eased towards white near the peak.
//  - chromaticity (u'v') compressed towards the white point, per hue, with
//    ACES's gamut compression curve: from a threshold inside the display's
//    gamut to the furthest the image itself goes in that direction, which
//    lands on the display's edge; nothing to do where the image fits.

static const float3x3 kRgbToXyz709 = float3x3(0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750,
                                              0.0193339, 0.1191920, 0.9503041);
static const float3x3 kXyzToRgb709 = float3x3(3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560,
                                              0.0556434, -0.2040259, 1.0572252);
static const float2 kWhiteUv = float2(0.19783, 0.46832);  // D65
static const float kTwoPi = 6.28318530718;

float pq_from_nits(float nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float y = pow(saturate(nits / 10000), m1);
    return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

float nits_from_pq(float e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float p = pow(saturate(e), 1 / m2);
    return 10000 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1 / m1);
}

// BT.2390's EETF: luminance `y` (1 = SDR white, at `white` nits) from a
// source peaking at `source_peak` to a display peaking at `display_peak`.
float eetf(float y, float source_peak, float display_peak, float white)
{
    if (source_peak <= display_peak || y <= 0)
        return y;
    const float src = pq_from_nits(source_peak * white);
    const float e = pq_from_nits(y * white) / src;  // 0..1 over the source's range
    const float max_lum = pq_from_nits(display_peak * white) / src;
    const float ks = 1.5 * max_lum - 0.5;  // where the roll-off starts
    float out_e = e;
    if (e > ks)
    {
        const float t = (min(e, 1.0) - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
        out_e = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * max_lum;
    }
    return nits_from_pq(out_e * src) / white;
}

float2 uv_from_xyz(float3 xyz)
{
    const float d = xyz.x + 15 * xyz.y + 3 * xyz.z;
    return d > 1e-9 ? float2(4 * xyz.x, 9 * xyz.y) / d : kWhiteUv;
}

float3 xyz_from_yuv(float y, float2 uv)
{
    const float x = y * 9 * uv.x / (4 * uv.y), z = y * (12 - 3 * uv.x - 20 * uv.y) / (4 * uv.y);
    return float3(x, y, z);
}

// How far from white the triangle's edge is in direction `dir` (unit).
float edge_distance(float2 dir, float2 r, float2 g, float2 b)
{
    float best = 1e9;
    const float2 corners[3] = {r, g, b};
    [unroll] for (int i = 0; i < 3; ++i)
    {
        const float2 a = corners[i] - kWhiteUv, e = corners[(i + 1) % 3] - corners[i];
        const float den = dir.x * e.y - dir.y * e.x;
        if (abs(den) < 1e-9)
            continue;
        const float t = (a.x * e.y - a.y * e.x) / den;  // along dir
        const float s = (a.x * dir.y - a.y * dir.x) / den;  // along the edge
        if (t > 0 && s >= -1e-6 && s <= 1 + 1e-6)
            best = min(best, t);
    }
    return best;
}

// ACES's compression of distance `d` (1 = the display's edge): unchanged
// below `threshold`, `limit` onto 1.
float compress(float d, float threshold, float limit)
{
    const float p = 1.2;
    if (d <= threshold || limit <= 1)
        return d;
    const float scale = (limit - threshold) / pow(max(pow((1 - threshold) / (limit - threshold), -p) - 1, 1e-6), 1 / p);
    const float x = (d - threshold) / scale;
    return threshold + scale * x / pow(1 + pow(max(x, 0.0), p), 1 / p);
}

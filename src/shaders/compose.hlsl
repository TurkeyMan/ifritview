// The view's frame: the image (filtered vertically from the first pass's
// rows, or nearest from the image itself), exposed and tone-mapped, over
// the transparency checkerboard or the background, all in linear light,
// then encoded for the front buffer: sRGB (8-bit dithered, or 10-bit), PQ,
// or linear scRGB.
#include "display.hlsli"
#include "resample.hlsli"

cbuffer Params : register(b0)
{
    float4 background;  // linear
    float4 checker_a;   // linear; the two squares
    float4 checker_b;
    float4 image_rect;  // x0, y0, x1, y1 in frame pixels
    int nearest;        // 1: texel fetch from `source` (the image), mapping both axes below
    int filter;         // else: vertical filter over `source` (the first pass's rows)
    float inv;          // source texels per frame pixel, vertically (and horizontally when nearest)
    float offset;       // source position of frame row 0's top edge
    float offset_x;     // nearest: source position of frame column 0's left edge
    int column;         // filtered: frame column -> source column (subtracted)
    int rows;           // the source's rows: taps past them are clamped
    int checker_size;   // device pixels; 0: no checkerboard
    float inv_x;        // nearest: source texels per frame pixel, horizontally
    int columns;        // nearest: the image's width
    float exposure;     // the scene's scale before tone mapping (2^ev; a raw without a curve: its gain too)
    int tone;           // 0: clip; 1: the raw's camera-matched curve (`tone_curve`), by luminance
    int output;         // 0: sRGB, 8-bit, dithered; 1: sRGB, 10-bit; 2: PQ; 3: linear scRGB
    float white;        // SDR white in nits (PQ output, and the luminance roll-off's scale)
    float source_peak;  // the image's peak luminance after exposure (1 = SDR white); 0: no mapping
    float display_peak; // the display's
    int gamut;          // 1: compress chromaticity into the display's gamut, per hue (`extents`)
    float2 display_r;   // the display's primaries in u'v'
    float2 display_g;
    float2 display_b;
    float2 unused;
};

Texture2D<float4> source : register(t0);
Texture2D<float> tone_curve : register(t1);  // 4096 display-linear values for scene luminance 0..1
Texture2D<float> extents : register(t2);     // per hue (72 x 1): how far the image goes from white, 1 = the display's edge

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

// The scene colour as the display shows it (linear, 1 = SDR white).
float3 tone_map(float3 c)
{
    c *= exposure;
    if (tone == 1)
    {
        // The camera's curve maps luminance; the colour scales with it, so hue holds.
        const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
        const float x = saturate(lum) * 4095;
        const int i = int(x);
        const float o = lerp(tone_curve.Load(int3(i, 0, 0)), tone_curve.Load(int3(min(i + 1, 4095), 0, 0)), x - i);
        c *= lum > 1e-6 ? o / lum : 0;
    }
    // BT.709 primaries in, as the image's data is kept; the display's out
    // (the same until there's a display description).
    if (source_peak > 0 || gamut)
    {
        const float3 xyz = mul(kRgbToXyz709, c);
        float y = xyz.y;
        float2 uv = uv_from_xyz(xyz);
        if (source_peak > 0 && tone != 1)  // a raw's camera curve is display-referred already
        {
            const float mapped = eetf(y, source_peak, display_peak, white);
            // Towards white as it nears the display's peak, as film does.
            const float knee = 0.75 * display_peak;
            uv = lerp(uv, kWhiteUv, smoothstep(knee, display_peak, mapped) * 0.6);
            y = mapped;
        }
        if (gamut)
        {
            const float2 v = uv - kWhiteUv;
            const float len = length(v);
            if (len > 1e-6)
            {
                const float2 dir = v / len;
                const float edge = edge_distance(dir, display_r, display_g, display_b);
                const float hue = (atan2(dir.y, dir.x) / kTwoPi + 1) * 72;
                const uint i0 = uint(floor(hue)) % 72u, i1 = (i0 + 1u) % 72u;
                const float limit = lerp(extents.Load(int3(i0, 0, 0)), extents.Load(int3(i1, 0, 0)), frac(hue));
                // A short knee when the image barely leaves the gamut.
                const float threshold = clamp(1 - 0.6 * (limit - 1), 0.8, 0.95);
                uv = kWhiteUv + dir * compress(len / edge, threshold, max(limit, 1.0)) * edge;
            }
        }
        c = mul(kXyzToRgb709, xyz_from_yuv(y, uv));
    }
    return c;
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
    const float3 rgb = c.rgb + (1 - c.a) * under.rgb;
    if (output == 3)
        return float4(rgb, 1);
    if (output == 2)
        return float4(pq_encode(rgb.r * white), pq_encode(rgb.g * white), pq_encode(rgb.b * white), 1);
    float3 e = float3(srgb_encode(rgb.r), srgb_encode(rgb.g), srgb_encode(rgb.b));
    if (output == 0)
        e += dither(p) / 255;
    return float4(e, 1);
}

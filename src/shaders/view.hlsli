// What the view's passes share: the frame's parameters, the image's
// textures and measures, and the mapping of a scene colour to the display
// (in the compose pass, and in the histogram's dispatch so it counts what's
// shown). Display-only: nothing here reaches what's saved.
#include "display.hlsli"

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
    int measured;       // 1: `stats` holds the image's measures (peak, extents): map to the display by them
    float display_peak; // the display's peak luminance (1 = SDR white)
    // (Ordered so nothing straddles a 16-byte row: the C++ side is a plain sequence.)
    float2 display_r;   // the display's primaries in u'v'
    float2 display_g;
    float2 display_b;
    int show_histogram; // 1: the histogram panel, at `panel`
    uint histogram_channels;  // which (histogram.hlsli)
    float4 panel;       // x0, y0, x1, y1 in frame pixels
    float4 input_x;     // rows of the image's data -> XYZ (its colour matrix, a raw's white balance in it)
    float4 input_y;
    float4 input_z;
    int auto_exposure;  // 1: a raw developed on the GPU, its scale from its own 99th percentile (auto_gain)
    int3 unused2;
};

Texture2D<float4> source : register(t0);
Texture2D<float> tone_curve : register(t1);  // 4096 display-linear values for scene luminance 0..1

// The image's measures (stats.hlsl): words, as below.
#ifdef COMPUTE
RWByteAddressBuffer stats : register(u1);
#else
ByteAddressBuffer stats : register(t8);
#endif
static const uint kStatsLuminance = 0;  // 1024 counts: log2 luminance, 2^-20 .. 2^12
static const uint kStatsExtents = 1024; // 72 floats (as uints): how far from white per 5 degrees of hue, 1 = the display's edge
static const uint kStatsCount = 1096;   // pixels counted
static const uint kStatsPeak = 1097;    // float: the 99.9th percentile's luminance (peak.hlsl)
static const uint kStatsMinimum = 1098; // ~float bits of the least luminance above 0 (max of the complement: a min)
static const uint kStatsMaximum = 1099; // float bits of the greatest
static const uint kStatsP99 = 1100;     // float: the 99th percentile's luminance (peak.hlsl), for auto exposure
static const uint kStatsWords = 1101;

// The scene colour as the display shows it (linear, 1 = SDR white).
// The scene's luminance scale for a raw developed on the GPU: its 99th
// percentile at SDR white, as LibRaw's auto-brightening does (the roll-off
// takes the 1% above). 1 for the rest.
float auto_gain()
{
    if (!auto_exposure || !measured)
        return 1;
    const float p99 = asfloat(stats.Load(kStatsP99 * 4));
    return p99 > 0 ? 1 / p99 : 1;
}

// A colour of the image's data (its own space: input_x/y/z take it to XYZ)
// as the display shows it, in the display's linear RGB (BT.709 until
// there's a display description).
float3 tone_map(float3 c)
{
    const float scale = exposure * auto_gain();
    float3 xyz = mul(float3x3(input_x.xyz, input_y.xyz, input_z.xyz), c) * scale;
    if (tone == 1)
    {
        // The camera's curve maps luminance; the colour scales with it, so hue
        // holds. It covers the scene from 0 to 1: pushed past that (exposure
        // above the camera's), the scene is rolled off into it first, as the
        // display mapping would, not clipped at its end.
        const float lum = xyz.y;
        const float peak = measured ? asfloat(stats.Load(kStatsMaximum * 4)) * scale : 0;
        const float fitted = peak > 1 ? eetf(lum, peak, 1, white) : lum;
        const float x = saturate(fitted) * 4095;
        const int i = int(x);
        const float o = lerp(tone_curve.Load(int3(i, 0, 0)), tone_curve.Load(int3(min(i + 1, 4095), 0, 0)), x - i);
        xyz *= lum > 1e-6 ? o / lum : 0;
    }
    if (measured)
    {
        float y = xyz.y;
        float2 uv = uv_from_xyz(xyz);
        // From the brightest it reaches (on the filtered level the measures
        // come from, so a lone hot pixel is already smoothed away): nothing is
        // clipped, and the top keeps some gradation.
        const float source_peak = asfloat(stats.Load(kStatsMaximum * 4)) * scale;
        if (source_peak > display_peak && tone != 1)  // a raw's camera curve is display-referred already
        {
            const float mapped = eetf(y, source_peak, display_peak, white);
            // Towards white as it nears the display's peak, as film does (only
            // when there's something to compress: an image that fits is shown as it is).
            const float knee = 0.75 * display_peak;
            uv = lerp(uv, kWhiteUv, smoothstep(knee, display_peak, mapped) * 0.6);
            y = mapped;
        }
        {
            const float2 v = uv - kWhiteUv;
            const float len = length(v);
            if (len > 1e-6)
            {
                const float2 dir = v / len;
                const float edge = edge_distance(dir, display_r, display_g, display_b);
                const float hue = (atan2(dir.y, dir.x) / kTwoPi + 1) * 72;
                const uint i0 = uint(floor(hue)) % 72u, i1 = (i0 + 1u) % 72u;
                const float limit = lerp(asfloat(stats.Load((kStatsExtents + i0) * 4)),
                                         asfloat(stats.Load((kStatsExtents + i1) * 4)), frac(hue));
                // A short knee when the image barely leaves the gamut.
                const float threshold = clamp(1 - 0.6 * (limit - 1), 0.8, 0.95);
                uv = kWhiteUv + dir * compress(len / edge, threshold, max(limit, 1.0)) * edge;
            }
        }
        xyz = xyz_from_yuv(y, uv);
    }
    return mul(kXyzToRgb709, xyz);
}

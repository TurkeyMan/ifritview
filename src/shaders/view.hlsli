// What the view's passes share: the frame's parameters, the image's
// textures and measures, and a colour of the image as it's shown (in the
// compose pass, and in the histogram's dispatch so it counts what's shown),
// in two stages:
//  - the edit (edit()): the image's data to the edited scene, whatever the
//    source: its white balance and matrix, exposure, the default look (a
//    raw's camera-matched curve) and the develop controls (edit.hlsli);
//  - the display mapping (display.hlsli), after it: to the display; for
//    what's saved (GpuView::export_pixels), SDR's, its peak at SDR white
//    whatever the display.
#include "display.hlsli"
#include "edit.hlsli"
#include "perceived.hlsli"

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
    int output;         // 0: sRGB, 8-bit; 1: sRGB, 10-bit; 2: PQ, 10-bit (all dithered); 3: linear scRGB
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
    int auto_exposure;  // 1: a raw developed on the GPU, its scale its own (auto_gain)
    int gain;           // the difference pass: its errors amplified by this (1..255)
    int adjusting;      // bit 0: the develop controls' tables apply (edit.hlsli, t4 and t5); bit 1: their channel curves too
    float anchor;       // auto_exposure: the camera's own scale, middle grey where its JPEG has it (peak.hlsl)
    float4 to_working_x;  // XYZ to the edit's working space (rows); w of the first: its tone table's top, encoded
    float4 to_working_y;
    float4 to_working_z;
    float4 from_working_x;  // and back
    float4 from_working_y;
    float4 from_working_z;
    float lc_weight;    // 0: kLightnessChromaWeight; else that (a test hook: IFRIT_LC_WEIGHT)
    float min_slope;    // < 0: kMinBrightnessSlope; else that (a test hook: IFRIT_MIN_SLOPE; 0 turns the hold off)
    int hk_probe;       // a test hook (IFRIT_HK_PROBE), for a raw's volume mapping: bit 0, show input L**, output L** and
                        // output C* / 100 as R, G, B; bit 1, hold luminance exactly instead of perceived brightness
};

Texture2D<float4> source : register(t0);
Texture2D<float> tone_curve : register(t1);  // 4096 display-linear values for scene luminance 0..1

// The image's measures (stats.hlsl): words, as below.
#ifdef COMPUTE
RWByteAddressBuffer stats : register(u1);
#else
ByteAddressBuffer stats : register(t8);
#endif
#include "stats_layout.hlsli"

// The scene's luminance scale for a raw developed on the GPU: the camera's
// own, moved by the scene a stop at most (peak.hlsl). 1 for the rest.
float auto_gain()
{
    if (!auto_exposure || !measured)
        return 1;
    const float gain = asfloat(stats.Load(kStatsAutoGain * 4));
    return gain > 0 ? gain : 1;
}

// The edit: a colour of the image's data (its own space: input_x/y/z take
// it to XYZ, a raw's white balance in them) as edited, in XYZ (linear,
// 1 = white), scaled by `scale` (exposure).
float3 edit(float3 c, float scale)
{
    float3 xyz = mul(float3x3(input_x.xyz, input_y.xyz, input_z.xyz), c) * scale;
    if (auto_exposure && tone == 0 && xyz.y > 0)
    {
        // A raw's toe: the darkest tones (the sensor's noise floor: a few
        // hundredths of a percent of white) towards black, as a camera's
        // curve takes them, and their colour towards grey, so the floor's
        // tint and chroma noise don't show in a dark scene. A hundredth of
        // white loses a third; middle grey 3%; hue kept.
        const float y = xyz.y;
        xyz = lerp(y * float3(0.95047, 1, 1.08883), xyz, y / (y + 0.01)) * (y / (y + 0.005));
    }
    if (tone == 1)
    {
        // The camera's curve maps luminance; the colour scales with it, so hue
        // holds. It covers the scene from 0 to 1: pushed past that (exposure
        // above the camera's), the scene is rolled off into it first, not
        // clipped at its end.
        const float lum = xyz.y;
        const float peak = measured ? asfloat(stats.Load(kStatsMaximum * 4)) * scale : 0;
        const float fitted = peak > 1 ? eetf(lum, peak, 1, white) : lum;
        const float x = saturate(fitted) * 4095;
        const int i = int(x);
        const float o = lerp(tone_curve.Load(int3(i, 0, 0)), tone_curve.Load(int3(min(i + 1, 4095), 0, 0)), x - i);
        xyz *= lum > 1e-6 ? o / lum : 0;
    }
    if (adjusting & 1)
    {
        const float3 w = adjust(mul(float3x3(to_working_x.xyz, to_working_y.xyz, to_working_z.xyz), xyz), from_working_y.xyz,
                                to_working_x.w, (adjusting & 2) != 0);
        xyz = mul(float3x3(from_working_x.xyz, from_working_y.xyz, from_working_z.xyz), w);
    }
    return xyz;
}

// The display's gamut is a volume, not only a triangle: a bright colour
// whose chromaticity fits can still need more of a primary than the display
// has (a light blue as bright as a leaf in sun needs more blue than there
// is; blue alone gives 7% of white). Such a colour goes towards white at its
// own luminance and constant Oklab hue (perceived.hlsli's toward_white())
// until every channel fits, and lighter rather than clipped channel by
// channel, which bent its hue and flattened it (#0000FF). Below zero
// likewise.
float3 fit_display(float3 rgb, float y)
{
    return toward_white(rgb, y, display_peak);
}

// The display model. A raw developed here is shown with the least
// perceptual change the display allows: two stated models applied in turn,
// brightness then colour, and one style parameter.
//
// Brightness: a monotone, continuous curve on luminance, by histogram
// adjustment (Ward Larson, Rushmeier and Piatko 1997) above a knee. The
// identity up to a knee, kKnee of the display's peak (lower only when the
// image's span above it is so wide that the floor below couldn't otherwise
// be kept: brightness_curve.hlsli's curve_knee()); above it, in log luminance,
// each band of the image's histogram is given output range in proportion to
// how many pixels it holds, its slope (stops out per stop in) never above 1,
// so no band gets more contrast than the scene had, and never under
// kMinBrightnessSlope, so a sparse range such as a glint still visibly
// brightens. The image's true maximum (its luminance after exposure, white
// balance and the edit, at full resolution, past its few brightest pixels)
// lands exactly on the peak, and the slope is 1 at the knee, falling from it
// over the band kKneeRamp stops above. A scene whose top fits under the peak
// is shown as it is. Since a dense band keeps slope 1 and the compression
// goes where there are few pixels, the knee needs no placing per image to
// suit its content: it's only where the curve may first bend. The colour is scaled with its luminance. The curve's made for
// each exposure, white balance and edit, from the luminance those give
// (volume.hlsl's CURVE), at kCurveBins bins between the knee and the top.
#include "brightness_curve.hlsli"

float brightness(float y)
{
    const float knee = asfloat(stats.Load(kStatsCurve * 4)), top = asfloat(stats.Load((kStatsCurve + 1) * 4));
    if (!(top > knee) || !(y > exp2(knee)))
        return y;
    const float t = (log2(y) - knee) / (top - knee) * float(kCurveBins);
    if (t >= float(kCurveBins))
        return display_peak;
    const int i = int(t);
    const float lo = asfloat(stats.Load((kStatsCurve + 2 + i) * 4)), hi = asfloat(stats.Load((kStatsCurve + 3 + i) * 4));
    return exp2(lerp(lo, hi, t - i));
}

// Oklab, from and to linear BT.709 (L 1 at SDR white).
float3 cbrt3(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3); }
float3 oklab_from_rgb(float3 c)
{
    const float3 lms = cbrt3(mul(float3x3(0.4122214708, 0.5363325363, 0.0514459929, 0.2119034982, 0.6806995451, 0.1073969566,
                                          0.0883024619, 0.2817188376, 0.6299787005), c));
    return mul(float3x3(0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050, 0.4505937099,
                        0.0259040371, 0.7827717662, -0.8086757660), lms);
}
float3 rgb_from_oklab(float3 o)
{
    const float3 lms = mul(float3x3(1, 0.3963377774, 0.2158037573, 1, -0.1055613458, -0.0638541728, 1, -0.0894841775,
                                    -1.2914855480), o);
    return mul(float3x3(4.0767416621, -3.3077115913, 0.2309699292, -1.2684380046, 2.6097574011, -0.3413193965,
                        -0.0041960863, -0.7034186147, 1.7076147010), lms * lms * lms);
}

// Colour: the display's volume is computed in linear light at its peak
// (display_peak, BT.709 primaries) and expressed in Oklab; its boundary, the
// most chroma at a hue and lightness, is found exactly for each pixel
// (max_chroma()), never interpolated. A colour inside the volume is
// shown unchanged. One outside it is moved inside by the least perceptual change:
// at constant hue, along the line to its nearest boundary point under the
// weighted distance kLightnessChromaWeight * dL^2 + dC^2 (nearest_edge()).
// The projection is image-adaptive: the image volume (volume.hlsl) is a
// table over hue and lightness of how far the image's colours lie along
// those lines against the boundary (1 on it); the image's extent in each
// direction is mapped exactly onto the boundary along display.hlsli's
// compress() curve, and the colours short of it in proportion, so in a
// direction where the image fits, nothing moves at all.
//
// Brightness order is held across hues, in perceived brightness: the named
// model is Fairchild and Pirrotta's Helmholtz-Kohlrausch-corrected lightness
// L** (perceived.hlsli), under which a saturated colour looks brighter than
// a grey of the same luminance. A pixel comes out with exactly the L** a
// grey of the same input L** comes out at (the brightness curve's output
// for that grey: match_perceived()): output L** is one rising function of
// input L**, whatever the hue, so a pixel that looks brighter in the scene
// never looks darker than a dimmer one of any hue, and a near-neutral rim
// round a saturated core never outshines it. The projection chooses the
// chroma; the luminance it's shown at is then whatever gives that L** (less
// than a grey's by the chroma's HK boost), with what the display can't show
// there going towards white at constant hue. Over SDR white (L* > 100) the
// boost is none, so there L** is L*.
//
// The style parameter, kLightnessChromaWeight: what a unit of Oklab
// lightness change costs against a unit of chroma change in the
// projection, which decides how much chroma survives at the matched L**. Large keeps brightness and gives up colour (film-like: a bright
// saturated colour fades towards white at its own lightness); small keeps
// colour and changes lightness (vivid: it darkens to stay saturated, as far
// as the bound lets it).
static const float kLightnessChromaWeight = 4.0;

float lightness_chroma_weight()
{
    return lc_weight > 0 ? lc_weight : kLightnessChromaWeight;
}
static const uint kVolumeHues = 72, kVolumeSlices = 32;

// The display's boundary, exact: its most chroma at Oklab lightness L in
// direction `dir` (unit a, b), at display_peak. Along the ray each channel is
// a cubic in chroma; the first to leave 0..display_peak is found by halving
// the interval 20 times.
float max_chroma(float L, float2 dir)
{
    const float3 k = float3(0.3963377774 * dir.x + 0.2158037573 * dir.y, -0.1055613458 * dir.x - 0.0638541728 * dir.y,
                            -0.0894841775 * dir.x - 1.2914855480 * dir.y);
    const float3x3 m = float3x3(4.0767416621, -3.3077115913, 0.2309699292, -1.2684380046, 2.6097574011, -0.3413193965,
                                -0.0041960863, -0.7034186147, 1.7076147010);
    if (L <= 0 || L * L * L >= display_peak)
        return 0;
    float lo = 0, hi = 0.6 * pow(display_peak, 1.0 / 3);
    [loop] for (int i = 0; i < 20; ++i)
    {
        const float mid = 0.5 * (lo + hi);
        const float3 lms = L + mid * k;
        const float3 rgb = mul(m, lms * lms * lms);
        if (all(rgb >= 0) && all(rgb <= display_peak))
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

// The same, from a table made once for the display (GpuView::boundary(), the
// same exact boundary at 720 hues by 256 lightnesses from black to white),
// read bilinearly, round in hue: only to find where a colour's nearest
// boundary point is (nearest_edge()) and whether it's near the boundary at
// all, never for the boundary itself. `u`: the hue's column
// (boundary_column()).
Texture2D<float> display_boundary : register(t3);
static const uint kBoundaryHues = 720, kBoundaryLevels = 256;

float table_chroma(float L, float u)
{
    const float v = L / pow(display_peak, 1.0 / 3) * (kBoundaryLevels - 1);
    if (v <= 0 || v >= kBoundaryLevels - 1)
        return 0;
    const int u0 = int(floor(u)) % int(kBoundaryHues), v0 = int(floor(v));
    const int u1 = (u0 + 1) % int(kBoundaryHues);
    const float fu = frac(u), fv = v - v0;
    const float lo = lerp(display_boundary.Load(int3(u0, v0, 0)), display_boundary.Load(int3(u1, v0, 0)), fu);
    const float hi = lerp(display_boundary.Load(int3(u0, v0 + 1, 0)), display_boundary.Load(int3(u1, v0 + 1, 0)), fu);
    return lerp(lo, hi, fv);
}

float boundary_column(float2 dir)
{
    return frac(atan2(dir.y, dir.x) / kTwoPi + 1) * kBoundaryHues;
}

// The display's white's Oklab lightness.
float white_l()
{
    return pow(display_peak, 1.0 / 3);
}

// The boundary point nearest the colour (L, c) at its hue under the weighted
// distance: sampled along the boundary, then narrowed (golden section).
float nearest_edge(float L, float c, float u)
{
    const float top = white_l(), w = lightness_chroma_weight();
    float best = 0, best_d = 1e30;
    [loop] for (int i = 0; i <= 16; ++i)
    {
        const float l = top * i / 16, cb = table_chroma(l, u);
        const float d = w * (L - l) * (L - l) + (c - cb) * (c - cb);
        if (d < best_d)
            best = l, best_d = d;
    }
    float lo = max(best - top / 16, 0.0), hi = min(best + top / 16, top);
    [loop] for (int j = 0; j < 12; ++j)
    {
        const float m1 = lo + 0.382 * (hi - lo), m2 = lo + 0.618 * (hi - lo);
        const float c1 = table_chroma(m1, u), c2 = table_chroma(m2, u);
        const float d1 = w * (L - m1) * (L - m1) + (c - c1) * (c - c1);
        const float d2 = w * (L - m2) * (L - m2) + (c - c2) * (c - c2);
        if (d1 < d2)
            hi = m2;
        else
            lo = m1;
    }
    const float l = 0.5 * (lo + hi);
    return l;
}

// A colour's line: from its point on the grey axis (`from`, the lightness
// where the line through the colour and its nearest boundary point meets
// it) through the colour; returns where the colour lies on it, 1 on the
// boundary. A colour under half way to the boundary at its own lightness is
// far inside it, and nothing that far in ever moves: its line is taken at
// its own lightness, its place read from the table, without the search.
float edge_x(float L, float c, float2 dir, out float from)
{
    from = L;
    const float u = boundary_column(dir);
    const float near = table_chroma(L, u);
    if (c < 0.5 * near)
        return c / max(near, 1e-6);
    const float l = nearest_edge(L, c, u), edge = max_chroma(l, dir);
    if (abs(c - edge) > 1e-4)
        from = clamp(L - c * (L - l) / (c - edge), 0.0, white_l());
    return c / max(edge, 1e-6);
}

// The measure pass runs tone_map() only as far as each colour's place in the
// table, into these.
static bool g_measuring = false;
static float g_volume_x = 0, g_volume_hue = 0, g_volume_l = 0;

// The table's cell for Oklab hue h and lightness L (L from 0 to the peak's).
uint2 volume_cell(float h, float L)
{
    const uint hue = uint(frac(h / kTwoPi + 1) * kVolumeHues) % kVolumeHues;
    const uint slice = uint(clamp(L / pow(display_peak, 1.0 / 3) * kVolumeSlices, 0.0, kVolumeSlices - 1.0));
    return uint2(hue, slice);
}

// The prepared table (volume.hlsl's PREPARE), bilinear between cell centres,
// round in hue.
float volume_limit(float h, float L)
{
    const float u = frac(h / kTwoPi + 1) * kVolumeHues - 0.5, v = clamp(L / pow(display_peak, 1.0 / 3) * kVolumeSlices - 0.5, 0.0, kVolumeSlices - 1.0);
    const int u0 = int(floor(u)), v0 = int(floor(v));
    const uint h0 = uint(u0 + int(kVolumeHues)) % kVolumeHues, h1 = (h0 + 1) % kVolumeHues;
    const uint s0 = uint(v0), s1 = min(s0 + 1, kVolumeSlices - 1);
    const float fu = u - u0, fv = v - v0;
    const float a = lerp(asfloat(stats.Load((kStatsVolume + h0 * kVolumeSlices + s0) * 4)), asfloat(stats.Load((kStatsVolume + h1 * kVolumeSlices + s0) * 4)), fu);
    const float b = lerp(asfloat(stats.Load((kStatsVolume + h0 * kVolumeSlices + s1) * 4)), asfloat(stats.Load((kStatsVolume + h1 * kVolumeSlices + s1) * 4)), fu);
    return lerp(a, b, fv);
}

// Where a colour at `x` on its line goes: compress() with the image's extent
// there (`limit`) mapped exactly onto the boundary, the identity where the
// image fits (limit 1 or less); a colour past the measure (taken on a
// smaller level) onto the boundary.
float fit_x(float x, float limit)
{
    const float r = limit > 1 ? compress(x, clamp(1 - 0.6 * (limit - 1), 0.8, 0.95), limit) : x;
    return min(r, 1.0);
}

// `o` fitted with the image's extent `limit` in its direction.
float3 fit_with(float3 o, float limit)
{
    const float c = length(o.yz);
    if (c < 1e-5)
        return o;
    const float2 dir = o.yz / c;
    float from;
    const float x = edge_x(o.x, c, dir, from);
    const float r = fit_x(x, limit);
    if (r >= x)
        return o;
    const float t = r / x;  // along the line, from its grey point
    return float3(from + t * (o.x - from), dir * (c * t));
}

// The least slope of brightness out against brightness in, in log terms
// (stops out per stop in; for Oklab lightness, a power of luminance, the
// same): the brightness curve's in its sparsest band (brightness()), and the
// colour fit's up each hue (volume_floor()).
static const float kMinBrightnessSlope = 0.25;

// The least lightness a colour at hue h and lightness L may come out at, so
// at each hue a brighter colour always comes out brighter than a dimmer one,
// its log lightness rising at least least_slope() of the input's (volume.hlsl's
// PREPARE), bilinear like volume_limit().

float least_slope()
{
    return min_slope < 0 ? kMinBrightnessSlope : min_slope;
}
float volume_floor(float h, float L)
{
    const float u = frac(h / kTwoPi + 1) * kVolumeHues - 0.5, v = clamp(L / pow(display_peak, 1.0 / 3) * kVolumeSlices - 0.5, 0.0, kVolumeSlices - 1.0);
    const int u0 = int(floor(u)), v0 = int(floor(v));
    const uint h0 = uint(u0 + int(kVolumeHues)) % kVolumeHues, h1 = (h0 + 1) % kVolumeHues;
    const uint s0 = uint(v0), s1 = min(s0 + 1, kVolumeSlices - 1);
    const float fu = u - u0, fv = v - v0;
    const float a = lerp(asfloat(stats.Load((kStatsFloor + h0 * kVolumeSlices + s0) * 4)), asfloat(stats.Load((kStatsFloor + h1 * kVolumeSlices + s0) * 4)), fu);
    const float b = lerp(asfloat(stats.Load((kStatsFloor + h0 * kVolumeSlices + s1) * 4)), asfloat(stats.Load((kStatsFloor + h1 * kVolumeSlices + s1) * 4)), fu);
    return lerp(a, b, fv);
}

float3 fit_volume(float3 o)
{
    const float c = length(o.yz);
    if (c < 1e-5)
        return o;
    const float2 dir = o.yz / c;
    const float h = atan2(dir.y, dir.x), limit = volume_limit(h, o.x);
    float3 r = fit_with(o, limit);
    if (least_slope() > 0)
    {
        // Not below the floor a dimmer colour at this hue sets (volume.hlsl's
        // PREPARE): held at that lightness, its chroma fitted there.
        const float least = min(volume_floor(h, o.x), o.x);
        if (r.x < least)
        {
            const float x = c / max(max_chroma(least, dir), 1e-6);
            r = float3(least, dir * (c * min(fit_x(x, limit) / x, 1.0)));
        }
    }
    return r;
}

// A colour of the image's data as the display shows it (the edit, then the
// display mapping), in the display's linear RGB (BT.709 until there's a
// display description).
float3 tone_map(float3 c)
{
    const float scale = exposure * auto_gain();
    float3 xyz = edit(c, scale);
    const float3 scene = xyz;  // as edited, before the display mapping
    if (measured)
    {
        float y = xyz.y;
        float2 uv = uv_from_xyz(xyz);
        const float2 uv0 = uv;  // the chromaticity before it's taken towards white
        // From the brightest it reaches (on the filtered level the measures
        // come from, so a lone hot pixel is already smoothed away), as the
        // edit's tone curve takes it: nothing is clipped, and the top keeps
        // some gradation.
        float source_peak = asfloat(stats.Load(kStatsMaximum * 4)) * scale;
        if (adjusting & 1)
            source_peak = tone_lookup(source_peak, 0, to_working_x.w);
        bool volume = false;   // a raw's: its colours fitted to the display's volume (fit_volume())
        if (auto_exposure)
        {
            // A raw developed here: its brightness curve, its top onto the peak.
            if (y > 0)
            {
                y = brightness(y);
                volume = true;
            }
        }
        else if (source_peak > display_peak && tone != 1)  // a raw's camera curve is display-referred already
        {
            const float mapped = eetf(y, source_peak, display_peak, white);
            // Towards white as it nears the display's peak, as film does (only
            // when there's something to compress: an image that fits is shown as it is).
            const float knee = 0.75 * display_peak;
            uv = lerp(uv, kWhiteUv, smoothstep(knee, display_peak, mapped) * 0.6);
            y = mapped;
        }
        if (!volume)  // the rest: chromaticity compressed towards white per hue, to the image's extent
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
        if (!volume)
        {
            // How far that took it towards white, taken at constant Oklab hue
            // (a straight line in chromaticity, as in linear RGB, drifts in
            // hue: the Abney effect): its Oklab chroma scaled by as much, at
            // luminance y.
            const float was = length(uv0 - kWhiteUv), now = length(uv - kWhiteUv);
            if (was > 1e-6 && now < was)
            {
                const float3 o = ok_from_rgb709(mul(kXyzToRgb709, xyz_from_yuv(y, uv0)));
                return fit_display(at_luminance(rgb709_from_ok(float3(o.x, o.yz * (now / was))), y), y);
            }
        }
        xyz = xyz_from_yuv(y, uv);
        if (volume)
        {
            const float3 o = oklab_from_rgb(mul(kXyzToRgb709, xyz));
            if (g_measuring)
            {
                const float c = length(o.yz);
                float from;
                g_volume_x = c < 1e-5 ? 0 : edge_x(o.x, c, o.yz / c, from);
                g_volume_hue = atan2(o.z, o.y);
                g_volume_l = o.x;
                return 0;
            }
            // Brightness order across hues, in perceived brightness
            // (perceived.hlsli's L**): the fitted colour exactly as bright
            // as a grey of the same input L** comes out (match_perceived()).
            const float3 rgb = rgb_from_oklab(fit_volume(o));
            const float in_l = perceived_lightness(scene);
            float3 shown;
            if (hk_probe & 2)
            {
                // (A test hook: luminance held exactly, the colour's chromaticity kept.)
                const float fitted = dot(kRgbToXyz709[1], rgb);
                shown = fit_display(fitted > 1e-9 ? rgb * (xyz.y / fitted) : xyz.y, xyz.y);
            }
            else
                shown = match_perceived(rgb, lightness_of(brightness(cie_y(in_l))), display_peak);
            if (hk_probe & 1)
            {
                const float3 lab = cie_lab(xyz_of_rgb709(shown));
                return float3(in_l, perceived_lightness(xyz_of_rgb709(shown)), length(lab.yz)) / 100;
            }
            return shown;
        }
    }
    return fit_display(mul(kXyzToRgb709, xyz), xyz.y);
}

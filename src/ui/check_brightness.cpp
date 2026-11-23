// --check-brightness: the brightness curve's shape (shaders/brightness_curve.hlsli
// and brightness_shape.hlsli, the code volume.hlsl's CURVE runs), on the CPU
// over made-up histograms: at every bin its slope at least its floor
// (kMinBrightnessSlope, 1 at the knee and falling to it over kKneeRamp stops)
// and at most 1, the top exactly on the peak, the knee at most kKnee of the
// peak and only as low as the floors need; for tops from just over the peak
// to 20 stops past it, at three least slopes and five shapes of histogram.
// And perceived brightness matched (shaders/perceived.hlsli): a colour,
// however the fit darkened or desaturated it, comes out with exactly the L**
// a grey of the same input L** does, so across hues output order is input
// order.
// And the way to white at constant hue: an orange-to-white ramp, its Oklab
// hue as shown within 2 degrees of the orange's while it has any chroma.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace curve
{
using uint = unsigned;
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
using std::log2;
#include "../shaders/brightness_curve.hlsli"
float g_count[kCurveBins], g_floor[kCurveBins];
#define CURVE_LOOP
#include "../shaders/brightness_shape.hlsli"
#undef CURVE_LOOP
}  // namespace curve

namespace perceived
{
struct float3
{
    float x, y, z;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float3 operator*(float3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float3 operator/(float3 a, float k) { return {a.x / k, a.y / k, a.z / k}; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
using curve::min;
using curve::max;
using std::abs;
using std::sqrt;
using std::sin;
using std::atan2;
using std::exp2;
using std::log2;
#include "../shaders/perceived.hlsli"
}  // namespace perceived

// Perceived brightness matched (shaders/perceived.hlsli's match_perceived()):
// every hue, four saturations, input luminances from dark to 4 x the peak,
// the fit's result darkened to as little as a tenth and desaturated, through
// a brightness curve that's the identity to half the peak and compresses a
// third above, on an SDR display and on an HDR one of 4 x SDR white. Each
// comes out inside the display with exactly the L** a grey of the same input
// L** does; and across every pair of them, of any two hues, one brighter in
// the scene (L**) never comes out darker than the other (the inversions a
// bound alone allowed: 103.71 in out at 91.16, 102.15 at 97.17; 98.10 at
// 88.92, 97.02 at 90.23), checked too at exactly those inputs across hues.
static int check_perceived()
{
    using namespace perceived;
    int failures = 0, cases = 0, pairs_bad = 0;
    float worst = 0, worst_pair = 0;
    for (float peak : {1.0f, 4.0f})
    {
        const float knee = 0.5f * peak;
        const auto curve = [&](float y) { return y <= knee ? y : std::min(peak, knee * std::exp2(std::log2(y / knee) / 3)); };
        std::vector<std::pair<float, float>> seen;  // input L**, output L**
        for (int hue = 0; hue < 36; ++hue)
            for (float sat : {0.3f, 0.6f, 0.9f, 1.4f})
                for (float scale : {0.02f, 0.1f, 0.3f, 0.6f, 0.8f, 1.0f, 1.2f, 2.0f, 4.0f})
                    for (float dark : {1.0f, 0.7f, 0.4f, 0.1f})
                        for (float grey : {0.0f, 0.5f})
                        {
                            const float h = 6.2831853f * hue / 36;
                            const float3 scene = float3(std::max(0.0f, 1 + sat * std::cos(h)), std::max(0.0f, 1 + sat * std::cos(h - 2.0943951f)),
                                                        std::max(0.0f, 1 + sat * std::cos(h + 2.0943951f))) * (scale * peak);
                            const float y = dot(float3(0.2126729f, 0.7151522f, 0.0721750f), scene);
                            if (!(y > 0))
                                continue;
                            const float in_l = perceived_lightness(xyz_of_rgb709(scene));
                            const float target = lightness_of(curve(cie_y(in_l)));  // the grey's output L**
                            float3 fit = scene * (curve(y) / y * dark);
                            fit = fit + (float3(1, 1, 1) * dot(float3(0.2126729f, 0.7151522f, 0.0721750f), fit) - fit) * grey;
                            const float3 out = match_perceived(fit, target, peak);
                            const float out_l = perceived_lightness(xyz_of_rgb709(out));
                            ++cases;
                            seen.push_back({in_l, out_l});
                            worst = std::max(worst, std::abs(out_l - target));
                            const bool fits = std::min(out.x, std::min(out.y, out.z)) >= -1e-4f && std::max(out.x, std::max(out.y, out.z)) <= peak + 1e-4f;
                            if (!(std::abs(out_l - target) <= 0.02f) || !fits)
                            {
                                if (++failures <= 10)
                                    std::fprintf(stderr, "check-brightness: perceived (peak %g, hue %d, sat %g, scale %g, dark %g): out L** %.3f, the grey's %.3f%s\n", peak,
                                                 hue * 10, sat, scale, dark, out_l, target, fits ? "" : ", or past the display");
                            }
                        }
        // Codex's pairs, from a generated DNG: at any hues, input L** 103.71
        // came out under 102.15, and 98.10 under 97.02. Every colour of the
        // grid at the brighter input against every one at the dimmer.
        for (const auto& pair : {std::pair<float, float>{103.71f, 102.15f}, std::pair<float, float>{98.10f, 97.02f}})
        {
            float lowest_hi = 1e9f, highest_lo = -1e9f;
            for (int hue = 0; hue < 36; ++hue)
                for (float sat : {0.3f, 0.6f, 0.9f, 1.4f})
                    for (float dark : {1.0f, 0.7f, 0.4f, 0.1f})
                        for (float grey : {0.0f, 0.5f})
                            for (float in_l : {pair.first, pair.second})
                            {
                                const float h = 6.2831853f * hue / 36;
                                const float3 base = float3(std::max(0.0f, 1 + sat * std::cos(h)), std::max(0.0f, 1 + sat * std::cos(h - 2.0943951f)),
                                                           std::max(0.0f, 1 + sat * std::cos(h + 2.0943951f)));
                                float lo = 0, hi = 64;  // the scale giving that input L**
                                for (int i = 0; i < 40; ++i)
                                    (perceived_lightness(xyz_of_rgb709(base * (0.5f * (lo + hi)))) < in_l ? lo : hi) = 0.5f * (lo + hi);
                                const float3 scene = base * hi;
                                const float y = dot(float3(0.2126729f, 0.7151522f, 0.0721750f), scene);
                                float3 fit = scene * (curve(y) / y * dark);
                                fit = fit + (float3(1, 1, 1) * dot(float3(0.2126729f, 0.7151522f, 0.0721750f), fit) - fit) * grey;
                                const float out_l = perceived_lightness(xyz_of_rgb709(match_perceived(fit, lightness_of(curve(cie_y(in_l))), peak)));
                                if (in_l == pair.first)
                                    lowest_hi = std::min(lowest_hi, out_l);
                                else
                                    highest_lo = std::max(highest_lo, out_l);
                            }
            worst_pair = std::max(worst_pair, highest_lo - lowest_hi);
            if (highest_lo > lowest_hi + 0.02f && ++pairs_bad <= 5)
                std::fprintf(stderr, "check-brightness: perceived order (peak %g): in L** %.2f out as low as %.2f, under %.2f in at %.2f\n", peak, pair.first,
                             lowest_hi, pair.second, highest_lo);
        }
        // Every pair: sorted by input L**, each output no lower than the
        // highest output of any input strictly dimmer.
        std::sort(seen.begin(), seen.end());
        float dimmer_top = -1e9f;
        for (size_t i = 0, j = 0; i < seen.size(); ++i)
        {
            for (; j < i && seen[j].first < seen[i].first - 1e-4f; ++j)
                dimmer_top = std::max(dimmer_top, seen[j].second);
            const float by = dimmer_top - seen[i].second;
            worst_pair = std::max(worst_pair, by);
            if (by > 0.02f && ++pairs_bad <= 5)
                std::fprintf(stderr, "check-brightness: perceived order (peak %g): in L** %.2f out %.2f, under a dimmer one's %.2f\n", peak, seen[i].first,
                             seen[i].second, dimmer_top);
        }
    }
    failures += pairs_bad;
    std::printf("perceived: %s, %d cases, output L** off the grey's by at most %.4f, across pairs darker than a dimmer input by at most %.4f\n",
                failures ? "FAILED" : "ok", cases, worst, std::max(worst_pair, 0.0f));
    return failures ? 1 : 0;
}

// An orange's luminance ramped from 0.2 to the peak, as the display stage
// shows it (toward_white()): its Oklab hue within 2 degrees of the orange's
// until its chroma is near none; beside it, the hue a straight line to white
// in linear RGB gave (what b1c6662..af24613 did).
static int check_hue_ramp()
{
    using namespace perceived;
    const float peak = 1;
    const float3 orange = float3(1.0f, 0.42f, 0.08f);  // a candle's
    const float3 o0 = ok_from_rgb709(orange);
    const float h0 = std::atan2(o0.z, o0.y) * 57.29578f;
    int failures = 0;
    float worst = 0, worst_linear = 0;
    std::printf("hue ramp: orange Oklab hue %.1f deg; luminance, chroma, hue error (constant hue | linear RGB)\n", h0);
    for (int i = 0; i <= 80; ++i)
    {
        const float y = 0.2f + (peak - 0.2f) * i / 80;
        const float3 rgb = orange * (y / dot(float3(0.2126729f, 0.7151522f, 0.0721750f), orange));
        const float3 shown = toward_white(rgb, y, peak);
        // The straight line in linear RGB.
        const float top = std::max(rgb.x, std::max(rgb.y, rgb.z));
        const float t = top > peak ? (peak - y) / (top - y) : 1;
        const float3 linear = float3(y, y, y) + (rgb - float3(y, y, y)) * t;
        const float3 o = ok_from_rgb709(shown), ol = ok_from_rgb709(linear);
        const float c = std::sqrt(o.y * o.y + o.z * o.z), cl = std::sqrt(ol.y * ol.y + ol.z * ol.z);
        const auto off = [&](float3 v) { return std::remainder(std::atan2(v.z, v.y) * 57.29578f - h0, 360.0f); };
        if (c > 0.005f)
            worst = std::max(worst, std::abs(off(o)));
        if (cl > 0.005f)
            worst_linear = std::max(worst_linear, std::abs(off(ol)));
        if (c > 0.005f && std::abs(off(o)) > 2)
            ++failures;
        if (!fits_display(shown, peak + 1e-4f) || std::abs(dot(float3(0.2126729f, 0.7151522f, 0.0721750f), shown) - y) > 1e-3f * y)
            ++failures;
        if (i % 8 == 0)
            std::printf("  %.2f  %.3f  %+6.2f | %+6.2f\n", y, c, c > 0.005f ? off(o) : 0.0f, cl > 0.005f ? off(ol) : 0.0f);
    }
    std::printf("hue ramp: %s, worst hue error %.2f deg (linear RGB: %.2f)\n", failures ? "FAILED" : "ok", worst, worst_linear);
    return failures ? 1 : 0;
}

int check_brightness()
{
    using namespace curve;
    int failures = 0;
    const auto fail = [&](const char* what, float top, float m, int shape, float value)
    {
        if (++failures <= 20)
            std::fprintf(stderr, "check-brightness: %s (top %+.2f stops, least %.2f, shape %d): %g\n", what, top, m, shape, value);
    };
    const float tops[] = {0.01f, 0.5f, 1, 2, 3, 5, 8, 11, 12, 16, 20};
    const float leasts[] = {0, 0.25f, 0.5f};
    const float peaks[] = {0, 2};  // log2: SDR, an HDR display 4 x SDR white
    float lowest = 2;
    for (float peak : peaks)
        for (float above : tops)
            for (float m : leasts)
                for (int shape = 0; shape < 5; ++shape)
                {
                    const float top = peak + above, knee = curve_knee(top, peak, m);
                    if (!(knee <= peak + std::log2(kKnee) + 1e-6f))
                        fail("knee over kKnee of the peak", above, m, shape, knee - peak);
                    const float width = (top - knee) / kCurveBins, range = peak - knee;
                    // Only as low as it must be: a little higher, the floors wouldn't fit.
                    if (knee < peak + std::log2(kKnee) - 1e-4f)
                    {
                        const float higher = knee + 0.01f, w = (top - higher) / kCurveBins;
                        float floors = 0;
                        for (uint f = 0; f < kCurveBins; ++f)
                            floors += curve_floor(f, w, m) * w;
                        if (!(floors > peak - higher))
                            fail("knee lower than the floors need", above, m, shape, knee - peak);
                    }
                    for (uint i = 0; i < kCurveBins; ++i)
                    {
                        const float x = (i + 0.5f) / kCurveBins;
                        g_count[i] = shape == 0   ? 1000 * std::exp(-12 * x)        // dense near the knee, thinning
                                     : shape == 1 ? 100                              // even
                                     : shape == 2 ? (i == kCurveBins - 1 ? 50 : 0)   // all at the top
                                     : shape == 3 ? 0                                // none (a top from few pixels)
                                                  : (i < 8 ? 5000.0f : i % 17 == 0 ? 3 : 0);  // a dense foot, sparse glints
                    }
                    curve_shape(knee, top, peak, m);
                    float sum = 0;
                    for (uint i = 0; i < kCurveBins; ++i)
                    {
                        const float slope = g_count[i] / width;
                        sum += g_count[i];
                        if (m == 0.25f)
                            lowest = std::min(lowest, slope);
                        if (!(slope >= curve_floor(i, width, m) - 1e-3f))
                            fail("slope under its floor", above, m, shape, slope);
                        if (!(slope >= curve_least(m) - 1e-3f))
                            fail("slope under the least", above, m, shape, slope);
                        if (!(slope <= 1 + 1e-3f))
                            fail("slope over 1", above, m, shape, slope);
                    }
                    if (!(std::fabs(sum - range) <= 1e-4f * std::max(range, 1.0f)))
                        fail("top not on the peak (stops off)", above, m, shape, sum - range);
                    if (!(g_count[0] / width >= 1 - 1e-3f))
                        fail("slope at the knee not 1", above, m, shape, g_count[0] / width);
                }
    // The two Codex found rescaled at b1c6662: 8 and 256 x the peak.
    for (float above : {3.0f, 8.0f})
    {
        const float knee = curve_knee(above, 0, 0.25f);
        std::printf("brightness: top %g x peak: knee %.3f x peak\n", std::exp2(above), std::exp2(knee));
    }
    std::printf("brightness: %s, least slope seen at 0.25: %.4f\n", failures ? "FAILED" : "ok", lowest);
    return (failures ? 1 : 0) | check_perceived() | check_hue_ramp();
}

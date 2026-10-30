#include "adjust.h"

#include "color_space.h"
#include "decoders/texel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>

std::vector<CrsField> crs_fields(Adjustments& a)
{
    std::vector<CrsField> f = {
        {"Contrast2012", &a.contrast, 2012},
        {"Highlights2012", &a.highlights, 2012},
        {"Shadows2012", &a.shadows, 2012},
        {"Whites2012", &a.whites, 2012},
        {"Blacks2012", &a.blacks, 2012},
        {"Brightness", &a.brightness, 2010},
        {"Contrast", &a.contrast2010, 2010},
        {"HighlightRecovery", &a.recovery, 2010, false},
        {"FillLight", &a.fill_light, 2010, false},
        {"Shadows", &a.blacks2010, 2010, false},
        {"Vibrance", &a.vibrance, 0},
        {"Saturation", &a.saturation, 0},
        {"ParametricShadows", &a.parametric[0], 0},
        {"ParametricDarks", &a.parametric[1], 0},
        {"ParametricLights", &a.parametric[2], 0},
        {"ParametricHighlights", &a.parametric[3], 0},
        {"ParametricShadowSplit", &a.splits[0], 0, false},
        {"ParametricMidtoneSplit", &a.splits[1], 0, false},
        {"ParametricHighlightSplit", &a.splits[2], 0, false},
    };
    static const char* const kBands[8] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
    for (int b = 0; b < 8; ++b)
    {
        f.push_back({std::string("HueAdjustment") + kBands[b], &a.hue[b], 0});
        f.push_back({std::string("SaturationAdjustment") + kBands[b], &a.sat[b], 0});
        f.push_back({std::string("LuminanceAdjustment") + kBands[b], &a.lum[b], 0});
    }
    return f;
}

std::vector<CrsCurve> crs_curves(Adjustments& a)
{
    if (a.legacy)
        return {{"ToneCurve", &a.curve}};
    return {{"ToneCurvePV2012", &a.curve}, {"ToneCurvePV2012Red", &a.red}, {"ToneCurvePV2012Green", &a.green}, {"ToneCurvePV2012Blue", &a.blue}};
}

const char* crs_curve_name(const Adjustments& a)
{
    return a.legacy ? "ToneCurveName" : "ToneCurveName2012";
}

namespace
{

// The presets ToneCurveName names, for an XMP that names one without its points.
Adjustments::Curve named_curve(const std::string& name)
{
    if (name == "Medium Contrast")
        return {{0, 0}, {32, 22}, {64, 56}, {128, 128}, {192, 196}, {255, 255}};
    if (name == "Strong Contrast")
        return {{0, 0}, {32, 16}, {64, 50}, {128, 128}, {192, 202}, {255, 255}};
    return {};
}

}  // namespace

bool Adjustments::neutral() const
{
    // What the process applies, against the defaults: the other process's
    // settings, the curve's name, and what's kept don't count, and a curve
    // of the two corners is linear, as no curve is.
    Adjustments mine = *this, none;
    none.legacy = legacy;
    none.curve_name = curve_name;
    none.kept = kept;
    for (const CrsField& f : crs_fields(mine))
        if (f.process && f.process != (legacy ? 2010 : 2012))
            *f.value = 0;
    if (curve.empty() && !named_curve(curve_name).empty())
        return false;
    const auto linear = [](const Curve& c) { return c.empty() || c == Curve{{0, 0}, {255, 255}}; };
    if (!linear(curve) || !linear(red) || !linear(green) || !linear(blue))
        return false;
    none.curve = curve, none.red = red, none.green = green, none.blue = blue;
    return mine == none;
}

namespace
{

using Map = std::function<double(double)>;

// The perceptual axis the tone controls are defined on: sRGB's encoding,
// carried on past white.
double encode(double x)
{
    return srgb_encode(x);
}
double decode(double g)
{
    return srgb_decode(g);
}

// Mid-grey, where contrast pivots.
const double kGrey = encode(0.18);

// What `f` (increasing) takes to `y`: each control's opposite direction is
// the inverse of its own, so -n undoes +n and both stay monotonic.
double inverse(const Map& f, double y)
{
    double lo = 0, hi = 1;
    while (f(hi) < y && hi < 1e3)
        hi *= 2;
    if (f(lo) >= y)
        return lo;
    for (int i = 0; i < 50; ++i)
    {
        const double mid = (lo + hi) / 2;
        (f(mid) < y ? lo : hi) = mid;
    }
    return (lo + hi) / 2;
}

// `amount` in -1..1 of a control whose positive half is `f(g, a)` for a in
// 0..1; the negative half its inverse.
double either_way(double g, double amount, double (*f)(double, double))
{
    if (amount == 0)
        return g;
    const double a = std::min(std::fabs(amount), 1.0);
    if (amount > 0)
        return f(g, a);
    return inverse([&](double x) { return f(x, a); }, g);
}

// A smooth hinge: 0 below 0, a quadratic over `w`, then rising at slope 1.
double hinge(double u, double w)
{
    return u <= 0 ? 0 : u < w ? u * u / (2 * w) : u - w / 2;
}

// Contrast: an S about mid-grey. Each half a blend of the line and a
// parabola: 1 + 0.5a the slope at grey, 1 - 0.5a at black and white (and
// on past white).
double contrast_s(double g, double a)
{
    const double m = 0.5 * a;
    if (g < kGrey)
    {
        const double t = g / kGrey;
        return kGrey * ((1 - m) * t + m * t * t);
    }
    if (g > 1)
        return 1 + (g - 1) * (1 - m);
    const double t = (g - kGrey) / (1 - kGrey);
    return kGrey + (1 - kGrey) * ((1 + m) * t - m * t * t);
}

// Highlights, the brightening half (the inverse of recovering them, which
// rolls everything over the middle of the range off at slope 1 - 0.6a and
// brings about a stop and a half over white back under it).
double highlights_compress(double g, double a)
{
    return g - 0.6 * a * hinge(g - 0.5, 0.4);
}
double highlights_up(double g, double a)
{
    return inverse([&](double x) { return highlights_compress(x, a); }, g);
}

// Shadows, lifting: a bump over the lower part (to 0.6), the darks raised
// most, black left where it is.
double shadows_lift(double g, double a)
{
    const double top = 0.6;
    if (g >= top)
        return g;
    const double u = 1 - g / top;
    return g + 1.5 * a * g * u * u;
}

// Whites, raising: the top stretched (white from about 0.8 at +100), the
// middle hardly moved. Taking them down is its inverse: what's over white
// is brought back.
double whites_stretch(double g, double a)
{
    return g + 0.4 * a * g * g * g * g;
}

// Blacks, lowering: the bottom stretched below 0 (clipped: the darkest
// tenth or so goes black at -100). Raising them is its inverse, black lifted.
double blacks_stretch(double g, double a)
{
    const double u = std::max(0.0, 1 - g);
    return g - 0.15 * a * u * u * u * u;
}
double blacks_down(double g, double a)
{
    return std::max(0.0, blacks_stretch(g, a));
}
double blacks_up(double g, double a)
{
    return inverse([&](double x) { return blacks_stretch(x, a); }, g);
}

// Process 2010's brightness: the middle raised (a power, the ends kept).
double brightness(double g, double amount)
{
    return g <= 0 ? 0 : std::pow(g, std::exp2(-0.5 * amount));
}

// A monotone cubic through points (Fritsch and Carlson): no overshoot, so a
// curve whose points rise rises everywhere. Before the first point, its
// value; past the last, on at the last segment's slope where that's the end
// of the range (1), else flat.
struct Spline
{
    std::vector<double> x, y, m;
    explicit Spline(std::vector<std::pair<double, double>> points)
    {
        std::sort(points.begin(), points.end());
        for (const auto& [px, py] : points)
            if (x.empty() || px > x.back())
                x.push_back(px), y.push_back(py);
        const size_t n = x.size();
        m.assign(n, 0);
        if (n < 2)
            return;
        std::vector<double> d(n - 1);
        for (size_t i = 0; i + 1 < n; ++i)
            d[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
        m[0] = d[0], m[n - 1] = d[n - 2];
        for (size_t i = 1; i + 1 < n; ++i)
            m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
        for (size_t i = 0; i + 1 < n; ++i)
        {
            if (d[i] == 0)
            {
                m[i] = m[i + 1] = 0;
                continue;
            }
            const double a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
            if (s > 9)
            {
                const double t = 3 / std::sqrt(s);
                m[i] = t * a * d[i], m[i + 1] = t * b * d[i];
            }
        }
    }
    double operator()(double v) const
    {
        const size_t n = x.size();
        if (n == 0)
            return v;
        if (n == 1 || v <= x[0])
            return y[0];
        if (v >= x[n - 1])
            return y[n - 1] + (x[n - 1] >= 1 ? (v - x[n - 1]) * (y[n - 1] - y[n - 2]) / (x[n - 1] - x[n - 2]) : 0);
        const size_t i = size_t(std::upper_bound(x.begin(), x.end(), v) - x.begin()) - 1;
        const double h = x[i + 1] - x[i], t = (v - x[i]) / h, t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y[i] + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * y[i + 1] +
               (t3 - t2) * h * m[i + 1];
    }
};

// A point curve (0..255) as a map of the axis; none when linear.
std::optional<Spline> point_curve(const Adjustments::Curve& c)
{
    if (c.size() < 2 || c == Adjustments::Curve{{0, 0}, {255, 255}})
        return std::nullopt;
    std::vector<std::pair<double, double>> p;
    for (const auto& [x, y] : c)
        p.emplace_back(x / 255.0, y / 255.0);
    return Spline(std::move(p));
}

// The parametric curve: each region's slider moves the curve at the
// region's middle, by up to 40% of the region's width, through a monotone
// cubic from (0, 0) to (1, 1); the splits say where the regions meet.
std::optional<Spline> parametric_curve(const Adjustments& a)
{
    if (std::all_of(std::begin(a.parametric), std::end(a.parametric), [](double v) { return v == 0; }))
        return std::nullopt;
    double edges[5] = {0, a.splits[0] / 100, a.splits[1] / 100, a.splits[2] / 100, 1};
    for (int i = 1; i < 4; ++i)
        edges[i] = std::clamp(edges[i], edges[i - 1] + 0.01, 0.99);
    std::vector<std::pair<double, double>> p{{0, 0}};
    double last = 0;
    for (int r = 0; r < 4; ++r)
    {
        const double width = edges[r + 1] - edges[r], mid = (edges[r] + edges[r + 1]) / 2;
        const double y = mid + std::clamp(a.parametric[r] / 100, -1.0, 1.0) * 0.4 * width;
        last = std::clamp(y, last + 1e-3, 1 - 1e-3 * (4 - r));  // rising
        p.emplace_back(mid, last);
    }
    p.emplace_back(1, 1);
    return Spline(std::move(p));
}

// The tone controls in order, on the axis: Process 2012's contrast first
// (the look), then the regions brought into it (highlights, shadows), then
// the ends set (whites, blacks); Process 2010's in its own order (recovery,
// fill light, blacks, brightness, contrast). Then the parametric curve,
// then the point curve.
double master(const Adjustments& a, const std::optional<Spline>& parametric, const std::optional<Spline>& points, double g)
{
    const auto unit = [](double v) { return std::clamp(v / 100, -1.0, 1.0); };
    if (a.legacy)
    {
        g = either_way(g, -unit(a.recovery), whites_stretch);
        g = either_way(g, unit(a.fill_light), shadows_lift);
        if (a.blacks2010 > 0)
            g = blacks_down(g, a.blacks2010 / 20);
        g = brightness(g, std::clamp(a.brightness / 100, -1.5, 1.5));
        g = either_way(g, unit(a.contrast2010), contrast_s);
    }
    else
    {
        g = either_way(g, unit(a.contrast), contrast_s);
        const double h = unit(a.highlights);
        g = h < 0 ? highlights_compress(g, -h) : h > 0 ? highlights_up(g, h) : g;
        g = either_way(g, unit(a.shadows), shadows_lift);
        g = either_way(g, unit(a.whites), whites_stretch);
        const double b = unit(a.blacks);
        g = b < 0 ? blacks_down(g, -b) : b > 0 ? blacks_up(g, b) : g;
    }
    if (parametric)
        g = (*parametric)(g);
    if (points)
        g = (*points)(g);
    return std::max(0.0, g);
}

void to_float(const Mat3& m, float out[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = float(m[i][j]);
}

// HSV of a linear colour (hue in turns, 0 at red); and back.
void hsv(const float c[3], float& h, float& s, float& v)
{
    v = std::max({c[0], c[1], c[2]});
    const float lo = std::min({c[0], c[1], c[2]}), range = v - lo;
    s = v > 0 ? range / v : 0;
    if (range <= 0)
    {
        h = 0;
        return;
    }
    if (v == c[0])
        h = (c[1] - c[2]) / range;
    else if (v == c[1])
        h = 2 + (c[2] - c[0]) / range;
    else
        h = 4 + (c[0] - c[1]) / range;
    h /= 6;
    if (h < 0)
        h += 1;
}

void rgb(float h, float s, float v, float c[3])
{
    h = (h - std::floor(h)) * 6;
    const int i = std::min(5, int(h));
    const float f = h - float(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    c[0] = table[i][0], c[1] = table[i][1], c[2] = table[i][2];
}

// A display colour (sRGB, encoded) in the working space's hue (turns).
float working_hue(const Mat3& from_709, double r, double g, double b)
{
    const double lin[3] = {decode(r), decode(g), decode(b)};
    float w[3];
    for (int i = 0; i < 3; ++i)
        w[i] = float(from_709[i][0] * lin[0] + from_709[i][1] * lin[1] + from_709[i][2] * lin[2]);
    float h, s, v;
    hsv(w, h, s, v);
    return h;
}

// The colour table: HSL's bands centred on the colours they're named for
// as sRGB shows them (red, orange (255, 128, 0), yellow, green, aqua, blue,
// purple (128, 0, 255), magenta), each band's setting blended into the next
// with a cosine across the hues between; a hue shift of +-100 moves a band
// half way to its neighbour on that side (30 degrees at most), saturation
// -100 to grey and +100 to twice the chroma, luminance +-1.5 EV on colours
// (greys stay, as they have no hue). Saturation scales every colour's
// chroma; vibrance most the least saturated's, and skin's hue (an sRGB skin
// tone's) half as much when raising. The table's saturation axis is HSV's
// (how far from grey); the scale applies to the colour's distance from its
// luminance (adjust()).
void colour_table(const Adjustments& a, const Mat3& from_709, std::vector<float>& out)
{
    static const double kNamed[8][3] = {{1, 0, 0}, {1, 0.5, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {0.5, 0, 1}, {1, 0, 1}};
    double centre[8];
    for (int k = 0; k < 8; ++k)
        centre[k] = working_hue(from_709, kNamed[k][0], kNamed[k][1], kNamed[k][2]);
    const double skin = working_hue(from_709, 0.87, 0.67, 0.55);
    const auto gap = [&](int k, int to) { return std::fmod(centre[(to + 8) % 8] - centre[k] + 2.0, 1.0); };  // forwards, in turns
    out.assign(size_t(AdjustTables::kHues) * AdjustTables::kSats * 4, 0);
    for (int hi = 0; hi < AdjustTables::kHues; ++hi)
    {
        const double h = double(hi) / AdjustTables::kHues;
        // The bands either side of this hue, and how far between them.
        int k = 7;
        for (int j = 0; j < 8; ++j)
            if (std::fmod(h - centre[j] + 2.0, 1.0) < gap(j, j + 1))
                k = j;
        const int next = (k + 1) % 8;
        const double t = std::fmod(h - centre[k] + 2.0, 1.0) / gap(k, k + 1);
        const double w_next = 0.5 - 0.5 * std::cos(t * std::numbers::pi), w = 1 - w_next;
        const auto band = [&](const double* v, int b) { return std::clamp(v[b] / 100, -1.0, 1.0); };
        const auto shift_of = [&](int b)
        {
            const double v = band(a.hue, b);
            return v * std::min(0.5 * (v > 0 ? gap(b, b + 1) : gap((b + 7) % 8, b)), 30.0 / 360);
        };
        const double shift = w * shift_of(k) + w_next * shift_of(next);
        const double sat = w * (1 + band(a.sat, k)) + w_next * (1 + band(a.sat, next));
        const double lum = w * band(a.lum, k) + w_next * band(a.lum, next);
        double d = std::fabs(h - skin);
        d = std::min(d, 1 - d) * 360;
        const double vibrance = std::clamp(a.vibrance / 100, -1.0, 1.0);
        const double protect = vibrance > 0 ? 1 - 0.5 * std::exp(-(d / 25) * (d / 25)) : 1;
        for (int si = 0; si < AdjustTables::kSats; ++si)
        {
            const double s = double(si) / (AdjustTables::kSats - 1);
            float* e = &out[(size_t(si) * AdjustTables::kHues + hi) * 4];
            e[0] = float(shift);
            e[1] = float(sat * (1 + std::clamp(a.saturation / 100, -1.0, 1.0)) * (1 + vibrance * (1 - s) * protect));
            e[2] = float(std::exp2(1.5 * lum * std::min(1.0, s / 0.3)));
            e[3] = 0;
        }
    }
}

std::atomic<uint64_t> serials{0};

}  // namespace

std::shared_ptr<const AdjustTables> build_tables(const Adjustments& a)
{
    auto t = std::make_shared<AdjustTables>();
    t->serial = ++serials;
    // The working space: linear ProPhoto.
    const Mat3 xyz_from_working = rgb_to_xyz_d65(ColorSpace::prophoto()), xyz_from_709 = rgb_to_xyz_d65(ColorSpace::srgb());
    const Mat3 working_from_xyz = inverse(xyz_from_working), working_from_709 = mul(working_from_xyz, xyz_from_709);
    to_float(working_from_xyz, t->to_working);
    to_float(xyz_from_working, t->from_working);
    to_float(working_from_709, t->from_709);
    to_float(mul(inverse(xyz_from_709), xyz_from_working), t->to_709);

    // Process 2010's curve is its ToneCurve, or the preset it names.
    Adjustments::Curve points = a.curve;
    if (points.empty())
        points = named_curve(a.curve_name);
    const std::optional<Spline> parametric = parametric_curve(a), master_points = point_curve(points);
    const std::optional<Spline> channel[3] = {point_curve(a.red), point_curve(a.green), point_curve(a.blue)};
    t->channels = channel[0] || channel[1] || channel[2];
    t->tone_top = float(encode(AdjustTables::kToneTopLinear));
    t->tone.resize(size_t(AdjustTables::kTone) * 4);
    for (int i = 0; i < AdjustTables::kTone; ++i)
    {
        const double g = double(t->tone_top) * i / (AdjustTables::kTone - 1);
        float* e = &t->tone[size_t(i) * 4];
        e[0] = float(decode(master(a, parametric, master_points, g)));
        for (int c = 0; c < 3; ++c)
            e[1 + c] = float(decode(channel[c] ? std::max(0.0, (*channel[c])(g)) : g));
    }
    colour_table(a, working_from_709, t->colour);
    return t;
}

namespace
{

// A linear value through a tone table's column (edit.hlsli's tone_lookup).
float lookup(const AdjustTables& t, int column, float x)
{
    if (x <= 0)  // on at slope 1 below black
        return t.tone[size_t(column)] + x;
    const float top = float(AdjustTables::kToneTopLinear);
    if (x >= top)
        return x * t.tone[size_t(AdjustTables::kTone - 1) * 4 + column] / top;
    const float at = float(encode(x)) / t.tone_top * (AdjustTables::kTone - 1);
    const int i = std::min(int(at), AdjustTables::kTone - 2);
    const float f = at - float(i);
    return t.tone[size_t(i) * 4 + column] * (1 - f) + t.tone[size_t(i + 1) * 4 + column] * f;
}

}  // namespace

// As edit.hlsli's adjust(), step for step.
void apply_adjustments(const AdjustTables& t, float c[3])
{
    float w[3];
    for (int i = 0; i < 3; ++i)
        w[i] = t.from_709[i][0] * c[0] + t.from_709[i][1] * c[1] + t.from_709[i][2] * c[2];
    // Colour: hue, saturation and value through the map.
    float h, s, v;
    hsv(w, h, s, v);
    if (v > 0)
    {
        const float x = h * AdjustTables::kHues, y = std::min(s, 1.0f) * (AdjustTables::kSats - 1);
        const int x0 = int(x) % AdjustTables::kHues, x1 = (x0 + 1) % AdjustTables::kHues, y0 = std::min(int(y), AdjustTables::kSats - 2);
        const float fx = x - std::floor(x), fy = y - float(y0);
        float e[3];
        for (int k = 0; k < 3; ++k)
        {
            const auto at = [&](int xi, int yi) { return t.colour[(size_t(yi) * AdjustTables::kHues + xi) * 4 + k]; };
            e[k] = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x1, y0 + 1) * fx) * fy;
        }
        if (e[0] != 0)
            rgb(h + e[0], s, v, w);
        // Chroma scaled about the colour's luminance, no further than the
        // working space's edge (where a component reaches 0).
        const float lum = t.from_working[1][0] * w[0] + t.from_working[1][1] * w[1] + t.from_working[1][2] * w[2];
        float k = e[1];
        for (int i = 0; i < 3 && k > 1; ++i)
            if (w[i] < lum && w[i] >= 0)
                k = std::min(k, std::max(1.0f, lum / (lum - w[i])));
        for (int i = 0; i < 3; ++i)
            w[i] = (lum + k * (w[i] - lum)) * e[2];
    }
    // Tone: the largest and smallest components through the master curve,
    // the middle one kept where it was between them; then each channel's.
    const float hi = std::max({w[0], w[1], w[2]}), lo = std::min({w[0], w[1], w[2]});
    const float hi2 = lookup(t, 0, hi);
    const float lo2 = lookup(t, 0, lo);
    for (int i = 0; i < 3; ++i)
    {
        w[i] = hi > lo ? lo2 + (hi2 - lo2) * (w[i] - lo) / (hi - lo) : hi2;
        if (t.channels)
            w[i] = lookup(t, 1 + i, w[i]);
    }
    for (int i = 0; i < 3; ++i)
        c[i] = t.to_709[i][0] * w[0] + t.to_709[i][1] * w[1] + t.to_709[i][2] * w[2];
}

#include "look.h"

#include "decoders/texel.h"
#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>

bool LookParams::identity() const
{
    return contrast == 1 && chroma_gain == 1 && vibrance == 0 && std::all_of(std::begin(hue_gain), std::end(hue_gain), [](double g) { return g == 1; }) &&
           std::all_of(std::begin(hue_shift), std::end(hue_shift), [](double s) { return s == 0; });
}

LookParams look_params(Look look)
{
    // Camera-like as fitted to camera JPEGs (look.h): final.json's curve
    // (c, p), its chroma gain and vibrance (S, V), and fit4.txt's per hue
    // sector gains and turns (hg, hs; the two sectors with too few images
    // left at none).
    LookParams camera;
    camera.contrast = 1.24;
    camera.pivot = 0.58;
    camera.chroma_gain = 1.04;
    camera.vibrance = 0.45;
    const double gains[12] = {0.950, 1.037, 1.001, 1.011, 1.120, 1.032, 0.744, 0.770, 0.815, 0.712, 1, 1};
    const double turns[12] = {1.97, -3.07, -2.03, 1.61, 0.22, -1.22, 5.88, 3.81, -3.16, -8.39, 0, 0};
    std::copy(std::begin(gains), std::end(gains), camera.hue_gain);
    std::copy(std::begin(turns), std::end(turns), camera.hue_shift);
    switch (look)
    {
        case Look::Neutral: return {};
        case Look::CameraLike: return camera;
        case Look::Enhanced:
        {
            // The same, half as far again from none in contrast and in the
            // vibrance, and a 6% stronger chroma gain; the camera's balance
            // between hues as it is.
            LookParams p = camera;
            p.contrast = 1.36;
            p.chroma_gain = 1.10;
            p.vibrance = 0.65;
            return p;
        }
        case Look::Monochrome:
        {
            LookParams p;
            p.contrast = camera.contrast;
            p.pivot = camera.pivot;
            p.chroma_gain = 0;
            return p;
        }
    }
    return {};
}

double look_tone(const LookParams& p, double g)
{
    if (p.contrast == 1 || g <= 0)
        return g;
    const double x = srgb_decode(g);
    if (x >= 1)
        return g;
    const double u = std::cbrt(x), c = p.contrast, q = p.pivot;
    const double v = u < q ? q * std::pow(u / q, c) : 1 - (1 - q) * std::pow((1 - u) / (1 - q), c);
    return srgb_encode(v * v * v);
}

namespace
{

// Oklab (Ottosson, 2020), from linear BT.709 and back; cube roots signed,
// for the working space's colours outside BT.709.
void to_oklab(const double c[3], double lab[3])
{
    const double l = std::cbrt(0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2]);
    const double m = std::cbrt(0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2]);
    const double s = std::cbrt(0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2]);
    lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

void from_oklab(const double lab[3], double c[3])
{
    const double l = std::pow(lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2], 3);
    const double m = std::pow(lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2], 3);
    const double s = std::pow(lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2], 3);
    c[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    c[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    c[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

// A sector table at hue h (degrees): its centres at 15, 45..., linear between.
double sectors(const double (&t)[12], double h)
{
    const double x = (h - 15) / 30;
    const double i = std::floor(x), f = x - i;
    const int i0 = ((int(i) % 12) + 12) % 12;
    return t[i0] * (1 - f) + t[(i0 + 1) % 12] * f;
}

}  // namespace

double look_colour(const LookParams& p, double rgb[3])
{
    double lab[3];
    to_oklab(rgb, lab);
    const double C = std::hypot(lab[1], lab[2]);
    if (C <= 0)
        return p.chroma_gain * (1 + p.vibrance);
    double h = std::atan2(lab[2], lab[1]) * 180 / std::numbers::pi;
    if (h < 0)
        h += 360;
    const double scale = p.chroma_gain * (1 + p.vibrance * std::exp(-C / p.vibrance_width)) * sectors(p.hue_gain, h);
    if (const double turn = sectors(p.hue_shift, h); turn != 0)
    {
        const double r = (h + turn) * std::numbers::pi / 180;
        lab[1] = C * std::cos(r);
        lab[2] = C * std::sin(r);
        from_oklab(lab, rgb);
    }
    return scale;
}

const char* look_id(Look look)
{
    switch (look)
    {
        case Look::Neutral: return "neutral";
        case Look::CameraLike: return "camera";
        case Look::Enhanced: return "enhanced";
        case Look::Monochrome: return "monochrome";
    }
    return "neutral";
}

std::optional<Look> look_named(std::string_view id)
{
    for (int i = 0; i < kLookCount; ++i)
        if (id == look_id(Look(i)))
            return Look(i);
    return std::nullopt;
}

const char* look_label(Look look)
{
    switch (look)
    {
        case Look::Neutral: return N_("Neutral");
        case Look::CameraLike: return N_("Camera-like");
        case Look::Enhanced: return N_("Enhanced");
        case Look::Monochrome: return N_("Monochrome");
    }
    return N_("Neutral");
}

namespace
{

const struct
{
    const char* name;
    Look look;
} kProfiles[] = {
    {"Adobe Neutral", Look::Neutral},
    {"Camera Neutral", Look::Neutral},
    {"Camera Faithful", Look::Neutral},
    {"Adobe Standard", Look::CameraLike},
    {"Adobe Color", Look::CameraLike},
    {"Camera Standard", Look::CameraLike},
    {"Adobe Portrait", Look::CameraLike},
    {"Camera Portrait", Look::CameraLike},
    {"Adobe Vivid", Look::Enhanced},
    {"Adobe Landscape", Look::Enhanced},
    {"Camera Landscape", Look::Enhanced},
    {"Adobe Monochrome", Look::Monochrome},
    {"Camera Monochrome", Look::Monochrome},
};

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has(const std::string& s, const char* word)
{
    return s.find(word) != std::string::npos;
}

bool monochrome(const std::string& s)
{
    return has(s, "monochrome") || has(s, "b&w") || has(s, "acros") || has(s, "black & white") || has(s, "black and white") ||
           s == "bw" || s.rfind("bw ", 0) == 0;
}

}  // namespace

Look look_of_profile(std::string_view profile)
{
    for (const auto& p : kProfiles)
        if (profile == p.name)
            return p.look;
    if (profile.substr(0, 7) == "Camera ")  // a camera matching profile: the style it matches
        return look_of_picture_style(profile.substr(7));
    return monochrome(lower(profile)) ? Look::Monochrome : Look::CameraLike;
}

bool known_profile(std::string_view profile)
{
    return std::any_of(std::begin(kProfiles), std::end(kProfiles), [&](const auto& p) { return profile == p.name; });
}

const char* adobe_profile(Look look)
{
    switch (look)
    {
        case Look::Neutral: return "Adobe Neutral";
        case Look::CameraLike: return "Adobe Color";
        case Look::Enhanced: return "Adobe Vivid";
        case Look::Monochrome: return "Adobe Monochrome";
    }
    return "Adobe Color";
}

Look look_of_picture_style(std::string_view style)
{
    const std::string s = lower(style);
    if (monochrome(s))
        return Look::Monochrome;
    // The low-contrast, low-saturation styles: as the scene is.
    for (const char* w : {"neutral", "faithful", "flat", "low saturation", "pro neg", "eterna"})
        if (has(s, w))
            return Look::Neutral;
    for (const char* w : {"landscape", "vivid", "high saturation", "velvia"})
        if (has(s, w))
            return Look::Enhanced;
    return Look::CameraLike;
}

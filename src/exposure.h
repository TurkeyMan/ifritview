// A raw's automatic exposure model (shaders/exposure.hlsli), the shader's
// own code, for the CPU: the luminance at a raw's white from its EXIF, and
// ifritview --check-exposure.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace exposure
{
using std::exp2;
using std::log10;
using std::log2;
inline float clamp(float x, float lo, float hi) { return std::clamp(x, lo, hi); }
inline float min(float a, float b) { return std::min(a, b); }
inline float max(float a, float b) { return std::max(a, b); }
#include "shaders/exposure.hlsli"

// Which rating a raw's ISO is (Exif 2.3 / CIPA DC-008's SensitivityType,
// 0x8830). Each is only a nominal approximation of S_sat: an ISO 12232
// speed can be noise-based as well as saturation-based, a standard output
// sensitivity or recommended exposure index is set by the camera's maker's
// rendering. Recorded for the trace; none is taken as S_sat.
enum class Speed
{
    IsoSpeed,  // ISO 12232's ISO speed among what's stated (types 3, 5, 6, 7)
    Sos,       // standard output sensitivity only (1)
    Rei,       // recommended exposure index only (2)
    SosRei,    // both (4)
    Unstated,  // no SensitivityType, or 0
};
inline const char* speed_name(Speed s)
{
    switch (s)
    {
        case Speed::IsoSpeed: return "nominal: ISO speed";
        case Speed::Sos: return "nominal: standard output sensitivity";
        case Speed::Rei: return "nominal: recommended exposure index";
        case Speed::SosRei: return "nominal: SOS and REI";
        default: return "nominal: sensitivity type not stated";
    }
}

// The ISO to take as S (the photographic sensitivity, LibRaw's ISO), and
// which rating it is. `type` -1: no SensitivityType.
struct Sensitivity
{
    float iso = 0;
    Speed speed = Speed::Unstated;
};
inline Sensitivity scene_sensitivity(int type, float photographic)
{
    switch (type)
    {
        case 3:
        case 5:
        case 6:
        case 7: return {photographic, Speed::IsoSpeed};
        case 1: return {photographic, Speed::Sos};
        case 2: return {photographic, Speed::Rei};
        case 4: return {photographic, Speed::SosRei};
        default: return {photographic, Speed::Unstated};
    }
}

// Per camera: S_sat over its nominal ISO, in EV (negative: the sensor is
// less sensitive than it's rated, so the scene was lighter than the
// nominal ISO says), applied whatever the rating. Only entries with a
// cited measurement belong here, and only they make a camera's L_white
// calibrated; there are none yet. Without one the nominal ISO stands, and
// a camera-wide bias is absorbed in `kNightDepth`.
struct SpeedCorrection
{
    const char* make;
    const char* model;
    float ev;
    const char* source;
};
inline constexpr std::array<SpeedCorrection, 0> kSpeedCorrections{};
inline float speed_correction(const char* make, const char* model)
{
    for (const SpeedCorrection& c : kSpeedCorrections)
        if (!std::strcmp(c.make, make) && !std::strcmp(c.model, model))
            return c.ev;
    return 0;
}

// L_white for a raw: its nominal ISO, corrected by the table; 0 when the
// ISO, shutter or aperture is unknown.
inline float raw_white_luminance(Sensitivity s, float shutter, float f_number, const char* make, const char* model)
{
    return saturation_luminance(s.iso * std::exp2(speed_correction(make, model)), shutter, f_number);
}
}  // namespace exposure

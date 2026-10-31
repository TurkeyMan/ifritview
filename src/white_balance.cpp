#include "white_balance.h"

#include "image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{

// CIE 1960 uv, where the isotherms and the tint are measured.
void xy_to_uv(double x, double y, double& u, double& v)
{
    const double d = -2 * x + 12 * y + 3;
    u = 4 * x / d;
    v = 6 * y / d;
}

void uv_to_xy(double u, double v, double& x, double& y)
{
    const double d = 2 * u - 8 * v + 4;
    x = 3 * u / d;
    y = 2 * v / d;
}

// Robertson's isotherms (1968; Wyszecki & Stiles' table): the mired, the
// Planckian locus's u and v there, the isotherm's slope.
const double kIsotherms[31][4] = {
    {0, 0.18006, 0.26352, -0.24341},   {10, 0.18066, 0.26589, -0.25479},  {20, 0.18133, 0.26846, -0.26876},  {30, 0.18208, 0.27119, -0.28539},
    {40, 0.18293, 0.27407, -0.30470},  {50, 0.18388, 0.27709, -0.32675},  {60, 0.18494, 0.28021, -0.35156},  {70, 0.18611, 0.28342, -0.37915},
    {80, 0.18740, 0.28668, -0.40955},  {90, 0.18880, 0.28997, -0.44278},  {100, 0.19032, 0.29326, -0.47888}, {125, 0.19462, 0.30141, -0.58204},
    {150, 0.19962, 0.30921, -0.70471}, {175, 0.20525, 0.31647, -0.84901}, {200, 0.21142, 0.32312, -1.0182},  {225, 0.21807, 0.32909, -1.2168},
    {250, 0.22511, 0.33439, -1.4512},  {275, 0.23247, 0.33904, -1.7298},  {300, 0.24010, 0.34308, -2.0637},  {325, 0.24792, 0.34655, -2.4681},
    {350, 0.25591, 0.34951, -2.9641},  {375, 0.26400, 0.35200, -3.5814},  {400, 0.27218, 0.35407, -4.3633},  {425, 0.28039, 0.35577, -5.3762},
    {450, 0.28863, 0.35714, -6.7262},  {475, 0.29685, 0.35823, -8.5955},  {500, 0.30505, 0.35907, -11.324},  {525, 0.31320, 0.35968, -15.628},
    {550, 0.32129, 0.36011, -23.325},  {575, 0.32931, 0.36038, -40.770},  {600, 0.33724, 0.36051, -116.45}};

// The tint, as Lightroom's (the DNG specification's): the white's distance
// from the locus along its isotherm, in uv, times this. The light's: + a
// green light (which, balanced, takes the picture towards magenta).
const double kTintScale = -3000;

// Where the isotherms `f` of the way from i to i - 1 cross the locus, and
// their direction there (unit length, towards + u: below the locus, magenta).
void on_isotherm(int i, double f, double& u, double& v, double& du, double& dv)
{
    const double* a = kIsotherms[i - 1];
    const double* b = kIsotherms[i];
    u = a[1] * f + b[1] * (1 - f);
    v = a[2] * f + b[2] * (1 - f);
    const double la = std::sqrt(1 + a[3] * a[3]), lb = std::sqrt(1 + b[3] * b[3]);
    du = f / la + (1 - f) / lb;
    dv = f * a[3] / la + (1 - f) * b[3] / lb;
    const double len = std::hypot(du, dv);
    du /= len, dv /= len;
}

// A white's temperature and tint: between the two isotherms it lies
// between, as its distances from them divide the gap (Robertson's method),
// and its distance along them.
void temperature_tint(ColorSpace::XY w, double& kelvin, double& tint)
{
    double u, v;
    xy_to_uv(w.x, w.y, u, v);
    double last = 0;
    for (int i = 1; i < 31; ++i)
    {
        const double len = std::sqrt(1 + kIsotherms[i][3] * kIsotherms[i][3]);
        const double d = ((v - kIsotherms[i][2]) - (u - kIsotherms[i][1]) * kIsotherms[i][3]) / len;  // its distance past isotherm i
        if (d <= 0 || i == 30)
        {
            const double past = std::max(-d, 0.0), f = i == 1 ? 0 : past / (last + past);
            kelvin = 1e6 / (kIsotherms[i - 1][0] * f + kIsotherms[i][0] * (1 - f));
            double lu, lv, du, dv;
            on_isotherm(i, f, lu, lv, du, dv);
            tint = ((u - lu) * du + (v - lv) * dv) * kTintScale;
            return;
        }
        last = d;
    }
}

double temperature(ColorSpace::XY w)
{
    double kelvin, tint;
    temperature_tint(w, kelvin, tint);
    return kelvin;
}

// The white of a temperature and tint.
ColorSpace::XY white_of(double kelvin, double tint)
{
    const double mired = std::clamp(1e6 / kelvin, 0.0, kIsotherms[30][0]);
    int i = 1;
    while (i < 30 && kIsotherms[i][0] < mired)
        ++i;
    const double f = (kIsotherms[i][0] - mired) / (kIsotherms[i][0] - kIsotherms[i - 1][0]);
    double u, v, du, dv;
    on_isotherm(i, f, u, v, du, dv);
    ColorSpace::XY w;
    uv_to_xy(u + du * tint / kTintScale, v + dv * tint / kTintScale, w.x, w.y);
    return w;
}

const ColorSpace::XY kD50 = {0.3457, 0.3585};  // the DNG SDK's

// A DNG's calibrations mixed for a white of temperature `t`: by inverse
// temperature between the two illuminants', the nearer one's alone past them.
struct Calibrated
{
    Mat3 xyz_to_camera;  // AB * CC * CM
    Mat3 to_reference;   // Inverse(AB * CC)
    Mat3 forward;
};

Calibrated calibrated(const DngColour& d, double t)
{
    const double t1 = d.temperature[0], t2 = d.temperature[1];
    const double g = t1 > 0 && t2 > 0 && t1 != t2 && t > 0 ? std::clamp((1 / t - 1 / t2) / (1 / t1 - 1 / t2), 0.0, 1.0) : 1.0;
    const auto mix = [g](const Mat3& a, const Mat3& b)
    {
        Mat3 m;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i][j] = g * a[i][j] + (1 - g) * b[i][j];
        return m;
    };
    Mat3 ab_cc = mix(d.calibration[0], d.calibration[1]);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            ab_cc[i][j] *= d.analog[i];
    return {mul(ab_cc, mix(d.colour[0], d.colour[1])), inverse(ab_cc), mix(d.forward[0], d.forward[1])};
}

ColorSpace::XY to_xy(const Vec3& xyz)
{
    const double sum = xyz[0] + xyz[1] + xyz[2];
    return sum > 0 ? ColorSpace::XY{xyz[0] / sum, xyz[1] / sum} : kD50;
}

Vec3 greatest_one(Vec3 v)
{
    const double top = std::max({v[0], v[1], v[2]});
    for (double& c : v)
        c = top > 0 ? c / top : 1;
    return v;
}

}  // namespace

Vec3 dng_neutral(const DngColour& d, ColorSpace::XY white)
{
    return greatest_one(mul(calibrated(d, temperature(white)).xyz_to_camera, to_xyz(white)));
}

ColorSpace::XY dng_white(const DngColour& d, const Vec3& neutral)
{
    ColorSpace::XY last = kD50;
    for (int pass = 0; pass < 30; ++pass)
    {
        ColorSpace::XY next = to_xy(mul(inverse(calibrated(d, temperature(last)).xyz_to_camera), neutral));
        if (std::fabs(next.x - last.x) + std::fabs(next.y - last.y) < 1e-7)
            return next;
        if (pass == 29)  // still swinging: between the two
            next = {(next.x + last.x) / 2, (next.y + last.y) / 2};
        last = next;
    }
    return last;
}

Mat3 dng_camera_to_xyz(const DngColour& d, const Vec3& neutral)
{
    const ColorSpace::XY white = dng_white(d, neutral);
    const Calibrated c = calibrated(d, temperature(white));
    const Vec3 d65 = to_xyz(ColorSpace::srgb().white);
    if (c.forward[1] != Vec3{})
    {
        // ForwardMatrix * D * Inverse(AB * CC), D making the white's
        // reference camera RGB 1, 1, 1 (which it takes to D50).
        const Vec3 reference = mul(c.to_reference, neutral);
        Mat3 balance = {};
        for (int i = 0; i < 3; ++i)
            balance[i][i] = reference[i] > 0 ? 1 / reference[i] : 1;
        return mul(bradford(to_xyz(kD50), d65), mul(c.forward, mul(balance, c.to_reference)));
    }
    // Inverse(AB * CC * CM), scaled as the white's camera RGB is (the
    // greatest 1) for it to come out at Y = 1, then adapted from the white
    // to D65 (the specification's adaptation to D50 and ours from there, in one).
    const Vec3 camera = mul(c.xyz_to_camera, to_xyz(white));
    const double top = std::max({camera[0], camera[1], camera[2]});
    Mat3 m = mul(bradford(to_xyz(white), d65), inverse(c.xyz_to_camera));
    for (Vec3& row : m)
        for (double& v : row)
            v *= top;
    return m;
}

void wb_multipliers(const RawMosaic& m, double kelvin, double tint, float out[3])
{
    // The camera's response to the white (its neutral), each channel's
    // multiplier its reciprocal. Far from the light a matrix was made for
    // (a warm white with a strong tint), a channel's response can reach 0
    // and below, its multiplier running off to infinity and wrapping. Kept
    // to at least 1/8 of the strongest: no channel is raised more than 8
    // times the least, which tungsten's correction stays within (about 7);
    // past that the picture holds rather than falling apart.
    const ColorSpace::XY white = white_of(kelvin, tint);
    Vec3 neutral;
    if (m.dng)
        neutral = dng_neutral(*m.dng, white);
    else
    {
        const Vec3 xyz = to_xyz(white);
        for (int c = 0; c < 3; ++c)
            neutral[c] = m.cam_xyz[c][0] * xyz[0] + m.cam_xyz[c][1] * xyz[1] + m.cam_xyz[c][2] * xyz[2];
    }
    const double strongest = std::max({neutral[0], neutral[1], neutral[2]});
    for (int c = 0; c < 3; ++c)
        out[c] = float(strongest > 0 ? strongest / std::max(neutral[c], strongest / 8) : 1);
}

void wb_as_shot(const RawMosaic& m, double& kelvin, double& tint)
{
    // The white the camera balanced for: its neutral (1 / the multipliers)
    // back through the colour matrix.
    const Vec3 neutral = {1.0 / m.as_shot[0], 1.0 / m.as_shot[1], 1.0 / m.as_shot[2]};
    ColorSpace::XY white = ColorSpace::srgb().white;
    if (m.dng)
        white = dng_white(*m.dng, neutral);
    else
    {
        Mat3 xyz_to_camera;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                xyz_to_camera[i][j] = m.cam_xyz[i][j];
        const Vec3 xyz = mul(inverse(xyz_to_camera), neutral);
        if (const double sum = xyz[0] + xyz[1] + xyz[2]; std::isfinite(sum) && sum > 0)
            white = {xyz[0] / sum, xyz[1] / sum};
    }
    temperature_tint(white, kelvin, tint);
    if (std::getenv("IFRIT_TRACE"))
    {
        float back[3];
        wb_multipliers(m, kelvin, tint, back);
        std::fprintf(stderr, "white balance: as shot R %.4f G %.4f B %.4f -> %.0f K, tint %.1f -> R %.4f G %.4f B %.4f\n", m.as_shot[0], m.as_shot[1],
                     m.as_shot[2], kelvin, tint, back[0], back[1], back[2]);
    }
}

#include "white_balance.h"

#include "image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{

// CIE xy of a blackbody at `t` kelvin: Kim et al.'s cubic fits to the
// Planckian locus (1667 K..25000 K).
void planck_xy(double t, double& x, double& y)
{
    t = std::clamp(t, 1667.0, 25000.0);
    const double t2 = t * t, t3 = t2 * t;
    x = t <= 4000 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                  : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    if (t <= 2222)
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    else if (t <= 4000)
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
}

// CIE 1960 uv, where the tint is a distance off the locus.
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

// The white's XYZ (Y = 1): on the locus at `kelvin`, then `tint` along its
// normal, a tint of 100 being a Duv of 0.01 (towards magenta, below the locus).
void white_xyz(double kelvin, double tint, double xyz[3])
{
    double x, y, u, v;
    planck_xy(kelvin, x, y);
    xy_to_uv(x, y, u, v);
    double x2, y2, u2, v2;
    planck_xy(kelvin * 1.01, x2, y2);
    xy_to_uv(x2, y2, u2, v2);
    double du = u2 - u, dv = v2 - v;
    const double len = std::hypot(du, dv);
    if (len > 0)
        du /= len, dv /= len;
    // The normal towards magenta: below the locus (smaller v).
    double nu = -dv, nv = du;
    if (nv > 0)
        nu = -nu, nv = -nv;
    const double duv = tint / 100.0 * 0.01;
    u += nu * duv;
    v += nv * duv;
    uv_to_xy(u, v, x, y);
    xyz[0] = x / y;
    xyz[1] = 1;
    xyz[2] = (1 - x - y) / y;
}

// Robertson's isotherms (Wyszecki & Stiles, as the DNG SDK has them): the
// mired, the locus's u and v there (CIE 1960), the isotherm's slope.
const double kIsotherms[31][4] = {
    {0, 0.18006, 0.26352, -0.24341},   {10, 0.18066, 0.26589, -0.25479},  {20, 0.18133, 0.26846, -0.26876},  {30, 0.18208, 0.27119, -0.28539},
    {40, 0.18293, 0.27407, -0.30470},  {50, 0.18388, 0.27709, -0.32675},  {60, 0.18494, 0.28021, -0.35156},  {70, 0.18611, 0.28342, -0.37915},
    {80, 0.18740, 0.28668, -0.40955},  {90, 0.18880, 0.28997, -0.44278},  {100, 0.19032, 0.29326, -0.47888}, {125, 0.19462, 0.30141, -0.58204},
    {150, 0.19962, 0.30921, -0.70471}, {175, 0.20525, 0.31647, -0.84901}, {200, 0.21142, 0.32312, -1.0182},  {225, 0.21807, 0.32909, -1.2168},
    {250, 0.22511, 0.33439, -1.4512},  {275, 0.23247, 0.33904, -1.7298},  {300, 0.24010, 0.34308, -2.0637},  {325, 0.24792, 0.34655, -2.4681},
    {350, 0.25591, 0.34951, -2.9641},  {375, 0.26400, 0.35200, -3.5814},  {400, 0.27218, 0.35407, -4.3633},  {425, 0.28039, 0.35577, -5.3762},
    {450, 0.28863, 0.35714, -6.7262},  {475, 0.29685, 0.35823, -8.5955},  {500, 0.30505, 0.35907, -11.324},  {525, 0.31320, 0.35968, -15.628},
    {550, 0.32129, 0.36011, -23.325},  {575, 0.32931, 0.36038, -40.770},  {600, 0.33724, 0.36051, -116.45}};

// The correlated colour temperature of the white `w`: between the two
// isotherms it lies between (Robertson's method).
double temperature(ColorSpace::XY w)
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
            return 1e6 / (kIsotherms[i - 1][0] * f + kIsotherms[i][0] * (1 - f));
        }
        last = d;
    }
    return 0;
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
    double xyz[3];
    white_xyz(kelvin, tint, xyz);
    if (m.dng)
    {
        const Vec3 neutral = dng_neutral(*m.dng, to_xy({xyz[0], xyz[1], xyz[2]}));  // the greatest 1: the least multiplier
        for (int c = 0; c < 3; ++c)
            out[c] = float(neutral[c] > 0 ? 1 / neutral[c] : 1);
        return;
    }
    double cam[3];
    for (int c = 0; c < 3; ++c)
        cam[c] = m.cam_xyz[c][0] * xyz[0] + m.cam_xyz[c][1] * xyz[1] + m.cam_xyz[c][2] * xyz[2];
    double mul[3];
    for (int c = 0; c < 3; ++c)
        mul[c] = cam[c] > 1e-9 ? 1 / cam[c] : 1;
    const double least = std::min({mul[0], mul[1], mul[2]});
    for (int c = 0; c < 3; ++c)
        out[c] = float(mul[c] / least);
}

void wb_as_shot(const RawMosaic& m, double& kelvin, double& tint)
{
    // The temperature and tint whose multipliers' ratios (R/G, B/G) are
    // nearest the camera's: a coarse search, then a finer one round it.
    const double want_r = std::log(m.as_shot[0] / m.as_shot[1]), want_b = std::log(m.as_shot[2] / m.as_shot[1]);
    const auto miss = [&](double t, double n)
    {
        float mul[3];
        wb_multipliers(m, t, n, mul);
        const double r = std::log(mul[0] / mul[1]) - want_r, b = std::log(mul[2] / mul[1]) - want_b;
        return r * r + b * b;
    };
    double best = 1e300;
    kelvin = 5500, tint = 0;
    for (double t = 2000; t <= 15000; t *= 1.02)
        for (double n = -150; n <= 150; n += 5)
            if (const double e = miss(t, n); e < best)
                best = e, kelvin = t, tint = n;
    const double t0 = kelvin, n0 = tint;
    for (double t = t0 / 1.02; t <= t0 * 1.02; t *= 1.002)
        for (double n = n0 - 5; n <= n0 + 5; n += 0.5)
            if (const double e = miss(t, n); e < best)
                best = e, kelvin = t, tint = n;
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "white balance: as shot R %.3f G %.3f B %.3f (R/G %.3f, B/G %.3f) -> %.0f K, tint %.1f\n", m.as_shot[0],
                     m.as_shot[1], m.as_shot[2], m.as_shot[0] / m.as_shot[1], m.as_shot[2] / m.as_shot[1], kelvin, tint);
}

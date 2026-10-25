#include "white_balance.h"

#include "image.h"

#include <algorithm>
#include <cmath>

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

}  // namespace

void wb_multipliers(const RawMosaic& m, double kelvin, double tint, float out[3])
{
    double xyz[3];
    white_xyz(kelvin, tint, xyz);
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
}

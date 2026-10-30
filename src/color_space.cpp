#include "color_space.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace
{

using Vec = std::array<double, 3>;
using Mat = std::array<Vec, 3>;  // rows

Mat mul(const Mat& a, const Mat& b)
{
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[i][j] += a[i][k] * b[k][j];
    return r;
}

Vec mul(const Mat& a, const Vec& v)
{
    Vec r{};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            r[i] += a[i][k] * v[k];
    return r;
}

Mat inverse(const Mat& m)
{
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    return {{{(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
             {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
             {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}}};
}

Vec xyz(ColorSpace::XY c)
{
    return {c.x / c.y, 1.0, (1.0 - c.x - c.y) / c.y};
}

// ICC's connection space white.
const Vec kD50 = {0.9642, 1.0, 0.8249};

// RGB to XYZ for the space, then adapted to D50 (Bradford), as ICC wants
// its colorants; and the adaptation itself, for the chad tag.
void colorants(const ColorSpace& s, Mat& rgb_to_d50, Mat& adapt)
{
    const Vec r = xyz(s.red), g = xyz(s.green), b = xyz(s.blue), w = xyz(s.white);
    const Mat p = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
    const Vec scale = mul(inverse(p), w);  // each primary's share of white
    Mat m = p;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] *= scale[j];
    const Mat bradford = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
    const Vec from = mul(bradford, w), to = mul(bradford, kD50);
    const Mat cone = {{{to[0] / from[0], 0, 0}, {0, to[1] / from[1], 0}, {0, 0, to[2] / from[2]}}};
    adapt = mul(inverse(bradford), mul(cone, bradford));
    rgb_to_d50 = mul(adapt, m);
}

}  // namespace

void rgb_to_xyz_d65(const ColorSpace& space, double out[3][3])
{
    Mat m, adapt, srgb_m, d65_to_d50;
    colorants(space, m, adapt);
    colorants(ColorSpace::srgb(), srgb_m, d65_to_d50);
    const Mat r = mul(inverse(d65_to_d50), m);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = r[i][j];
}

namespace
{

// Big-endian ICC encodings.
struct Writer
{
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint32_t v) { u8(uint8_t(v >> 8)), u8(uint8_t(v)); }
    void u32(uint32_t v) { u16(v >> 16), u16(v & 0xFFFF); }
    void sig(const char* s) { b.insert(b.end(), s, s + 4); }
    void s15f16(double v) { u32(uint32_t(int32_t(std::lround(v * 65536.0)))); }
    void pad() { while (b.size() % 4) u8(0); }
};

std::vector<uint8_t> xyz_tag(const Vec& v)
{
    Writer w;
    w.sig("XYZ "), w.u32(0);
    for (double c : v)
        w.s15f16(c);
    return w.b;
}

std::vector<uint8_t> sf32_tag(const Mat& m)
{
    Writer w;
    w.sig("sf32"), w.u32(0);
    for (const Vec& row : m)
        for (double c : row)
            w.s15f16(c);
    return w.b;
}

// One English string.
std::vector<uint8_t> mluc_tag(const std::string& text)
{
    Writer w;
    w.sig("mluc"), w.u32(0), w.u32(1), w.u32(12);
    w.sig("enUS");
    w.u32(uint32_t(text.size() * 2)), w.u32(28);
    for (unsigned char c : text)
        w.u16(c);  // ASCII as UTF-16BE
    return w.b;
}

std::vector<uint8_t> para_tag(const ColorSpace& s)
{
    Writer w;
    w.sig("para"), w.u32(0);
    // Type 3: Y = (aX + b)^g for X >= d, else cX. Type 0: Y = X^g.
    auto type3 = [&](double g, double a, double b, double c, double d)
    {
        w.u16(3), w.u16(0);
        for (double v : {g, a, b, c, d})
            w.s15f16(v);
    };
    switch (s.transfer)
    {
        case ColorSpace::Transfer::SRGB: type3(2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045); break;
        case ColorSpace::Transfer::BT709: type3(1 / 0.45, 1 / 1.099, 0.099 / 1.099, 1 / 4.5, 0.081); break;
        case ColorSpace::Transfer::ROMM: type3(1.8, 1, 0, 1 / 16.0, 1 / 32.0); break;
        case ColorSpace::Transfer::Gamma: w.u16(0), w.u16(0), w.s15f16(s.gamma); break;
        case ColorSpace::Transfer::Linear: w.u16(0), w.u16(0), w.s15f16(1.0); break;
    }
    return w.b;
}

}  // namespace

ColorSpace ColorSpace::srgb()
{
    return {"sRGB", {0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}, Transfer::SRGB};
}
ColorSpace ColorSpace::display_p3()
{
    return {"Display P3", {0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}, Transfer::SRGB};
}
ColorSpace ColorSpace::adobe_rgb()
{
    return {"Adobe RGB (1998)", {0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}, {0.3127, 0.3290}, Transfer::Gamma, 563.0 / 256.0};
}
ColorSpace ColorSpace::rec2020()
{
    return {"Rec. 2020", {0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290}, Transfer::BT709};
}
ColorSpace ColorSpace::prophoto()
{
    return {"ProPhoto RGB", {0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}, {0.3457, 0.3585}, Transfer::ROMM};
}

ColorSpace ColorSpace::aces_ap0()
{
    return {"ACES2065-1 (AP0)", {0.7347, 0.2653}, {0.0, 1.0}, {0.0001, -0.0770}, {0.32168, 0.33767}, Transfer::Linear};
}
ColorSpace ColorSpace::aces_ap1()
{
    return {"ACEScg (AP1)", {0.713, 0.293}, {0.165, 0.830}, {0.128, 0.044}, {0.32168, 0.33767}, Transfer::Linear};
}

ColorSpace ColorSpace::from_primaries(XY red, XY green, XY blue, XY white, Transfer transfer, double gamma, const std::string& name)
{
    const auto near = [](XY a, XY b) { return std::fabs(a.x - b.x) < 0.002 && std::fabs(a.y - b.y) < 0.002; };
    for (ColorSpace s : {srgb(), display_p3(), adobe_rgb(), rec2020(), prophoto(), aces_ap0(), aces_ap1()})
        if (near(red, s.red) && near(green, s.green) && near(blue, s.blue) && near(white, s.white))
        {
            if (s.transfer == transfer && (transfer != Transfer::Gamma || std::fabs(s.gamma - gamma) < 0.01))
                return s;
            return s.with_transfer(transfer, gamma);
        }
    ColorSpace s{name, red, green, blue, white, transfer, gamma};
    return s;
}

ColorSpace ColorSpace::with_transfer(Transfer t, double g) const
{
    ColorSpace s = *this;
    if (s.transfer == t && (t != Transfer::Gamma || s.gamma == g))
        return s;
    s.transfer = t;
    s.gamma = g;
    s.name += t == Transfer::Linear ? " (linear)"
              : t == Transfer::SRGB ? ", sRGB curve"
              : t == Transfer::BT709 ? ", BT.709 curve"
              : t == Transfer::ROMM  ? ", ROMM curve"
                                     : ", gamma " + [g] { char b[16]; std::snprintf(b, sizeof b, "%.3g", g); return std::string(b); }();
    return s;
}

bool ColorSpace::is_srgb() const
{
    const ColorSpace s = srgb();
    const auto near = [](XY a, XY b) { return std::fabs(a.x - b.x) < 0.002 && std::fabs(a.y - b.y) < 0.002; };
    return transfer == Transfer::SRGB && near(red, s.red) && near(green, s.green) && near(blue, s.blue) && near(white, s.white);
}

std::vector<uint8_t> icc_profile(const ColorSpace& space)
{
    Mat m, adapt;
    colorants(space, m, adapt);
    const std::vector<uint8_t> curve = para_tag(space);
    struct Tag
    {
        const char* sig;
        std::vector<uint8_t> data;
        int same_as = -1;  // the three curves share one copy
    };
    const std::vector<Tag> tags = {
        {"desc", mluc_tag(space.name)},
        {"cprt", mluc_tag("No copyright, use freely")},
        {"wtpt", xyz_tag(kD50)},
        {"chad", sf32_tag(adapt)},
        {"rXYZ", xyz_tag({m[0][0], m[1][0], m[2][0]})},
        {"gXYZ", xyz_tag({m[0][1], m[1][1], m[2][1]})},
        {"bXYZ", xyz_tag({m[0][2], m[1][2], m[2][2]})},
        {"rTRC", curve},
        {"gTRC", {}, 7},
        {"bTRC", {}, 7},
    };

    // The tags' data after the header and the tag table, each on four bytes.
    Writer data;
    std::vector<uint32_t> offset(tags.size()), size(tags.size());
    const uint32_t start = 128 + 4 + 12 * uint32_t(tags.size());
    for (size_t i = 0; i < tags.size(); ++i)
    {
        if (tags[i].same_as >= 0)
        {
            offset[i] = offset[size_t(tags[i].same_as)];
            size[i] = size[size_t(tags[i].same_as)];
            continue;
        }
        offset[i] = start + uint32_t(data.b.size());
        size[i] = uint32_t(tags[i].data.size());
        data.b.insert(data.b.end(), tags[i].data.begin(), tags[i].data.end());
        data.pad();
    }

    Writer w;
    const uint32_t total = start + uint32_t(data.b.size());
    w.u32(total);
    w.u32(0);           // preferred CMM
    w.u32(0x04300000);  // version 4.3
    w.sig("mntr"), w.sig("RGB "), w.sig("XYZ ");
    for (uint32_t v : {2026u, 1u, 1u, 0u, 0u, 0u})  // created
        w.u16(v);
    w.sig("acsp");
    w.u32(0), w.u32(0), w.u32(0), w.u32(0);  // platform, flags, manufacturer, model
    w.u32(0), w.u32(0);                      // attributes
    w.u32(0);                                // perceptual
    for (double c : kD50)
        w.s15f16(c);
    w.sig("IfVw");                    // creator
    w.b.resize(w.b.size() + 16 + 28);  // profile ID (not computed), reserved
    w.u32(uint32_t(tags.size()));
    for (size_t i = 0; i < tags.size(); ++i)
        w.sig(tags[i].sig), w.u32(offset[i]), w.u32(size[i]);
    w.b.insert(w.b.end(), data.b.begin(), data.b.end());
    return w.b;
}

CicpSpace from_cicp(int primaries, int transfer, double gamma)
{
    using T = ColorSpace::Transfer;
    std::optional<ColorSpace> base;
    const char* p = nullptr;
    switch (primaries)
    {
        case 1: base = ColorSpace::srgb(), p = "BT.709"; break;
        case 9: base = ColorSpace::rec2020(), p = "BT.2020"; break;
        case 11:
            base = ColorSpace::from_primaries({0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.314, 0.351}, T::Gamma, 2.6, "DCI-P3");
            p = "DCI-P3";
            break;
        case 12: base = ColorSpace::display_p3(), p = "Display P3"; break;
        default: break;
    }
    std::optional<T> t;
    const char* tn = nullptr;
    switch (transfer)
    {
        case 1: case 6: case 14: case 15: t = T::BT709, tn = "BT.709"; break;
        case 4: t = T::Gamma, gamma = 2.2, tn = "gamma 2.2"; break;
        case 5: t = T::Gamma, gamma = 2.8, tn = "gamma 2.8"; break;
        case 8: t = T::Linear, tn = "linear"; break;
        case 13: t = T::SRGB, tn = "sRGB"; break;
        case 16: tn = "PQ"; break;
        case 18: tn = "HLG"; break;
        default:
            if (gamma > 0)
                t = T::Gamma, tn = "gamma";
            break;
    }
    CicpSpace c;
    if (base && t)
    {
        c.space = base->with_transfer(*t, gamma);
        c.label = c.space->is_srgb() ? "sRGB" : c.space->name;
    }
    else if (p)
        c.label = tn ? std::string(p) + " " + tn : p;
    return c;
}

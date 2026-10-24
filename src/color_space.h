#pragma once

// An RGB colour space as its primaries, white point and transfer curve:
// enough to describe the standard ones, and to write an ICC profile for one,
// so a space a file only names (an EXIF flag, a PNG's cHRM, HEIF's nclx)
// travels with the image like a profile it embeds.

#include <cstdint>
#include <string>
#include <vector>

struct ColorSpace
{
    struct XY
    {
        double x, y;  // CIE 1931 chromaticity
    };
    enum class Transfer
    {
        SRGB,    // IEC 61966-2-1
        BT709,   // ITU-R BT.709 / BT.2020's (the camera curve, inverted)
        ROMM,    // ProPhoto: 1.8 with a linear toe
        Gamma,   // a pure power, `gamma`
        Linear,
    };

    std::string name;  // the profile's description: "Display P3"
    XY red, green, blue, white;
    Transfer transfer = Transfer::SRGB;
    double gamma = 2.2;  // Transfer::Gamma

    static ColorSpace srgb();
    static ColorSpace display_p3();
    static ColorSpace adobe_rgb();
    static ColorSpace rec2020();
    static ColorSpace prophoto();
    // These primaries and white, as the standard spaces are; or a space of
    // their own, named `name`, with the transfer given.
    static ColorSpace from_primaries(XY red, XY green, XY blue, XY white, Transfer transfer, double gamma, const std::string& name);

    // The same primaries and white, with another transfer (what the 8-bit
    // rendering of linear data is: these primaries, sRGB's curve).
    ColorSpace with_transfer(Transfer t, double g = 2.2) const;

    // sRGB as an untagged image means it: nothing worth embedding.
    bool is_srgb() const;
};

// A display-class ICC v4 profile (matrix and parametric curves) for `space`.
std::vector<uint8_t> icc_profile(const ColorSpace& space);

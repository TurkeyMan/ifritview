#pragma once
// What the decoders share below their formats: 8-bit pixels made the way
// the image holds them, and the wider texel layouts of the texture
// containers (DDS, KTX, VTF), decoded one way whatever the container.

#include "../image.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

// -- 8-bit pixels -------------------------------------------------------------

// Straight 8-bit channels as the image holds a pixel: ARGB, colour
// multiplied by alpha. `premultiplied`: the colour already is, and a stored
// colour can't outshine its alpha.
inline uint32_t argb(uint32_t r, uint32_t g, uint32_t b, uint32_t a, bool premultiplied = false)
{
    if (a == 0)
        return 0;
    if (premultiplied)
        r = std::min(r, a), g = std::min(g, a), b = std::min(b, a);
    else if (a < 255)
        r = (r * a + 127) / 255, g = (g * a + 127) / 255, b = (b * a + 127) / 255;
    return a << 24 | r << 16 | g << 8 | b;
}
// The same for a pixel packed straight as ARGB.
inline uint32_t premultiply(uint32_t c)
{
    return argb(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF, c >> 24);
}
// argb() into `px`, clearing `opaque` for a pixel that isn't.
inline void put_argb(uint32_t* px, uint32_t r, uint32_t g, uint32_t b, uint32_t a, bool& opaque)
{
    opaque = opaque && a == 255;
    *px = argb(r, g, b, a);
}
// The channel a mask picks out of a packed word, scaled to 0..255.
inline uint32_t mask_channel(uint32_t v, uint32_t mask)
{
    if (!mask)
        return 0;
    int shift = 0;
    while (!(mask >> shift & 1))
        ++shift;
    const uint32_t max = mask >> shift;
    return uint32_t((uint64_t((v & mask) >> shift) * 255 + max / 2) / max);
}
// 0..1 to 0..255, clipped; NaN is 0.
inline uint32_t unit_to_byte(float v)
{
    return std::isnan(v) ? 0 : uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}
// sRGB's curve, its power law carried on past 1 (white) for what's
// brighter: linear light to its encoding, and back.
template <typename T>
T srgb_encode(T linear)
{
    return linear <= T(0.0031308) ? linear * T(12.92) : T(1.055) * std::pow(linear, T(1) / T(2.4)) - T(0.055);
}
template <typename T>
T srgb_decode(T v)
{
    return v <= T(0.04045) ? v / T(12.92) : std::pow((v + T(0.055)) / T(1.055), T(2.4));
}
// sRGB's curve to linear light.
inline float srgb_to_linear(float v)
{
    return srgb_decode(v);
}
// Linear light to sRGB's curve, clipped to 0..1; NaN is 0.
inline float linear_to_srgb(float linear)
{
    return std::isnan(linear) ? 0 : srgb_encode(std::clamp(linear, 0.0f, 1.0f));
}

// -- texture layouts ----------------------------------------------------------

// A cube map's page, named: its face (+X, −X, +Y, −Y, +Z, −Z), and which
// cube it's in when there are several (an array of them).
std::string cube_face_label(int page, int cubes);

// -- wider texels -------------------------------------------------------------

// More than 8 bits a channel: kept as the image's source data, so 16-bit
// samples aren't lost and floats past 1 get the exposure control.
struct WideTexels
{
    enum class Kind
    {
        Unorm16,
        Snorm16,
        Half,
        Float,
        B10G11R11,  // unsigned 11/11/10-bit floats in a 32-bit word
        E5B9G9R9,   // three 9-bit mantissas with a shared exponent
    };
    Kind kind;
    int channels;  // R, RG, RGB or RGBA; 3 for the packed kinds

    bool packed() const { return kind == Kind::B10G11R11 || kind == Kind::E5B9G9R9; }
    int sample_bytes() const { return packed() || kind == Kind::Float ? 4 : 2; }  // what a big-endian file swaps
    int pixel_bytes() const { return packed() ? 4 : sample_bytes() * channels; }
    bool floating() const { return kind != Kind::Unorm16 && kind != Kind::Snorm16; }
};

// `h` rows of `w` texels from `d` (`size` bytes), each row padded to
// `row_align` bytes, samples big-endian when `big_endian`, colour multiplied
// by alpha when `premultiplied`. Two channels (RG) are red and green, blue
// left empty. False, with `error`, when the data is short.
bool decode_wide(const WideTexels& f, const uint8_t* d, size_t size, int w, int h, size_t row_align, bool big_endian, bool premultiplied,
                 Image& out, std::string& error);

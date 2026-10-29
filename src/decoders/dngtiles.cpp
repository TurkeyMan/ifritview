#include "dngtiles.h"

#include "../parallel.h"
#include "decoder.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace dngtiles
{
namespace
{

float from_bits(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// A DNG's float sample `bytes` long as the DNG SDK (and LibRaw) widens it:
// 16 bits an IEEE half, 24 the SDK's own (a sign, 7 bits of exponent biased
// by 63, 16 of mantissa), 32 as it is. Infinity becomes the greatest finite
// value, NaN 0.
float widen(uint32_t v, int bytes)
{
    if (bytes == 4)
        return from_bits(v);
    const int m = bytes == 2 ? 10 : 16, top_exponent = bytes == 2 ? 31 : 127, bias = bytes == 2 ? 15 : 63;
    const uint32_t sign = (v >> (bytes * 8 - 1) & 1) << 31, one = 1u << m;
    int exponent = int(v >> m) & top_exponent;
    uint32_t mantissa = v & (one - 1);
    if (exponent == top_exponent)
        return mantissa ? 0 : from_bits(sign | uint32_t(top_exponent - 1 + 127 - bias) << 23 | (one - 1) << (23 - m));
    if (exponent == 0)
    {
        if (mantissa == 0)
            return from_bits(sign);
        while (!(mantissa & one))  // subnormal: normalised
            mantissa <<= 1, --exponent;
        ++exponent;
        mantissa &= one - 1;
    }
    return from_bits(sign | uint32_t(exponent + 127 - bias) << 23 | mantissa << (23 - m));
}

// LibRaw's MAX: NaN where `b` is, unless `a` is greater.
float greater(float a, float b)
{
    return a > b ? a : b;
}

// Tile k's place in the readout, and how much of it is inside.
struct Place
{
    unsigned x, y, cols, rows;
};

Place place(const Prepared& p, size_t k)
{
    const unsigned x = unsigned(k % p.across) * p.tile_w, y = unsigned(k / p.across) * p.tile_h;
    return {x, y, x < p.width ? std::min(p.tile_w, p.width - x) : 0, y < p.height ? std::min(p.tile_h, p.height - y) : 0};
}

// Errors from the tiles, the first said.
bool first_error(const std::vector<std::string>& errors, std::string& error)
{
    for (size_t k = 0; k < errors.size(); ++k)
        if (!errors[k].empty())
            return error = "tile " + std::to_string(k) + ": " + errors[k], false;
    return true;
}

// Tile k's samples, `components` a pixel: a lossy DNG's JPEG through each
// colour's curve; a JPEG XL's as stored, 16 bits (no reference says how
// another depth would be scaled), on threads of the decoder's own when
// `threaded`.
bool tile_samples(const Prepared& p, size_t k, std::vector<uint16_t>& s, int& w, int& h, int& components, std::string& error, bool threaded)
{
    if (p.offsets[k] >= p.size)
        return error = "past the file's end", false;
    const uint8_t* data = p.file + p.offsets[k];
    if (p.format == Format::lossy)
    {
#ifdef HAVE_JPEG
        std::vector<uint8_t> bytes;
        if (!decode_jpeg_samples(data, p.size - p.offsets[k], bytes, w, h, components, error))
            return false;
        if (unsigned(components) != p.samples)
            return error = "a JPEG of " + std::to_string(components) + " components", false;
        s.resize(bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i)
            s[i] = p.curves[i % p.samples * 256 + bytes[i]];
        return true;
#else
        return error = "lossy DNGs need JPEG support", false;
#endif
    }
#ifdef HAVE_JXL
    int bits = 0;
    if (p.lengths[k] > p.size - p.offsets[k])
        return error = "past the file's end", false;
    if (!decode_jxl_samples(data, size_t(p.lengths[k]), s, w, h, components, bits, error, threaded))
        return false;
    if (bits != 16)
        return error = "JPEG XL of " + (bits ? std::to_string(bits) + "-bit integers" : std::string("floats")), false;
    return true;
#else
    (void)threaded;
    return error = "JPEG XL DNGs need JPEG XL support", false;
#endif
}

}  // namespace

bool decode(const Prepared& p, std::vector<uint16_t>& out, std::string& error)
{
    if ((p.format != Format::lossy && p.format != Format::jxl) || (p.format == Format::lossy && p.curves.size() < size_t(p.samples) * 256) ||
        (p.format == Format::jxl && p.lengths.size() != p.offsets.size()))
        return error = "not prepared", false;
    out.assign(size_t(p.width) * p.height * p.samples, 0);
    std::vector<std::string> errors(p.offsets.size());
    // A tile a thread; one alone (a strip) on the decoder's own threads.
    parallel(p.offsets.size(),
             [&](size_t k)
             {
                 std::vector<uint16_t> s;
                 int w = 0, h = 0, components = 0;
                 if (!tile_samples(p, k, s, w, h, components, errors[k], p.offsets.size() == 1))
                     return;
                 if (unsigned(components) != p.samples)
                 {
                     errors[k] = std::to_string(components) + " components";
                     return;
                 }
                 const Place at = place(p, k);
                 const unsigned n = p.samples, rows = std::min(at.rows, unsigned(h)), cols = std::min(at.cols, unsigned(w));
                 for (unsigned y = 0; y < rows; ++y)
                     std::copy_n(s.data() + size_t(y) * w * n, size_t(cols) * n, out.data() + (size_t(at.y + y) * p.width + at.x) * n);
             });
    return first_error(errors, error);
}

bool decode(const Prepared& p, std::vector<float>& out, float& largest, std::string& error)
{
    const unsigned n = p.samples, b = unsigned(p.bytes), row = p.tile_w * n;  // a tile row's samples
    if ((p.format != Format::deflated && p.format != Format::floats) || b < 2 || b > 4 || p.factor < 1 || p.tile_w % p.factor ||
        p.lengths.size() != p.offsets.size())
        return error = "not prepared", false;
    out.assign(size_t(p.width) * p.height * n, 0.f);
    // Each tile row's greatest, folded in LibRaw's order once all are in.
    std::vector<float> greatest(p.offsets.size() * p.tile_h, 0.f);
    std::vector<std::string> errors(p.offsets.size());
    parallel(p.offsets.size(),
             [&](size_t k)
             {
                 const Place at = place(p, k);
                 const size_t tile_bytes = size_t(row) * b * p.tile_h;
                 const uint8_t* data = p.file + p.offsets[k];
                 std::vector<uint8_t> inflated;
                 if (p.format == Format::deflated)
                 {
                     inflated.resize(tile_bytes);
                     uLongf length = uLongf(tile_bytes);
                     if (p.offsets[k] > p.size || p.lengths[k] > p.size - p.offsets[k] ||
                         uncompress(inflated.data(), &length, data, uLong(p.lengths[k])) != Z_OK || length != tile_bytes)
                     {
                         errors[k] = "not deflated, or short";
                         return;
                     }
                     data = inflated.data();
                 }
                 else if (p.offsets[k] > p.size || size_t(row) * b * at.rows > p.size - p.offsets[k])
                 {
                     errors[k] = "past the file's end";
                     return;
                 }
                 std::vector<uint8_t> bytes(size_t(row) * b);
                 for (unsigned y = 0; y < at.rows; ++y)
                 {
                     const uint8_t* r = data + size_t(y) * row * b;
                     float* o = out.data() + (size_t(at.y + y) * p.width + at.x) * n;
                     float& most = greatest[k * p.tile_h + y];
                     // A sample's bytes: deflated, in planes (its most
                     // significant byte's plane first) each summed across,
                     // `factor` samples' worth of columns a step; as they
                     // are, in the file's order.
                     if (p.format == Format::deflated)
                     {
                         const unsigned stride = n * unsigned(p.factor);
                         std::copy_n(r, bytes.size(), bytes.begin());
                         for (size_t i = stride; i < bytes.size(); ++i)
                             bytes[i] = uint8_t(bytes[i] + bytes[i - stride]);
                     }
                     for (unsigned x = 0; x < row; ++x)
                     {
                         uint32_t v = 0;
                         for (unsigned j = 0; j < b; ++j)
                             v = v << 8 | (p.format == Format::deflated ? bytes[size_t(j) * row + x] : r[size_t(x) * b + (p.big_endian ? j : b - 1 - j)]);
                         const float f = widen(v, int(b));
                         most = greater(most, f);
                         if (x < at.cols * n)
                             o[x] = f;
                     }
                 }
             });
    if (!first_error(errors, error))
        return false;
    largest = 0.f;
    for (size_t k = 0; k < p.offsets.size(); ++k)
        for (unsigned y = 0; y < place(p, k).rows; ++y)
            largest = greater(largest, greatest[k * p.tile_h + y]);
    return true;
}

}  // namespace dngtiles

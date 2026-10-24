#include "texel.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace
{

// An unsigned float of `bits` mantissa bits and a 5-bit exponent (the packed
// 11- and 10-bit kinds): no sign, no NaN shown.
float small_float(uint32_t v, int bits)
{
    const uint32_t exp = v >> bits, mant = v & ((1u << bits) - 1);
    if (exp == 0)
        return std::ldexp(float(mant), -14 - bits);
    if (exp == 31)
        return mant ? 0.0f : 65024.0f;
    return std::ldexp(float(mant | 1u << bits), int(exp) - 15 - bits);
}

}  // namespace

bool decode_wide(const WideTexels& f, const uint8_t* d, size_t size, int w, int h, size_t row_align, bool big_endian, bool premultiplied,
                 Image& out, std::string& error)
{
    using S = SourceData::Sample;
    using K = WideTexels::Kind;
    const size_t align = row_align ? row_align : 1;
    const size_t row = (size_t(w) * f.pixel_bytes() + align - 1) / align * align;
    if (w <= 0 || h <= 0 || size / row < size_t(h))
    {
        error = "texture data is cut short";
        return false;
    }
    const bool alpha = !f.packed() && f.channels == 4;
    // As stored where that's all it takes; floats where the texels need
    // working out (packed kinds, alpha to unmultiply).
    const bool as_stored = !f.packed() && !(premultiplied && alpha);
    const S sample = !as_stored ? S::F32
                     : f.kind == K::Float   ? S::F32
                     : f.kind == K::Half    ? S::F16
                     : f.kind == K::Snorm16 ? S::I16
                                            : S::U16;
    const int channels = f.packed() || f.channels == 2 ? 3 : f.channels;  // RG gains an empty blue, not read as grey + alpha
    auto deep = std::make_shared<SourceData>(sample, channels, w, h, f.floating());
    const int sb = f.sample_bytes();
    auto read = [&](const uint8_t* p)
    {
        uint32_t v = 0;
        for (int k = 0; k < sb; ++k)
            v |= uint32_t(p[big_endian ? sb - 1 - k : k]) << (8 * k);
        return v;
    };
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* s = d + size_t(y) * row;
        for (int x = 0; x < w; ++x, s += f.pixel_bytes())
        {
            uint8_t* px = deep->pixel(x, y);
            if (as_stored)
            {
                // Each sample in native order; RG's missing blue is zero.
                std::memset(px, 0, deep->pixel_bytes());
                for (int c = 0; c < f.channels; ++c)
                {
                    const uint32_t v = read(s + c * sb);
                    std::memcpy(px + c * sb, &v, size_t(sb));  // little-endian machines: the low bytes
                }
                continue;
            }
            float c[4] = {0, 0, 0, 1};
            if (f.kind == K::B10G11R11)
            {
                const uint32_t v = read(s);
                c[0] = small_float(v & 0x7FF, 6), c[1] = small_float(v >> 11 & 0x7FF, 6), c[2] = small_float(v >> 22, 5);
            }
            else if (f.kind == K::E5B9G9R9)
            {
                const uint32_t v = read(s);
                const int exp = int(v >> 27) - 15 - 9;
                c[0] = std::ldexp(float(v & 0x1FF), exp), c[1] = std::ldexp(float(v >> 9 & 0x1FF), exp), c[2] = std::ldexp(float(v >> 18 & 0x1FF), exp);
            }
            else
            {
                // Premultiplied with alpha: read as the source would, then unmultiplied.
                SourceData as_is(f.kind == K::Float ? S::F32 : f.kind == K::Half ? S::F16 : f.kind == K::Snorm16 ? S::I16 : S::U16, 4, 1, 1, f.floating());
                for (int i = 0; i < 4; ++i)
                {
                    const uint32_t v = read(s + i * sb);
                    std::memcpy(as_is.data.data() + i * sb, &v, size_t(sb));
                }
                for (int i = 0; i < 4; ++i)
                    c[i] = as_is.value(as_is.data.data(), i);
                if (c[3] > 0)
                    for (int i = 0; i < 3; ++i)
                        c[i] /= c[3];
            }
            std::memcpy(px, c, size_t(channels) * 4);
        }
    }
    out.set_source(std::move(deep));
    return true;
}

// BCn surfaces block by block through bcdec, into Image's premultiplied
// 0xAARRGGBB. BC4/BC5 go through bcdec's precise float path, which also
// reads their signed forms; BC6H's HDR values, half floats of linear light,
// are kept as the image's source and rendered from there (Image::set_source).
#include "bcn.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"  // the variants not used here
#endif
#include "../../third_party/bcdec/bcdec.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace
{

uint32_t to_byte(float v)
{
    return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace

const char* bcn_name(Bcn format)
{
    switch (format)
    {
        case Bcn::BC1: return "BC1 (DXT1)";
        case Bcn::BC2: return "BC2 (DXT3)";
        case Bcn::BC3: return "BC3 (DXT5)";
        case Bcn::BC4: return "BC4 (ATI1)";
        case Bcn::BC4S: return "BC4 signed";
        case Bcn::BC5: return "BC5 (ATI2)";
        case Bcn::BC5S: return "BC5 signed";
        case Bcn::BC6H: return "BC6H";
        case Bcn::BC6HS: return "BC6H signed";
        case Bcn::BC7: return "BC7";
    }
    return "BCn";
}

size_t bcn_block_bytes(Bcn format)
{
    return format == Bcn::BC1 || format == Bcn::BC4 || format == Bcn::BC4S ? 8 : 16;
}

size_t bcn_size(Bcn format, int w, int h)
{
    return size_t((w + 3) / 4) * size_t((h + 3) / 4) * bcn_block_bytes(format);
}

bool decode_bcn(Bcn format, const uint8_t* data, size_t size, int w, int h, Image& out, std::string& error, bool premultiplied)
{
    if (w <= 0 || h <= 0 || size < bcn_size(format, w, h))
    {
        error = std::string(bcn_name(format)) + " data is cut short";
        return false;
    }
    out.allocate(w, h);
    const size_t block_bytes = bcn_block_bytes(format);
    const int blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
    uint8_t rgba[4 * 4 * 4];
    float f[4 * 4 * 2];
    bool opaque = true;
    const bool hdr = format == Bcn::BC6H || format == Bcn::BC6HS;
    if (hdr)
    {
        auto deep = std::make_shared<SourceData>(SourceData::Sample::F16, 3, w, h, true);
        uint16_t halves[4 * 4 * 3];
        for (int by = 0; by < blocks_y; ++by)
            for (int bx = 0; bx < blocks_x; ++bx)
            {
                bcdec_bc6h_half(data + (size_t(by) * blocks_x + bx) * block_bytes, halves, 12, format == Bcn::BC6HS);
                for (int y = 0; y < 4 && by * 4 + y < h; ++y)
                    for (int x = 0; x < 4 && bx * 4 + x < w; ++x)
                        std::memcpy(deep->pixel(bx * 4 + x, by * 4 + y), halves + (y * 4 + x) * 3, 6);
            }
        out.set_source(std::move(deep));
        return true;
    }
    for (int by = 0; by < blocks_y; ++by)
        for (int bx = 0; bx < blocks_x; ++bx)
        {
            const uint8_t* block = data + (size_t(by) * blocks_x + bx) * block_bytes;
            switch (format)
            {
                case Bcn::BC1: bcdec_bc1(block, rgba, 16); break;
                case Bcn::BC2: bcdec_bc2(block, rgba, 16); break;
                case Bcn::BC3: bcdec_bc3(block, rgba, 16); break;
                case Bcn::BC7: bcdec_bc7(block, rgba, 16); break;
                case Bcn::BC4:
                case Bcn::BC4S: bcdec_bc4_float(block, f, 4, format == Bcn::BC4S); break;
                case Bcn::BC5:
                case Bcn::BC5S: bcdec_bc5_float(block, f, 8, format == Bcn::BC5S); break;
                case Bcn::BC6H:
                case Bcn::BC6HS: break;  // above
            }
            for (int y = 0; y < 4 && by * 4 + y < h; ++y)
            {
                uint32_t* px = out.row(by * 4 + y) + bx * 4;
                for (int x = 0; x < 4 && bx * 4 + x < w; ++x)
                {
                    const int i = y * 4 + x;
                    // Signed channels span -1..1: shown with 0 at mid-grey.
                    const auto channel = [&](float v, bool is_signed) { return to_byte(is_signed ? (v + 1) / 2 : v); };
                    uint32_t r, g, b, a = 255;
                    switch (format)
                    {
                        case Bcn::BC4:
                        case Bcn::BC4S:
                            r = g = b = channel(f[i], format == Bcn::BC4S);
                            break;
                        case Bcn::BC5:
                        case Bcn::BC5S:
                            r = channel(f[i * 2], format == Bcn::BC5S);
                            g = channel(f[i * 2 + 1], format == Bcn::BC5S);
                            b = format == Bcn::BC5S ? 128 : 0;  // the missing channel at zero, as signed ones show it
                            break;
                        default:
                            r = rgba[i * 4];
                            g = rgba[i * 4 + 1];
                            b = rgba[i * 4 + 2];
                            a = rgba[i * 4 + 3];
                            if (premultiplied)
                            {
                                r = std::min(r, a);  // a stored colour can't outshine its alpha
                                g = std::min(g, a);
                                b = std::min(b, a);
                            }
                            else if (a < 255)
                            {
                                r = (r * a + 127) / 255;
                                g = (g * a + 127) / 255;
                                b = (b * a + 127) / 255;
                            }
                            break;
                    }
                    opaque = opaque && a == 255;
                    px[x] = a << 24 | r << 16 | g << 8 | b;
                }
            }
        }
    out.has_alpha = !opaque;
    return true;
}

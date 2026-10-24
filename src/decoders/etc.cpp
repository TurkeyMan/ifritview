// ETC1/ETC2/EAC surfaces block by block through etcdec, into Image's
// premultiplied 0xAARRGGBB. Unsigned EAC goes through etcdec's 16-bit path;
// signed EAC is decoded here, etcdec reading its blocks as unsigned ones.
#include "etc.h"

#define ETCDEC_IMPLEMENTATION
#define ETCDEC_STATIC
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"  // the variants not used here
#endif
#include "../../third_party/etcdec/etcdec.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstring>

namespace
{

// One signed EAC channel block (OpenGL ES 3.0, C.1.5): values in -1023..1023,
// row by row.
void eac_signed(const uint8_t* b, int out[16])
{
    static const int8_t modifiers[16][8] = {
        {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
        {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
        {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
        {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
        {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
        {-3, -5, -7, -9, 2, 4, 6, 8},
    };
    const int base = std::max(int(int8_t(b[0])), -127);  // -128 reads as -127
    const int multiplier = b[1] >> 4;
    const int8_t* table = modifiers[b[1] & 0xF];
    uint64_t indices = 0;
    for (int i = 2; i < 8; ++i)
        indices = indices << 8 | b[i];
    // 3-bit indices, most significant first, running down each column.
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y)
        {
            const int m = table[(indices >> (45 - (x * 4 + y) * 3)) & 7];
            out[y * 4 + x] = std::clamp(base * 8 + (multiplier ? m * multiplier * 8 : m), -1023, 1023);
        }
}

uint32_t from16(uint16_t v)
{
    return (uint32_t(v) * 255 + 32767) / 65535;
}

// Signed channels span -1..1: shown with 0 at mid-grey.
uint32_t from_signed(int v)
{
    return uint32_t((v + 1023) * 255 + 1023) / 2046;
}

}  // namespace

const char* etc_name(Etc format)
{
    switch (format)
    {
        case Etc::ETC1: return "ETC1";
        case Etc::ETC2_RGB: return "ETC2 RGB";
        case Etc::ETC2_RGBA1: return "ETC2 RGB A1";
        case Etc::ETC2_RGBA: return "ETC2 RGBA";
        case Etc::EAC_R11: return "EAC R11";
        case Etc::EAC_R11S: return "EAC R11 signed";
        case Etc::EAC_RG11: return "EAC RG11";
        case Etc::EAC_RG11S: return "EAC RG11 signed";
    }
    return "ETC";
}

size_t etc_block_bytes(Etc format)
{
    return format == Etc::ETC2_RGBA || format == Etc::EAC_RG11 || format == Etc::EAC_RG11S ? 16 : 8;
}

size_t etc_size(Etc format, int w, int h)
{
    return size_t((w + 3) / 4) * size_t((h + 3) / 4) * etc_block_bytes(format);
}

PixelFormat etc_pixel_format(Etc format)
{
    using M = PixelFormat::Model;
    PixelFormat f;
    switch (format)
    {
        case Etc::ETC1:
        case Etc::ETC2_RGB: f = PixelFormat::packed(M::RGB, "RGB", 8); break;
        case Etc::ETC2_RGBA1: f = PixelFormat::widths(M::RGB, "RGBA", {8, 8, 8, 1}); break;
        case Etc::ETC2_RGBA: f = PixelFormat::packed(M::RGB, "RGBA", 8); break;
        case Etc::EAC_R11:
        case Etc::EAC_R11S: f = PixelFormat::packed(M::L, "R", 11); break;
        case Etc::EAC_RG11:
        case Etc::EAC_RG11S: f = PixelFormat::packed(M::RGB, "RG", 11); break;
    }
    if (format == Etc::EAC_R11S || format == Etc::EAC_RG11S)
        f.sample = PixelFormat::Sample::Signed;
    f.bits_per_pixel = int(etc_block_bytes(format)) * 8 / 16;
    f.notes.push_back(std::string(etc_name(format)) + " block compression");
    return f;
}

bool decode_etc(Etc format, const uint8_t* data, size_t size, int w, int h, Image& out, std::string& error, bool premultiplied)
{
    if (w <= 0 || h <= 0 || size < etc_size(format, w, h))
    {
        error = std::string(etc_name(format)) + " data is cut short";
        return false;
    }
    out.allocate(w, h);
    const size_t block_bytes = etc_block_bytes(format);
    const int blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
    // etcdec reads its blocks as 64-bit words: copied out to be aligned.
    alignas(8) uint8_t block[16];
    uint8_t rgba[4 * 4 * 4];
    uint16_t u16[4 * 4 * 2];
    int s[2][16];
    bool opaque = true;
    for (int by = 0; by < blocks_y; ++by)
        for (int bx = 0; bx < blocks_x; ++bx)
        {
            std::memcpy(block, data + (size_t(by) * blocks_x + bx) * block_bytes, block_bytes);
            switch (format)
            {
                case Etc::ETC1:  // ETC2 RGB is a superset
                case Etc::ETC2_RGB: etcdec_etc_rgb(block, rgba, 16); break;
                case Etc::ETC2_RGBA1: etcdec_etc_rgb_a1(block, rgba, 16); break;
                case Etc::ETC2_RGBA: etcdec_eac_rgba(block, rgba, 16); break;
                case Etc::EAC_R11: etcdec_eac_r11_u16(block, u16, 8); break;
                case Etc::EAC_RG11: etcdec_eac_rg11_u16(block, u16, 16); break;
                case Etc::EAC_R11S: eac_signed(block, s[0]); break;
                case Etc::EAC_RG11S:
                    eac_signed(block, s[0]);
                    eac_signed(block + 8, s[1]);
                    break;
            }
            for (int y = 0; y < 4 && by * 4 + y < h; ++y)
            {
                uint32_t* px = out.row(by * 4 + y) + bx * 4;
                for (int x = 0; x < 4 && bx * 4 + x < w; ++x)
                {
                    const int i = y * 4 + x;
                    uint32_t r, g, b, a = 255;
                    switch (format)
                    {
                        case Etc::EAC_R11: r = g = b = from16(u16[i]); break;
                        case Etc::EAC_R11S: r = g = b = from_signed(s[0][i]); break;
                        case Etc::EAC_RG11:
                            r = from16(u16[i * 2]);
                            g = from16(u16[i * 2 + 1]);
                            b = 0;
                            break;
                        case Etc::EAC_RG11S:
                            r = from_signed(s[0][i]);
                            g = from_signed(s[1][i]);
                            b = 128;  // the missing channel at zero, as signed ones show it
                            break;
                        default:
                            r = rgba[i * 4];
                            g = rgba[i * 4 + 1];
                            b = rgba[i * 4 + 2];
                            a = format == Etc::ETC1 || format == Etc::ETC2_RGB ? 255 : rgba[i * 4 + 3];
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

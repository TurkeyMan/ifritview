// ASTC surfaces block by block through the decoder from Android's dEQP (as
// kept in Basis Universal), into Image's premultiplied 0xAARRGGBB. LDR
// blocks decode to 8 bits as the decode_unorm8 extension specifies; HDR
// ones to linear floats, clipped to 0..1 and given the sRGB curve.
#include "astc.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/astc/android_astc_decomp.cpp"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace
{

constexpr uint32_t kErrorColour = 0xFFFF00FFu;  // magenta, as the format specifies for a bad block

uint32_t to_byte(float v)
{
    return std::isnan(v) ? 0 : uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

float srgb(float linear)
{
    if (std::isnan(linear))
        return 0;
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
}

}  // namespace

bool astc_footprint_valid(int block_w, int block_h)
{
    switch (block_w * 100 + block_h)
    {
        case 404: case 504: case 505: case 605: case 606: case 805: case 806: case 808:
        case 1005: case 1006: case 1008: case 1010: case 1210: case 1212:
            return true;
    }
    return false;
}

size_t astc_size(int block_w, int block_h, int w, int h)
{
    return size_t((w + block_w - 1) / block_w) * size_t((h + block_h - 1) / block_h) * 16;
}

PixelFormat astc_pixel_format(int block_w, int block_h, bool hdr)
{
    PixelFormat f = PixelFormat::packed(PixelFormat::Model::RGB, "RGBA", hdr ? 16 : 8);
    if (hdr)
        f.sample = PixelFormat::Sample::Float;
    f.notes.push_back("ASTC " + std::to_string(block_w) + "x" + std::to_string(block_h) + " block compression");
    return f;
}

bool decode_astc(const uint8_t* data, size_t size, int block_w, int block_h, int w, int h, AstcProfile profile, Image& out,
                 std::string& error, bool premultiplied)
{
    if (!astc_footprint_valid(block_w, block_h))
    {
        error = "ASTC block size " + std::to_string(block_w) + "x" + std::to_string(block_h) + " isn't valid";
        return false;
    }
    if (w <= 0 || h <= 0 || size < astc_size(block_w, block_h, w, h))
    {
        error = "ASTC data is cut short";
        return false;
    }
    const int blocks_x = (w + block_w - 1) / block_w, blocks_y = (h + block_h - 1) / block_h;
    const size_t blocks = size_t(blocks_x) * blocks_y;
    if (profile == AstcProfile::Auto)
    {
        // Nothing marks an HDR surface but its blocks' endpoint modes.
        profile = AstcProfile::LDR;
        for (size_t i = 0; i < blocks; ++i)
        {
            bool hdr = false;
            if (basisu_astc::astc::is_hdr(data + i * 16, block_w, block_h, hdr) && hdr)
            {
                profile = AstcProfile::HDR;
                break;
            }
        }
    }
    const bool hdr = profile == AstcProfile::HDR;
    out.allocate(w, h);
    uint8_t rgba[12 * 12 * 4];
    float f[12 * 12 * 4];
    bool opaque = true;
    for (int by = 0; by < blocks_y; ++by)
        for (int bx = 0; bx < blocks_x; ++bx)
        {
            const uint8_t* block = data + (size_t(by) * blocks_x + bx) * 16;
            const bool ok = hdr ? basisu_astc::astc::decompress_hdr(f, block, block_w, block_h, true)
                                : basisu_astc::astc::decompress_ldr(rgba, block, profile == AstcProfile::sRGB, block_w, block_h, true);
            for (int y = 0; y < block_h && by * block_h + y < h; ++y)
            {
                uint32_t* px = out.row(by * block_h + y) + bx * block_w;
                for (int x = 0; x < block_w && bx * block_w + x < w; ++x)
                {
                    if (!ok)
                    {
                        px[x] = kErrorColour;
                        continue;
                    }
                    const int i = (y * block_w + x) * 4;
                    uint32_t r, g, b, a;
                    if (hdr)
                    {
                        r = to_byte(srgb(f[i]));
                        g = to_byte(srgb(f[i + 1]));
                        b = to_byte(srgb(f[i + 2]));
                        a = to_byte(f[i + 3]);
                    }
                    else
                    {
                        r = rgba[i];
                        g = rgba[i + 1];
                        b = rgba[i + 2];
                        a = rgba[i + 3];
                    }
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
                    opaque = opaque && a == 255;
                    px[x] = a << 24 | r << 16 | g << 8 | b;
                }
            }
        }
    out.has_alpha = !opaque;
    out.bits_per_sample = hdr ? 16 : 8;
    return true;
}

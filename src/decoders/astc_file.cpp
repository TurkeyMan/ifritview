// .astc, ARM's container for one ASTC surface: a 16-byte header (the magic
// 0x5CA1AB13, the block footprint in x, y and z, then the size as three
// 24-bit little-endian numbers), then the blocks, decoded through astc.cpp.
// Whether a file is LDR or HDR is told by its blocks; a volume (z > 1)
// shows its first slice when its blocks are 2D, and isn't supported when
// they're 3D.
#include "astc.h"
#include "decoder.h"

#include <string>

namespace
{

uint32_t le24(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
}

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 4 && h[0] == 0x13 && h[1] == 0xAB && h[2] == 0xA1 && h[3] == 0x5C;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 16 || !sniff(d, size))
    {
        error = "not an ASTC file";
        return false;
    }
    const int bw = d[4], bh = d[5], bd = d[6];
    const uint32_t w = le24(d + 7), h = le24(d + 10), depth = le24(d + 13);
    if (bd != 1)
    {
        error = "3D ASTC blocks (" + std::to_string(bw) + "x" + std::to_string(bh) + "x" + std::to_string(bd) + ") aren't supported";
        return false;
    }
    if (depth == 0 || !Image::fits(w, h))
    {
        error = "bad ASTC size";
        return false;
    }
    if (!decode_astc(d + 16, size - 16, bw, bh, int(w), int(h), AstcProfile::Auto, out, error))
        return false;

    out.format = "ASTC";
    const std::string footprint = std::to_string(bw) + "x" + std::to_string(bh);
    out.pixel_format = astc_pixel_format(bw, bh, out.bits_per_sample > 8);
    out.add("ASTC", "Block size", footprint);
    out.add("ASTC", "Profile", out.bits_per_sample > 8 ? "HDR" : "LDR");
    if (depth > 1)
        out.add("ASTC", "Volume", std::to_string(depth) + " slices, the first shown");
    return true;
}

}  // namespace

const Decoder& astc_decoder()
{
    static const Decoder d{"ASTC", {"astc"}, sniff, decode};
    return d;
}

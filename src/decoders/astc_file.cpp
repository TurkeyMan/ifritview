// .astc, ARM's container for one ASTC surface: a 16-byte header (the magic
// 0x5CA1AB13, the block footprint in x, y and z, then the size as three
// 24-bit little-endian numbers), then the blocks, decoded through astc.cpp.
// Whether a file is LDR or HDR is told by its blocks. A volume (z > 1)
// with 2D blocks holds its slices one after another, and each is a page;
// one with 3D blocks isn't supported.
#include "astc.h"
#include "../i18n.h"
#include "../bytes.h"
#include "decoder.h"

#include <algorithm>
#include <string>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 4 && h[0] == 0x13 && h[1] == 0xAB && h[2] == 0xA1 && h[3] == 0x5C;
}

bool decode_part(const std::string& path, ImagePart part, Image& out, std::string& error)
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
    out.page_count = int(std::min<uint32_t>(depth, 1 << 16));
    if (part.page >= out.page_count)
    {
        error = "the ASTC file has no such page";
        return false;
    }
    // Every slice is whole blocks, so each starts a fixed stride after the
    // last. A footprint the format doesn't define has no stride; decode_astc
    // turns it down.
    const uint64_t slice = astc_footprint_valid(bw, bh) ? astc_size(bw, bh, int(w), int(h)) : 0;
    const uint64_t at = 16 + slice * uint64_t(part.page);
    if (at > size)
    {
        error = "ASTC data is cut short";
        return false;
    }
    if (!decode_astc(d + at, size - size_t(at), bw, bh, int(w), int(h), AstcProfile::Auto, out, error))
        return false;

    out.format = "ASTC";
    const std::string footprint = std::to_string(bw) + "x" + std::to_string(bh);
    out.pixel_format = astc_pixel_format(bw, bh, out.bits_per_sample > 8);
    out.add(N_("ASTC"), N_("Block size"), footprint);
    out.add(N_("ASTC"), N_("Profile"), out.bits_per_sample > 8 ? "HDR" : "LDR");
    if (depth > 1)
        out.add(N_("ASTC"), N_("Volume"), rap::i18n::format(_("{0} slices"), depth));
    return true;
}

}  // namespace

const Decoder& astc_decoder()
{
    static const Decoder d{"ASTC", {"astc"}, sniff, [](const std::string& path, Image& out, std::string& error)
                           { return decode_part(path, {}, out, error); }, nullptr, false, decode_part};
    return d;
}

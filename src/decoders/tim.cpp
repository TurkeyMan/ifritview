// PlayStation (PS1) TIM textures. No library needed: a flags word, an
// optional CLUT block of 16-bit colours, then the pixel block with its VRAM
// position. 16-bit colours are BGR 5-5-5 with an STP (semi-transparency)
// bit; colour 0x0000 is fully transparent, 0x8000 opaque black.
#include "decoder.h"
#include "../i18n.h"
#include "../bytes.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    if (n < 8 || le32(h) != 0x10)
        return false;
    const uint32_t flags = le32(h + 4);
    return (flags & ~0xFu) == 0 && (flags & 7) <= 4;
}

uint32_t colour15(uint16_t c, bool& stp)
{
    stp = (c & 0x8000) != 0;
    const uint32_t r = (c & 0x1F) * 255 / 31, g = ((c >> 5) & 0x1F) * 255 / 31, b = ((c >> 10) & 0x1F) * 255 / 31;
    if (c == 0)
        return 0;  // transparent
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    std::vector<uint8_t> d;
    if (FILE* f = open_file(path))
    {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        d.resize(n > 0 ? size_t(n) : 0);
        if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size())
            d.clear();
        std::fclose(f);
    }
    if (d.size() < 8 || !sniff(d.data(), d.size()))
    {
        error = "not a TIM file";
        return false;
    }
    const uint32_t flags = le32(d.data() + 4);
    const int mode = flags & 7;
    const bool has_clut = flags & 8;
    size_t pos = 8;

    std::vector<uint32_t> clut;
    int clut_entries = 0, clut_count = 0;
    if (has_clut)
    {
        if (pos + 12 > d.size())
        {
            error = "truncated CLUT";
            return false;
        }
        const uint32_t size = le32(d.data() + pos);
        const int cx = le16(d.data() + pos + 4), cy = le16(d.data() + pos + 6);
        clut_entries = le16(d.data() + pos + 8);
        clut_count = le16(d.data() + pos + 10);
        if (pos + size > d.size() || size < 12 + size_t(clut_entries) * 2)
        {
            error = "bad CLUT block";
            return false;
        }
        for (int i = 0; i < clut_entries; ++i)
        {  // first palette only
            bool stp;
            clut.push_back(colour15(le16(d.data() + pos + 12 + i * 2), stp));
        }
        out.add(N_("TIM"), N_("CLUT"),
                std::to_string(clut_entries) + " entries × " + std::to_string(clut_count) + (clut_count == 1 ? " palette" : " palettes") + ", VRAM (" +
                    std::to_string(cx) + ", " + std::to_string(cy) + ")");
        pos += size;
    }
    if (pos + 12 > d.size())
    {
        error = "truncated image block";
        return false;
    }
    const int ix = le16(d.data() + pos + 4), iy = le16(d.data() + pos + 6);
    const int w16 = le16(d.data() + pos + 8), h = le16(d.data() + pos + 10);
    const uint8_t* px = d.data() + pos + 12;
    const size_t avail = d.size() - (pos + 12);
    const int width = mode == 0 ? w16 * 4 : mode == 1 ? w16 * 2 : mode == 3 ? w16 * 2 / 3 : w16;
    if (width <= 0 || h <= 0 || size_t(w16) * 2 * h > avail)
    {
        error = "bad image block";
        return false;
    }

    out.allocate(width, h);
    bool any_stp = false;
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = px + size_t(y) * w16 * 2;
        uint32_t* o = out.row(y);
        for (int x = 0; x < width; ++x)
        {
            uint32_t c = 0;
            bool stp = false;
            switch (mode)
            {
                case 0:
                {
                    const int idx = (row[x >> 1] >> ((x & 1) * 4)) & 0xF;
                    c = idx < int(clut.size()) ? clut[idx] : 0xFF000000u | (idx * 17 * 0x010101);
                    break;
                }
                case 1:
                {
                    const int idx = row[x];
                    c = idx < int(clut.size()) ? clut[idx] : 0xFF000000u | (idx * 0x010101);
                    break;
                }
                case 2:
                    c = colour15(le16(row + x * 2), stp);
                    break;
                case 3:
                    c = 0xFF000000u | (uint32_t(row[x * 3]) << 16) | (uint32_t(row[x * 3 + 1]) << 8) | row[x * 3 + 2];
                    break;
                default:
                    c = 0xFF000000u;
                    break;
            }
            any_stp |= stp;
            o[x] = c;
        }
    }
    out.has_alpha = mode <= 2;  // index 0 / colour 0 are transparent
    out.bits_per_sample = mode == 3 ? 8 : mode == 2 ? 5 : mode == 1 ? 8 : 4;
    out.format = "PlayStation TIM";
    using M = PixelFormat::Model;
    if (mode == 0 || mode == 1)
        out.pixel_format = PixelFormat::palette(mode == 0 ? 4 : 8, clut_entries);
    else if (mode == 2)
    {
        out.pixel_format = PixelFormat::widths(M::RGB, "XBGR", {1, 5, 5, 5}, 16);
        out.pixel_format.notes.push_back("STP bit");
    }
    else
        out.pixel_format = PixelFormat::packed(M::RGB, "RGB", 8);
    out.add(N_("TIM"), N_("Mode"), mode == 0 ? "4-bit CLUT" : mode == 1 ? "8-bit CLUT" : mode == 2 ? "16-bit direct" : mode == 3 ? "24-bit direct" : "mixed");
    out.add(N_("TIM"), N_("VRAM position"), "(" + std::to_string(ix) + ", " + std::to_string(iy) + ")");
    if (any_stp)
        out.add(N_("TIM"), N_("Semi-transparent pixels"), "yes");
    return true;
}

}  // namespace

const Decoder& tim_decoder()
{
    static const Decoder d{"TIM", {"tim"}, sniff, decode};
    return d;
}

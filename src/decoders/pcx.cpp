// ZSoft PCX: a 128-byte header, RLE scanlines stored plane by plane, and
// for 256-colour files a VGA palette trailing the data. Handles 1-bit
// mono, 2/4-plane EGA (16 colours from the header palette), 8-bit palette
// and 24-bit (three 8-bit planes).
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
    return n >= 128 && h[0] == 0x0A && h[1] <= 5 && (h[2] == 0 || h[2] == 1) && (h[3] == 1 || h[3] == 2 || h[3] == 4 || h[3] == 8);
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
    if (!sniff(d.data(), d.size()))
    {
        error = "not a PCX file";
        return false;
    }
    const int version = d[1], rle = d[2], bpp = d[3];
    const int xmin = le16(d.data() + 4), ymin = le16(d.data() + 6), xmax = le16(d.data() + 8), ymax = le16(d.data() + 10);
    const int w = xmax - xmin + 1, h = ymax - ymin + 1, planes = d[65], bytes_per_line = le16(d.data() + 66);
    if (w <= 0 || h <= 0 || planes < 1 || planes > 4 || bytes_per_line <= 0)
    {
        error = "bad PCX header";
        return false;
    }
    const int bits = bpp * planes;

    // Decode the RLE stream into rows of planes*bytes_per_line bytes.
    const size_t row_bytes = size_t(planes) * bytes_per_line;
    std::vector<uint8_t> raw(row_bytes * h, 0);
    size_t pos = 128, o = 0;
    while (o < raw.size() && pos < d.size())
    {
        uint8_t v = d[pos++];
        int count = 1;
        if (rle && (v & 0xC0) == 0xC0)
        {
            count = v & 0x3F;
            if (pos >= d.size())
                break;
            v = d[pos++];
        }
        for (int k = 0; k < count && o < raw.size(); ++k)
            raw[o++] = v;
    }

    std::vector<uint32_t> palette;
    if (bits == 8)
    {
        uint32_t vga[256];
        if (pcx_vga_palette(d.data(), d.size(), vga))
            palette.assign(vga, vga + 256);
        else
            for (int i = 0; i < 256; ++i)
                palette.push_back(0xFF000000u | uint32_t(i) * 0x010101);  // grey ramp fallback
    }
    else if (bits <= 4)
    {
        if (bits == 1)
        {
            palette = {0xFF000000u, 0xFFFFFFFFu};
        }
        else
        {
            for (int i = 0; i < 16; ++i)
                palette.push_back(0xFF000000u | (uint32_t(d[16 + i * 3]) << 16) | (uint32_t(d[17 + i * 3]) << 8) | d[18 + i * 3]);
        }
    }

    out.allocate(w, h);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = raw.data() + size_t(y) * row_bytes;
        uint32_t* px = out.row(y);
        for (int x = 0; x < w; ++x)
        {
            uint32_t c;
            if (bits == 24)
            {
                c = 0xFF000000u | (uint32_t(row[x]) << 16) | (uint32_t(row[bytes_per_line + x]) << 8) | row[2 * bytes_per_line + x];
            }
            else if (bits == 8)
            {
                c = palette[row[x]];
            }
            else if (bpp == 1)
            {  // one bit per plane, planes combine into the index
                uint32_t idx = 0;
                for (int p = 0; p < planes; ++p)
                    if (row[p * bytes_per_line + (x >> 3)] & (0x80 >> (x & 7)))
                        idx |= 1u << p;
                c = idx < palette.size() ? palette[idx] : 0xFF000000u;
            }
            else
            {  // 2 or 4 bits per pixel, single plane
                const int per_byte = 8 / bpp, shift = (per_byte - 1 - (x % per_byte)) * bpp;
                const uint32_t idx = (row[x / per_byte] >> shift) & ((1u << bpp) - 1);
                c = idx < palette.size() ? palette[idx] : 0xFF000000u;
            }
            px[x] = c;
        }
    }
    out.bits_per_sample = bits == 24 ? 8 : bits;
    out.format = "PCX";
    using M = PixelFormat::Model;
    if (bits == 24)
    {
        out.pixel_format = PixelFormat::packed(M::RGB, "RGB", 8);
        out.pixel_format.planar = true;
    }
    else if (bits == 1)
        out.pixel_format = PixelFormat::packed(M::L, "L", 1);
    else
    {
        out.pixel_format = PixelFormat::palette(bits, int(palette.size()));
        if (planes > 1)
            out.pixel_format.planar = true;
    }
    if (rle)
        out.pixel_format.notes.push_back("RLE");
    out.add(N_("PCX"), N_("Version"), std::to_string(version));
    out.add(N_("PCX"), N_("Planes × bits"), std::to_string(planes) + " × " + std::to_string(bpp));
    const int hdpi = le16(d.data() + 12), vdpi = le16(d.data() + 14);
    if (hdpi && vdpi)
        out.add(N_("PCX"), N_("Resolution"), std::to_string(hdpi) + " × " + std::to_string(vdpi) + " dpi");
    return true;
}

}  // namespace

bool pcx_vga_palette(const uint8_t* d, size_t n, uint32_t palette[256])
{
    // A 0x0C marker then 768 bytes of RGB at the end of the file.
    if (!sniff(d, n) || n < 128 + 769 || d[n - 769] != 0x0C)
        return false;
    for (size_t i = 0, at = n - 768; i < 256; ++i, at += 3)
        palette[i] = 0xFF000000u | uint32_t(d[at]) << 16 | uint32_t(d[at + 1]) << 8 | d[at + 2];
    return true;
}

const Decoder& pcx_decoder()
{
    static const Decoder d{"PCX", {"pcx"}, sniff, decode};
    return d;
}

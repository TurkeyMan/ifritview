// PlayStation 2 TIM2 textures (.tm2). A 16-byte file header ("TIM2", a
// version, an alignment flag and the picture count), then each picture: a
// header, the image data (all its mipmaps, largest first) and the CLUT.
// Images are 4 or 8-bit indexed, or direct 16-bit (A1B5G5R5), 24-bit or
// 32-bit; CLUT entries are 16, 24 or 32-bit. A 256-colour CLUT stored as
// CSM1 has entries 8-15 and 16-23 of every 32 swapped (the GS reads it in
// that order); CSM2 is linear. PS2 alpha runs 0-128, 128 opaque. Each
// picture is a page, its mipmaps the levels.
#include "decoder.h"
#include "../i18n.h"
#include "texel.h"
#include "../bytes.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 16 && std::memcmp(h, "TIM2", 4) == 0 && h[5] <= 1;
}

// Colour types, shared by the image and the CLUT.
enum
{
    kRGB16 = 1,  // A1B5G5R5
    kRGB24 = 2,
    kRGB32 = 3,
    kIndex4 = 4,
    kIndex8 = 5
};

// PS2 alpha (0x80 opaque; some writers go past it) to 0-255.
uint32_t ps2_alpha(uint32_t a)
{
    return std::min(255u, (a * 255 + 64) / 128);
}

// A colour as straight 0xAARRGGBB, from a 16, 24 or 32-bit entry.
uint32_t colour(const uint8_t* p, int bytes)
{
    if (bytes == 2)
    {
        const uint32_t c = le16(p);
        const uint32_t r = (c & 0x1F) * 255 / 31, g = (c >> 5 & 0x1F) * 255 / 31, b = (c >> 10 & 0x1F) * 255 / 31;
        return (c & 0x8000 ? 0xFF000000u : 0) | r << 16 | g << 8 | b;
    }
    const uint32_t a = bytes == 4 ? ps2_alpha(p[3]) : 255;
    return a << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
}

const char* type_name(int t)
{
    switch (t)
    {
        case kRGB16: return "16-bit direct";
        case kRGB24: return "24-bit direct";
        case kRGB32: return "32-bit direct";
        case kIndex4: return "4-bit indexed";
        case kIndex8: return "8-bit indexed";
    }
    return "unknown";
}

bool decode_part(const std::string& path, ImagePart part, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || !sniff(d, size))
    {
        error = "not a TIM2 file";
        return false;
    }
    const int version = d[4], pictures = le16(d + 6);
    size_t pos = d[5] == 1 ? 128 : 16;  // pictures 128-byte aligned: the header is padded
    if (pictures == 0)
    {
        error = "TIM2 file holds no pictures";
        return false;
    }
    out.page_count = pictures;
    if (part.page >= pictures)
    {
        error = "the TIM2 has no such page or level";
        return false;
    }
    // Each picture header starts with the picture's whole size, padding
    // included, which steps to the next.
    for (int i = 0; i < part.page; ++i)
    {
        const uint32_t total = pos + 48 <= size ? le32(d + pos) : 0;
        if (total < 48 || total > size - pos)
        {
            error = "TIM2 picture is cut short";
            return false;
        }
        pos += total;
    }
    if (pos + 48 > size)
    {
        error = "TIM2 picture header is cut short";
        return false;
    }
    const uint8_t* ph = d + pos;
    const uint32_t clut_size = le32(ph + 4), image_size = le32(ph + 8);
    const int header_size = le16(ph + 12), clut_colours = le16(ph + 14);
    const int mipmaps = ph[17], clut_type = ph[18], image_type = ph[19];
    const int full_w = le16(ph + 20), full_h = le16(ph + 22);
    if (header_size < 48 || image_type < kRGB16 || image_type > kIndex8)
    {
        if (mipmaps == 0)
            error = "TIM2 picture holds only a CLUT";
        else
            error = "TIM2 image type " + std::to_string(image_type) + " isn't recognised";
        return false;
    }
    if (full_w == 0 || full_h == 0)
    {
        error = "bad TIM2 size";
        return false;
    }
    // The GS takes the base image and up to six more levels.
    out.level_count = std::clamp(mipmaps, 1, 7);
    if (part.level >= out.level_count)
    {
        error = "the TIM2 has no such page or level";
        return false;
    }
    const int w = std::max(1, full_w >> part.level), h = std::max(1, full_h >> part.level);
    // Header (with the mipmap table and any user data), image, CLUT.
    const size_t image_pos = pos + header_size, clut_pos = image_pos + size_t(image_size);
    if (image_pos > size || image_size > size - image_pos || clut_size > size - clut_pos)
    {
        error = "TIM2 data is cut short";
        return false;
    }
    static const int bits_for[] = {0, 16, 24, 32, 4, 8};
    const int bits = bits_for[image_type];
    const size_t row_bytes = (size_t(w) * bits + 7) / 8;
    // Where this level starts: past the larger ones, whose sizes (padding
    // included) a mipmap header after the picture header lists, after the
    // two GS MIPTBP registers. Without one, they're taken as packed.
    size_t level_pos = 0;
    for (int i = 0; i < part.level; ++i)
    {
        const size_t listed = header_size >= 64 + 4 * (i + 1) ? le32(ph + 64 + 4 * i) : 0;
        level_pos += listed ? listed : (size_t(std::max(1, full_w >> i)) * bits + 7) / 8 * size_t(std::max(1, full_h >> i));
    }
    if (level_pos > image_size || row_bytes * h > image_size - level_pos)
    {
        error = "TIM2 image data is smaller than its size";
        return false;
    }
    const uint8_t* img = d + image_pos + level_pos;
    const bool indexed = image_type == kIndex4 || image_type == kIndex8;

    // The CLUT, put in index order. Missing (it can live in a separate
    // file), the indices are shown as grey levels.
    std::vector<uint32_t> clut;
    const int clut_bytes = (clut_type & 0x3F) == kRGB16 ? 2 : (clut_type & 0x3F) == kRGB24 ? 3 : (clut_type & 0x3F) == kRGB32 ? 4 : 0;
    const bool csm2 = clut_type & 0x80;
    if (indexed && clut_bytes && clut_colours > 0 && size_t(clut_colours) * clut_bytes <= clut_size)
    {
        clut.resize(clut_colours);
        for (int i = 0; i < clut_colours; ++i)
        {
            int at = i;
            if (image_type == kIndex8 && !csm2)
                at = (i & ~0x18) | (i & 0x08) << 1 | (i & 0x10) >> 1;  // CSM1: swap entries 8-15 and 16-23 of every 32
            clut[i] = at < clut_colours ? colour(d + clut_pos + size_t(at) * clut_bytes, clut_bytes) : 0xFF000000u;
        }
    }
    const int levels = indexed ? 1 << bits : 0;
    if (indexed && clut.size() < size_t(levels))
    {
        const bool grey = clut.empty();
        for (int i = int(clut.size()); i < levels; ++i)
            clut.push_back(grey ? 0xFF000000u | uint32_t(i * 255 / (levels - 1)) * 0x010101 : 0xFF000000u);
    }

    out.allocate(w, h);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* s = img + size_t(y) * row_bytes;
        uint32_t* o = out.row(y);
        for (int x = 0; x < w; ++x)
            switch (image_type)
            {
                case kIndex4: o[x] = clut[s[x >> 1] >> (x & 1) * 4 & 0xF]; break;  // low nibble first
                case kIndex8: o[x] = clut[s[x]]; break;
                default: o[x] = colour(s + size_t(x) * (bits / 8), bits / 8); break;
            }
    }

    // A texture whose alpha is zero throughout is drawn opaque (the GS's
    // TEXA register, or the game, supplies the alpha): show the colours.
    bool all_clear = true, opaque = true;
    for (int y = 0; y < h && all_clear; ++y)
        for (const uint32_t* p = out.row(y); p != out.row(y) + w; ++p)
            if (*p >> 24)
            {
                all_clear = false;
                break;
            }
    for (int y = 0; y < h; ++y)
        for (uint32_t* p = out.row(y); p != out.row(y) + w; ++p)
        {
            if (all_clear)
                *p |= 0xFF000000u;
            else if (*p >> 24 != 255)
            {
                opaque = false;
                *p = premultiply(*p);
            }
        }
    const bool has_alpha_channel = image_type == kRGB16 || image_type == kRGB32 || (indexed && clut_bytes != 3 && clut_bytes != 0);
    out.has_alpha = !opaque;
    out.format = "TIM2";
    using M = PixelFormat::Model;
    if (indexed)
    {
        out.bits_per_sample = bits;
        out.pixel_format = PixelFormat::palette(bits, clut_colours);
    }
    else if (image_type == kRGB16)
    {
        out.bits_per_sample = 5;
        out.pixel_format = PixelFormat::widths(M::RGB, "ABGR", {1, 5, 5, 5}, 16);
    }
    else
    {
        out.bits_per_sample = 8;
        out.pixel_format = PixelFormat::packed(M::RGB, image_type == kRGB32 ? "RGBA" : "RGB", 8);
    }
    out.add(N_("TIM2"), N_("Version"), std::to_string(version));
    out.add(N_("TIM2"), N_("Image type"), type_name(image_type));
    if (indexed)
    {
        if (clut_bytes && clut_colours > 0 && size_t(clut_colours) * clut_bytes <= clut_size)
        {
            const size_t cluts = clut_size / (size_t(clut_colours) * clut_bytes);
            out.add(N_("TIM2"), N_("CLUT"),
                    std::to_string(clut_colours) + " × " + std::to_string(clut_bytes * 8) + "-bit, " + (csm2 ? "CSM2" : "CSM1") +
                        (cluts > 1 ? ", " + std::to_string(cluts) + " CLUTs, the first used" : ""));
        }
        else
            out.add(N_("TIM2"), N_("CLUT"), "none, shown in grey");
    }
    if (has_alpha_channel && all_clear)
        out.add(N_("TIM2"), N_("Alpha"), "zero throughout, shown opaque");
    if (pictures > 1)
        out.add(N_("TIM2"), N_("Pictures"), std::to_string(pictures));
    if (mipmaps > 1)
        out.add(N_("TIM2"), N_("Mipmaps"), std::to_string(mipmaps));
    return true;
}

}  // namespace

const Decoder& tim2_decoder()
{
    static const Decoder d{"TIM2", {"tm2", "tim2"}, sniff, [](const std::string& path, Image& out, std::string& error)
                           { return decode_part(path, {}, out, error); }, nullptr, false, decode_part};
    return d;
}

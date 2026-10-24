// .pvr is two unrelated formats:
//
// Dreamcast (Sega Katana) textures: an optional "GBIX" global index chunk,
// then a "PVRT" chunk naming a pixel format (ARGB1555, RGB565, ARGB4444,
// YUV422, BUMP height maps, RGB555) and a data layout: twiddled (Morton
// order, x bits in the odd positions), twiddled with mipmaps, VQ and small
// VQ (a codebook of 2 × 2 blocks and twiddled one-byte indices), 4/8-bit
// palette indices (the colours in a .pvp file beside it; grey when there's
// none), rectangle and stride (linear), twiddled rectangle, and 32-bit
// bitmaps. Mipmaps are stored smallest first: the largest, shown, is last.
//
// PowerVR SDK textures (iOS and Android): the 52-byte v2 header with "PVR!"
// at offset 44, and v3 with "PVR\x03" magic. Uncompressed formats (8-bit
// channels byte by byte, 16-bit ones packed first channel highest) and BC1-
// BC7 through bcn.cpp, ETC/EAC through etc.cpp and 2D ASTC through astc.cpp
// are shown; PVRTC isn't decoded.
// Surfaces, cube faces and depth slices are pages, mipmaps levels.
#include "astc.h"
#include "../i18n.h"
#include "../bytes.h"
#include "bcn.h"
#include "decoder.h"
#include "texel.h"
#include "etc.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{

bool is_pow2(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

// Scales an n-bit value to 8 bits.
uint32_t to8(uint32_t v, int bits)
{
    if (bits >= 8)
        return v >> (bits - 8);
    const uint32_t max = (1u << bits) - 1;
    return (v * 255 + max / 2) / max;
}

void set_has_alpha(Image& out)
{
    out.has_alpha = false;
    for (int y = 0; y < out.height && !out.has_alpha; ++y)
        for (const uint32_t* px = out.row(y); px != out.row(y) + out.width; ++px)
            if (*px >> 24 != 255)
            {
                out.has_alpha = true;
                break;
            }
}

void flip_rows(Image& out)
{
    for (int y = 0; y < out.height / 2; ++y)
        std::swap_ranges(out.row(y), out.row(y) + out.width, out.row(out.height - 1 - y));
}

void flip_columns(Image& out)
{
    for (int y = 0; y < out.height; ++y)
        std::reverse(out.row(y), out.row(y) + out.width);
}

// ---- Dreamcast ----

enum Pixel : int
{
    ARGB1555 = 0,
    RGB565 = 1,
    ARGB4444 = 2,
    YUV422 = 3,
    BUMP = 4,
    RGB555 = 5,
    YUV420 = 6,
};

const char* pixel_name(int p)
{
    switch (p)
    {
        case ARGB1555: return "ARGB1555";
        case RGB565: return "RGB565";
        case ARGB4444: return "ARGB4444";
        case YUV422: return "YUV422";
        case BUMP: return "BUMP";
        case RGB555: return "RGB555";
        case YUV420: return "YUV420";
    }
    return nullptr;
}

// Data layouts, with whether they hold mipmaps.
enum Layout : int
{
    Twiddled = 1,
    TwiddledMips = 2,
    VQ = 3,
    VQMips = 4,
    Pal4 = 5,
    Pal4Mips = 6,
    Pal8 = 7,
    Pal8Mips = 8,
    Rectangle = 9,
    Stride = 11,
    TwiddledRectangle = 13,
    Bitmap = 14,  // 32-bit
    BitmapMips = 15,
    SmallVQ = 16,
    SmallVQMips = 17,
    TwiddledMipsAlias = 18,  // the 1 × 1 level padded as the hardware addresses it
};

const char* layout_name(int l)
{
    switch (l)
    {
        case Twiddled: return "twiddled";
        case TwiddledMips: return "twiddled, mipmaps";
        case VQ: return "VQ";
        case VQMips: return "VQ, mipmaps";
        case Pal4: return "4-bit palette";
        case Pal4Mips: return "4-bit palette, mipmaps";
        case Pal8: return "8-bit palette";
        case Pal8Mips: return "8-bit palette, mipmaps";
        case Rectangle: return "rectangle";
        case Stride: return "stride";
        case TwiddledRectangle: return "twiddled rectangle";
        case Bitmap: return "32-bit bitmap";
        case BitmapMips: return "32-bit bitmap, mipmaps";
        case SmallVQ: return "small VQ";
        case SmallVQMips: return "small VQ, mipmaps";
        case TwiddledMipsAlias: return "twiddled, mipmaps (aliased)";
    }
    return nullptr;
}

bool has_mips(int l)
{
    return l == TwiddledMips || l == VQMips || l == Pal4Mips || l == Pal8Mips || l == BitmapMips || l == SmallVQMips ||
           l == TwiddledMipsAlias;
}
bool is_vq(int l)
{
    return l == VQ || l == VQMips || l == SmallVQ || l == SmallVQMips;
}
bool is_linear(int l)
{
    return l == Rectangle || l == Stride || l == Bitmap || l == BitmapMips;
}
bool is_palette(int l)
{
    return l >= Pal4 && l <= Pal8Mips;
}

// Position of (x, y) in a twiddled w × h surface (powers of two): Morton
// order within squares of the shorter side, the squares one after another.
size_t twiddle(int x, int y, int w, int h)
{
    const int m = std::min(w, h);
    const size_t square = size_t(w > h ? x / m : y / m) * m * m;
    x %= m;
    y %= m;
    size_t i = 0;
    for (int bit = 0; (1 << bit) < m; ++bit)
        i |= size_t(y >> bit & 1) << (2 * bit) | size_t(x >> bit & 1) << (2 * bit + 1);
    return square + i;
}

// Entries in a small VQ codebook, which depends on the size.
int small_vq_entries(int w, bool mips)
{
    if (w <= 16)
        return 16;
    if (w == 32)
        return mips ? 64 : 32;
    if (w == 64)
        return mips ? 256 : 128;
    return 256;
}

// Bytes of the smaller mipmaps stored before the largest of a w × w
// texture: each level at its size, the 1 × 1 one in a slot of its own.
size_t mip_skip(int layout, int w)
{
    size_t skip = 0;
    for (size_t s = 2; s < size_t(w); s *= 2)
        switch (layout)
        {
            case VQMips:
            case SmallVQMips: skip += (s / 2) * (s / 2); break;
            case Pal4Mips: skip += s * s / 2; break;
            case Pal8Mips: skip += s * s; break;
            case BitmapMips: skip += s * s * 4; break;
            default: skip += s * s * 2; break;
        }
    switch (layout)
    {
        case VQMips:
        case SmallVQMips: return skip + 1;
        case Pal4Mips: return skip + 2;
        case TwiddledMipsAlias: return skip + 8;
        default: return skip + 4;
    }
}

uint32_t texel_colour(int format, uint16_t c)
{
    switch (format)
    {
        case ARGB1555: return argb(to8(c >> 10 & 0x1F, 5), to8(c >> 5 & 0x1F, 5), to8(c & 0x1F, 5), c & 0x8000 ? 255 : 0);
        case RGB565: return argb(to8(c >> 11, 5), to8(c >> 5 & 0x3F, 6), to8(c & 0x1F, 5), 255);
        case ARGB4444: return argb(to8(c >> 8 & 0xF, 4), to8(c >> 4 & 0xF, 4), to8(c & 0xF, 4), to8(c >> 12, 4));
        case RGB555: return argb(to8(c >> 10 & 0x1F, 5), to8(c >> 5 & 0x1F, 5), to8(c & 0x1F, 5), 255);
        case BUMP:
        {
            // S (high byte): the angle up from the surface, 255 straight up;
            // R: the direction around it. Shown as a normal map.
            const double pi = 3.14159265358979323846;
            const double s = (1.0 - (c >> 8) / 255.0) * pi / 2, r = (c & 0xFF) / 255.0 * 2 * pi;
            const auto u8 = [](double v) { return uint32_t(std::clamp((v + 1) * 0.5, 0.0, 1.0) * 255 + 0.5); };
            return argb(u8(std::sin(s) * std::cos(r)), u8(std::sin(s) * std::sin(r)), u8(std::cos(s)), 255);
        }
    }
    return 0;
}

// Two horizontally adjacent YUV422 texels: Y0 with U, Y1 with V, converted
// as the PowerVR does (full range).
void yuv_pair(uint16_t t0, uint16_t t1, uint32_t& p0, uint32_t& p1)
{
    const int u = (t0 & 0xFF) - 128, v = (t1 & 0xFF) - 128;
    const auto rgb = [&](int y)
    {
        const auto c = [](int x) { return uint32_t(std::clamp(x, 0, 255)); };
        return argb(c(y + v * 11 / 8), c(y - (u * 11 + v * 22) / 32), c(y + u * 110 / 64), 255);
    };
    p0 = rgb(t0 >> 8);
    p1 = rgb(t1 >> 8);
}

// The .pvp palette beside a paletted texture: "PVPL", size, colour format
// (byte 8), entry count (16-bit at 14), entries from 16. Empty when none.
std::vector<uint32_t> read_pvp(const std::string& path, std::string& format)
{
    std::vector<uint32_t> palette;
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return palette;
    for (const char* ext : {".pvp", ".PVP", ".Pvp"})
    {
        FILE* f = platform::fopen(path.substr(0, dot) + ext, "rb");
        if (!f)
            continue;
        std::vector<uint8_t> d(16 + 256 * 4);
        d.resize(std::fread(d.data(), 1, d.size(), f));
        std::fclose(f);
        if (d.size() < 16 || std::memcmp(d.data(), "PVPL", 4) != 0)
            break;
        const int type = d[8];
        const bool wide = type == 6;  // ARGB8888
        const size_t entries = std::min<size_t>(le16(d.data() + 14), (d.size() - 16) / (wide ? 4 : 2));
        format = wide ? "ARGB8888" : pixel_name(type == 1 ? RGB565 : type == 2 ? ARGB4444 : ARGB1555);
        for (size_t i = 0; i < entries; ++i)
        {
            const uint8_t* e = d.data() + 16 + i * (wide ? 4 : 2);
            palette.push_back(wide ? argb(e[2], e[1], e[0], e[3])
                                   : texel_colour(type == 1 ? RGB565 : type == 2 ? ARGB4444 : ARGB1555, le16(e)));
        }
        break;
    }
    return palette;
}

bool dreamcast_decode(const std::string& path, const uint8_t* d, size_t size, ImagePart part, Image& out, std::string& error)
{
    size_t pos = 0;
    std::optional<uint32_t> global_index;
    if (size >= 8 && std::memcmp(d, "GBIX", 4) == 0)
    {
        const uint32_t n = le32(d + 4);
        if (n >= 4 && size >= 12)
            global_index = le32(d + 8);
        pos = 8 + size_t(n);
    }
    if (pos > size || size - pos < 16 || std::memcmp(d + pos, "PVRT", 4) != 0)
    {
        error = "PVR texture chunk is missing or cut short";
        return false;
    }
    const int pixel = d[pos + 8], layout = d[pos + 9];
    int w = le16(d + pos + 12), h = le16(d + pos + 14);  // the level's, once checked
    const uint8_t* data = d + pos + 16;
    const size_t avail = size - pos - 16;
    if (!layout_name(layout))
    {
        error = "PVR data format " + std::to_string(layout) + " isn't supported";
        return false;
    }
    const bool bitmap = layout == Bitmap || layout == BitmapMips;
    if (!bitmap && !is_palette(layout) && (!pixel_name(pixel) || pixel == YUV420))
    {
        error = "PVR pixel format " + (pixel_name(pixel) ? std::string(pixel_name(pixel)) : std::to_string(pixel)) +
                " isn't supported";
        return false;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
    {
        error = "bad PVR size";
        return false;
    }
    if (!is_linear(layout) && (!is_pow2(w) || !is_pow2(h) || (is_vq(layout) && (w < 2 || h < 2))))
    {
        error = "twiddled PVR texture isn't a power of two in size";
        return false;
    }
    if (has_mips(layout) && w != h)
    {
        error = "mipmapped PVR texture isn't square";
        return false;
    }
    if (pixel == YUV422 && w % 2)
    {
        error = "YUV422 PVR texture has an odd width";
        return false;
    }

    // Mipmaps, smallest first and the largest last, are levels, down to
    // 2 × 2: the 1 × 1 one sits in a slot whose size differs by layout.
    const int full_w = w;
    int levels = 1;
    while (has_mips(layout) && (full_w >> levels) >= 2)
        ++levels;
    out.level_count = levels;
    if (part.page > 0 || part.level >= levels)
    {
        error = "the PVR has no such page or level";
        return false;
    }
    if (part.level > 0)
        w = h = full_w >> part.level;  // mipmapped ones are square

    // Every layout first becomes w × h texels in raster order: 16-bit
    // colours, palette indices, or (bitmaps) 32-bit colours.
    const size_t n = size_t(w) * h;
    size_t offset = 0;
    std::vector<uint16_t> codebook;
    if (is_vq(layout))
    {
        const size_t entries = layout == SmallVQ || layout == SmallVQMips ? small_vq_entries(full_w, layout == SmallVQMips) : 256;
        if (avail < entries * 8)
        {
            error = "PVR codebook is cut short";
            return false;
        }
        codebook.resize(entries * 4);
        for (size_t i = 0; i < codebook.size(); ++i)
            codebook[i] = le16(data + 2 * i);
        offset = entries * 8;
    }
    if (has_mips(layout))
        offset += mip_skip(layout, w);
    const int bits = is_vq(layout) ? 2 : layout == Pal4 || layout == Pal4Mips ? 4 : layout == Pal8 || layout == Pal8Mips ? 8 : bitmap ? 32 : 16;
    const size_t need = (n * bits + 7) / 8;
    if (offset > avail || avail - offset < need)
    {
        error = "PVR data is cut short";
        return false;
    }
    const uint8_t* s = data + offset;
    std::vector<uint32_t> texels(n);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint32_t& t = texels[size_t(y) * w + x];
            if (is_vq(layout))
            {
                // Codebook blocks hold their four texels in twiddled order.
                const uint8_t index = s[twiddle(x / 2, y / 2, w / 2, h / 2)];
                const size_t entry = size_t(index) * 4 + (x & 1) * 2 + (y & 1);
                t = entry < codebook.size() ? codebook[entry] : 0;
            }
            else if (is_linear(layout))
            {
                const size_t i = size_t(y) * w + x;
                t = bitmap ? le32(s + 4 * i) : le16(s + 2 * i);
            }
            else
            {
                const size_t i = twiddle(x, y, w, h);
                t = bits == 4 ? s[i / 2] >> (i & 1 ? 4 : 0) & 0xF : bits == 8 ? s[i] : le16(s + 2 * i);
            }
        }

    out.allocate(w, h);
    std::string palette_format;
    std::vector<uint32_t> palette;
    if (is_palette(layout))
        palette = read_pvp(path, palette_format);
    for (int y = 0; y < h; ++y)
    {
        uint32_t* px = out.row(y);
        const uint32_t* t = texels.data() + size_t(y) * w;
        if (pixel == YUV422 && !bitmap && !is_palette(layout))
        {
            for (int x = 0; x < w; x += 2)
                yuv_pair(uint16_t(t[x]), uint16_t(t[x + 1]), px[x], px[x + 1]);
            continue;
        }
        for (int x = 0; x < w; ++x)
        {
            if (is_palette(layout))
            {
                if (!palette.empty())
                    px[x] = t[x] < palette.size() ? palette[t[x]] : 0;
                else
                {
                    const uint32_t v = bits == 4 ? t[x] * 17 : t[x];
                    px[x] = 0xFF000000u | v << 16 | v << 8 | v;
                }
            }
            else if (bitmap)  // bytes A, B, G, R
                px[x] = argb(t[x] >> 24, t[x] >> 16 & 0xFF, t[x] >> 8 & 0xFF, t[x] & 0xFF);
            else
                px[x] = texel_colour(pixel, uint16_t(t[x]));
        }
    }
    set_has_alpha(out);

    out.format = "PVR";
    out.bits_per_sample = 8;
    using M = PixelFormat::Model;
    if (is_palette(layout))
        out.pixel_format = PixelFormat::palette(bits, int(palette.size()));
    else if (bitmap)
        out.pixel_format = PixelFormat::packed(M::RGB, "ABGR", 8);
    else
        switch (pixel)
        {
            case ARGB1555: out.pixel_format = PixelFormat::widths(M::RGB, "ARGB", {1, 5, 5, 5}); break;
            case RGB565: out.pixel_format = PixelFormat::widths(M::RGB, "RGB", {5, 6, 5}); break;
            case ARGB4444: out.pixel_format = PixelFormat::packed(M::RGB, "ARGB", 4); break;
            case RGB555: out.pixel_format = PixelFormat::widths(M::RGB, "XRGB", {1, 5, 5, 5}); break;
            case YUV422:
                out.pixel_format = PixelFormat::ycbcr(8, 2, 1);
                out.pixel_format.planar = false;
                break;
            case BUMP: out.pixel_format.model = M::Vector; out.pixel_format.notes.push_back("bump map (S, R angles)"); break;
        }
    out.pixel_format.notes.push_back(layout_name(layout));
    if (is_vq(layout))
        out.pixel_format.bits_per_pixel = 2;
    out.add(N_("PVR"), N_("Format"), std::string(bitmap || is_palette(layout) ? "" : std::string(pixel_name(pixel)) + ", ") + layout_name(layout));
    if (global_index)
        out.add(N_("PVR"), N_("Global index"), std::to_string(*global_index));
    if (is_vq(layout))
        out.add(N_("PVR"), N_("Codebook"), std::to_string(codebook.size() / 4) + " entries");
    if (is_palette(layout))
        out.add(N_("PVR"), N_("Palette"), palette.empty() ? "no .pvp beside it: indices shown grey"
                                                  : ".pvp, " + std::to_string(palette.size()) + " " + palette_format + " entries");
    if (has_mips(layout))
    {
        int levels = 1;
        while ((1 << (levels - 1)) < full_w)
            ++levels;
        out.add(N_("PVR"), N_("Mipmaps"), std::to_string(levels));
    }
    return true;
}

// ---- PowerVR SDK ----

// Uncompressed pixels: up to four channels ('r', 'g', 'b', 'a', 'l' grey,
// 'i' intensity, 'x' padding) with their widths. All 8-bit: one byte each,
// in order. Otherwise packed into a little-endian 16-bit word, the first
// channel in the highest bits.
struct Channels
{
    char name[4];
    int bits[4];
    int count;
};

int channel_bits(const Channels& c)
{
    int total = 0;
    for (int i = 0; i < c.count; ++i)
        total += c.bits[i];
    return total;
}

bool channels_supported(const Channels& c)
{
    const int total = channel_bits(c);
    bool bytes = true;
    for (int i = 0; i < c.count; ++i)
    {
        if (!std::strchr("rgbalix", c.name[i]) || c.bits[i] <= 0)
            return false;
        bytes = bytes && c.bits[i] == 8;
    }
    return c.count > 0 && (bytes || total == 16);
}

bool decode_channels(const Channels& c, const uint8_t* d, size_t size, int w, int h, bool premultiplied, Image& out,
                     std::string& error)
{
    const int bytes = channel_bits(c) / 8;
    if (size / bytes / w < size_t(h))
    {
        error = "PVR data is cut short";
        return false;
    }
    bool bytewise = true;
    for (int i = 0; i < c.count; ++i)
        bytewise = bytewise && c.bits[i] == 8;
    bool has_colour = false;
    for (int i = 0; i < c.count; ++i)
        has_colour = has_colour || std::strchr("rgbli", c.name[i]);
    out.allocate(w, h);
    for (int y = 0; y < h; ++y)
    {
        uint32_t* px = out.row(y);
        const uint8_t* s = d + size_t(y) * w * bytes;
        for (int x = 0; x < w; ++x, s += bytes)
        {
            uint32_t v[4];
            if (bytewise)
                for (int i = 0; i < c.count; ++i)
                    v[i] = s[i];
            else
            {
                const uint32_t word = le16(s);
                int shift = 16;
                for (int i = 0; i < c.count; ++i)
                {
                    shift -= c.bits[i];
                    v[i] = to8(word >> shift & ((1u << c.bits[i]) - 1), c.bits[i]);
                }
            }
            uint32_t r = 0, g = 0, b = 0, a = 255;
            for (int i = 0; i < c.count; ++i)
                switch (c.name[i])
                {
                    case 'r': r = v[i]; break;
                    case 'g': g = v[i]; break;
                    case 'b': b = v[i]; break;
                    case 'a': a = v[i]; break;
                    case 'l':
                    case 'i': r = g = b = v[i]; break;
                }
            if (!has_colour)  // alpha alone: shown grey
                r = g = b = a, a = 255;
            px[x] = argb(r, g, b, a, premultiplied);
        }
    }
    set_has_alpha(out);
    return true;
}

// v2 pixel types (flags & 0xFF) shown here.
std::optional<Channels> v2_channels(int type)
{
    switch (type)
    {
        case 0x00: return Channels{{'a', 'r', 'g', 'b'}, {4, 4, 4, 4}, 4};  // MGL ARGB4444
        case 0x01: return Channels{{'a', 'r', 'g', 'b'}, {1, 5, 5, 5}, 4};  // MGL ARGB1555
        case 0x02:
        case 0x13: return Channels{{'r', 'g', 'b'}, {5, 6, 5}, 3};
        case 0x10: return Channels{{'r', 'g', 'b', 'a'}, {4, 4, 4, 4}, 4};
        case 0x11: return Channels{{'r', 'g', 'b', 'a'}, {5, 5, 5, 1}, 4};
        case 0x12: return Channels{{'r', 'g', 'b', 'a'}, {8, 8, 8, 8}, 4};
        case 0x15: return Channels{{'r', 'g', 'b'}, {8, 8, 8}, 3};
        case 0x16: return Channels{{'l'}, {8}, 1};
        case 0x17: return Channels{{'l', 'a'}, {8, 8}, 2};
        case 0x1A: return Channels{{'b', 'g', 'r', 'a'}, {8, 8, 8, 8}, 4};
        case 0x1B: return Channels{{'a'}, {8}, 1};
    }
    return std::nullopt;
}

// A block-compressed SDK format: BCn, ETC/EAC or ASTC (one of them set).
struct Blocks
{
    std::optional<Bcn> bcn;
    std::optional<Etc> etc;
    int astc_w = 0, astc_h = 0;
    AstcProfile profile = AstcProfile::Auto;
    bool any() const { return bcn || etc || astc_w; }
    std::string name() const
    {
        if (bcn)
            return bcn_name(*bcn);
        if (etc)
            return etc_name(*etc);
        return "ASTC " + std::to_string(astc_w) + "x" + std::to_string(astc_h);
    }
};

std::optional<Bcn> v2_bcn(int type, bool& premultiplied)
{
    switch (type)
    {
        case 0x20: return Bcn::BC1;
        case 0x21: premultiplied = true; return Bcn::BC2;
        case 0x22: return Bcn::BC2;
        case 0x23: premultiplied = true; return Bcn::BC3;
        case 0x24: return Bcn::BC3;
    }
    return std::nullopt;
}

const char* v2_compressed_name(int type)
{
    switch (type)
    {
        case 0x0C:
        case 0x18: return "PVRTC 2bpp";
        case 0x0D:
        case 0x19: return "PVRTC 4bpp";
        case 0x1C: return "PVRTC-II 4bpp";
        case 0x1D: return "PVRTC-II 2bpp";
        case 0x36: return "ETC1";
    }
    return nullptr;
}

// v3 compressed formats (the high 32 bits of the pixel format zero).
const char* v3_compressed_name(uint32_t f)
{
    static const char* const names[] = {
        "PVRTC 2bpp RGB", "PVRTC 2bpp RGBA", "PVRTC 4bpp RGB", "PVRTC 4bpp RGBA", "PVRTC-II 2bpp", "PVRTC-II 4bpp",
        "ETC1", "BC1 (DXT1)", "DXT2", "BC2 (DXT3)", "DXT4", "BC3 (DXT5)", "BC4", "BC5", "BC6H", "BC7", "UYVY", "YUY2",
        "1-bit black and white", "RGB9E5", "RGBG8888", "GRGB8888", "ETC2 RGB", "ETC2 RGBA", "ETC2 RGB A1", "EAC R11",
        "EAC RG11", "ASTC 4x4", "ASTC 5x4", "ASTC 5x5", "ASTC 6x5", "ASTC 6x6", "ASTC 8x5", "ASTC 8x6", "ASTC 8x8",
        "ASTC 10x5", "ASTC 10x6", "ASTC 10x8", "ASTC 10x10", "ASTC 12x10", "ASTC 12x12", "ASTC 3x3x3", "ASTC 4x3x3",
        "ASTC 4x4x3", "ASTC 4x4x4", "ASTC 5x4x4", "ASTC 5x5x4", "ASTC 5x5x5", "ASTC 6x5x5", "ASTC 6x6x5", "ASTC 6x6x6",
        "Basis ETC1S", "Basis UASTC", "RGBM", "RGBD", "PVRTC HDR 6bpp", "PVRTC HDR 8bpp", "PVRTC-II HDR 6bpp",
        "PVRTC-II HDR 8bpp"};
    return f < sizeof(names) / sizeof(names[0]) ? names[f] : nullptr;
}

// v3's ETC/EAC and 2D ASTC formats, by number.
Blocks v3_mobile(uint32_t f, uint32_t channel_type, uint32_t colour_space)
{
    const bool is_signed = channel_type == 1 || channel_type == 3 || channel_type == 5 || channel_type == 12;
    Blocks b;
    switch (f)
    {
        case 6: b.etc = Etc::ETC1; break;
        case 22: b.etc = Etc::ETC2_RGB; break;
        case 23: b.etc = Etc::ETC2_RGBA; break;
        case 24: b.etc = Etc::ETC2_RGBA1; break;
        case 25: b.etc = is_signed ? Etc::EAC_R11S : Etc::EAC_R11; break;
        case 26: b.etc = is_signed ? Etc::EAC_RG11S : Etc::EAC_RG11; break;
    }
    static const int footprints[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    if (f >= 27 && f < 27 + std::size(footprints))
    {
        b.astc_w = footprints[f - 27][0];
        b.astc_h = footprints[f - 27][1];
        // Float channels mean HDR; sRGB colour space, sRGB endpoints.
        b.profile = channel_type >= 12 ? AstcProfile::HDR : colour_space == 1 ? AstcProfile::sRGB : AstcProfile::LDR;
    }
    return b;
}

std::optional<Bcn> v3_bcn(uint32_t f, uint32_t channel_type, bool& premultiplied)
{
    const bool is_signed = channel_type == 1 || channel_type == 12;  // signed byte normalised, signed float
    switch (f)
    {
        case 7: return Bcn::BC1;
        case 8: premultiplied = true; return Bcn::BC2;
        case 9: return Bcn::BC2;
        case 10: premultiplied = true; return Bcn::BC3;
        case 11: return Bcn::BC3;
        case 12: return is_signed ? Bcn::BC4S : Bcn::BC4;
        case 13: return is_signed ? Bcn::BC5S : Bcn::BC5;
        case 14: return is_signed ? Bcn::BC6HS : Bcn::BC6H;
        case 15: return Bcn::BC7;
    }
    return std::nullopt;
}

std::string channels_name(const Channels& c)
{
    std::string s;  // "R5G6B5"
    for (int i = 0; i < c.count; ++i)
    {
        const uint8_t n = uint8_t(c.name[i]);
        s += (n >= 32 && n < 127 ? char(std::toupper(n)) : '?') + std::to_string(c.bits[i]);
    }
    return s.empty() ? "?" : s;
}

void describe_sdk(Image& out, const Blocks& blocks, const std::optional<Channels>& channels)
{
    out.format = "PVR";
    out.bits_per_sample = 8;
    using M = PixelFormat::Model;
    if (blocks.etc)
    {
        out.pixel_format = etc_pixel_format(*blocks.etc);
        return;
    }
    if (blocks.astc_w)
    {
        const bool hdr = out.bits_per_sample == 16;
        out.pixel_format = astc_pixel_format(blocks.astc_w, blocks.astc_h, hdr);
        return;
    }
    if (const std::optional<Bcn>& bcn = blocks.bcn)
    {
        out.pixel_format.model = *bcn == Bcn::BC4 || *bcn == Bcn::BC4S ? M::L : M::RGB;
        out.pixel_format.notes.push_back(std::string(bcn_name(*bcn)) + " block compression");
        if (*bcn == Bcn::BC6H || *bcn == Bcn::BC6HS)
            out.bits_per_sample = 16;
        return;
    }
    std::vector<int> bits(channels->bits, channels->bits + channels->count);
    std::string names;
    bool grey = true;
    for (int i = 0; i < channels->count; ++i)
    {
        const char n = channels->name[i];
        names += n == 'l' || n == 'i' ? 'L' : char(n - 'a' + 'A');
        grey = grey && !std::strchr("rgb", n);
    }
    out.pixel_format = PixelFormat::widths(grey ? M::L : M::RGB, names.c_str(), bits);
}

// Bytes of one w × h surface.
size_t sdk_bytes(const Blocks& blocks, const std::optional<Channels>& channels, int w, int h)
{
    if (blocks.bcn)
        return bcn_size(*blocks.bcn, w, h);
    if (blocks.etc)
        return etc_size(*blocks.etc, w, h);
    if (blocks.astc_w)
        return astc_size(blocks.astc_w, blocks.astc_h, w, h);
    return (size_t(w) * size_t(h) * size_t(channel_bits(*channels)) + 7) / 8;
}

// Shared by v2 and v3 once the format is known.
bool decode_sdk_surface(const Blocks& blocks, const std::optional<Channels>& channels, bool premultiplied, const uint8_t* d, size_t size, int w,
                        int h, Image& out, std::string& error)
{
    if (blocks.bcn)
    {
        if (!decode_bcn(*blocks.bcn, d, size, w, h, out, error, premultiplied))
            return false;
    }
    else if (blocks.etc)
    {
        if (!decode_etc(*blocks.etc, d, size, w, h, out, error, premultiplied))
            return false;
    }
    else if (blocks.astc_w)
    {
        if (!decode_astc(d, size, blocks.astc_w, blocks.astc_h, w, h, blocks.profile, out, error, premultiplied))
            return false;
    }
    else if (!decode_channels(*channels, d, size, w, h, premultiplied, out, error))
        return false;
    const int bits = out.bits_per_sample;  // ASTC HDR set 16
    describe_sdk(out, blocks, channels);
    if (blocks.astc_w && bits == 16)
    {
        out.bits_per_sample = 16;
        out.pixel_format = astc_pixel_format(blocks.astc_w, blocks.astc_h, true);
    }
    return true;
}

bool v2_decode(const uint8_t* d, size_t size, ImagePart part, Image& out, std::string& error)
{
    const int h = int(le32(d + 4)), w = int(le32(d + 8));
    const uint32_t mips = le32(d + 12), flags = le32(d + 16), surfaces = le32(d + 48);
    const int type = flags & 0xFF;
    if (!Image::fits(w, h))
    {
        error = "bad PVR size";
        return false;
    }
    bool premultiplied = false;
    Blocks blocks;
    blocks.bcn = v2_bcn(type, premultiplied);
    if (type == 0x36)
        blocks.etc = Etc::ETC1;
    const std::optional<Channels> channels = v2_channels(type);
    if (!blocks.any() && !channels)
    {
        const char* name = v2_compressed_name(type);
        error = name ? std::string("PVR format ") + name + " isn't supported" : "PVR pixel type " + std::to_string(type) + " isn't supported";
        return false;
    }
    if (flags & 0x200 && !blocks.any())
    {
        error = "twiddled PowerVR SDK texture isn't supported";
        return false;
    }
    // Each surface (or cube face) with its mipmaps after it, largest first.
    const int levels = flags & 0x100 ? int(std::min(mips, 31u)) + 1 : 1;
    out.page_count = int(std::clamp(surfaces, 1u, 1u << 16));
    out.level_count = levels;
    if (part.page >= out.page_count || part.level >= levels)
    {
        error = "the PVR has no such page or level";
        return false;
    }
    if (flags & 0x1000)
        out.page_label = cube_face_label(part.page, out.page_count / 6);
    size_t at = 52;
    for (int m = 0; m < levels; ++m)
        at += sdk_bytes(blocks, channels, std::max(1, w >> m), std::max(1, h >> m)) * (m < part.level ? 1 + size_t(part.page) : size_t(part.page));
    const int lw = std::max(1, w >> part.level), lh = std::max(1, h >> part.level);
    if (at > size || !decode_sdk_surface(blocks, channels, premultiplied, d + at, size - at, lw, lh, out, error))
    {
        if (error.empty())
            error = "PVR data is cut short";
        return false;
    }
    if (flags & 0x10000)  // stored bottom up
        flip_rows(out);
    out.add(N_("PVR"), N_("Version"), "PowerVR SDK v2");
    out.add(N_("PVR"), N_("Format"), blocks.any() ? blocks.name() : channels_name(*channels));
    if (flags & 0x100 && mips > 0)
        out.add(N_("PVR"), N_("Mipmaps"), std::to_string(mips + 1));
    if (flags & 0x1000)
        out.add(N_("PVR"), N_("Cube map"), _("6 faces"));
    if (surfaces > 1 && !(flags & 0x1000))
        out.add(N_("PVR"), N_("Surfaces"), std::to_string(surfaces));
    return true;
}

bool v3_decode(const uint8_t* d, size_t size, ImagePart part, Image& out, std::string& error)
{
    if (size < 52)
    {
        error = "PVR header is cut short";
        return false;
    }
    const uint32_t flags = le32(d + 4), lo = le32(d + 8), hi = le32(d + 12);
    const uint32_t colour_space = le32(d + 16), channel_type = le32(d + 20);
    const int h = int(le32(d + 24)), w = int(le32(d + 28));
    const uint32_t depth = le32(d + 32), surfaces = le32(d + 36), faces = le32(d + 40), mips = le32(d + 44);
    const uint32_t meta = le32(d + 48);
    if (!Image::fits(w, h))
    {
        error = "bad PVR size";
        return false;
    }
    if (meta > size - 52)
    {
        error = "PVR metadata is cut short";
        return false;
    }
    bool premultiplied = (flags & 2) != 0;
    Blocks blocks;
    std::optional<Channels> channels;
    if (hi == 0)
    {
        blocks = v3_mobile(lo, channel_type, colour_space);
        blocks.bcn = v3_bcn(lo, channel_type, premultiplied);
        if (!blocks.any())
        {
            const char* name = v3_compressed_name(lo);
            error = name ? std::string("PVR format ") + name + " isn't supported" : "PVR format " + std::to_string(lo) + " isn't supported";
            return false;
        }
    }
    else
    {
        Channels c{};
        for (int i = 0; i < 4 && (lo >> (8 * i) & 0xFF); ++i)
        {
            c.name[i] = char(lo >> (8 * i));
            c.bits[i] = int(hi >> (8 * i) & 0xFF);
            c.count = i + 1;
        }
        // Unsigned normalised byte, short or int; not raw integers or floats.
        if (!channels_supported(c) || (channel_type != 0 && channel_type != 4 && channel_type != 8))
        {
            const char* kind = channel_type >= 12 ? " float" : channel_type % 2 ? " signed" : channel_type % 4 ? " integer" : "";
            error = "PVR format " + channels_name(c) + kind + " isn't supported";
            return false;
        }
        channels = c;
    }
    // Metadata entries: FourCC, key, size, data. Orientation ("PVR\x03", key
    // 3): x 1 = right to left, y 1 = bottom up.
    bool flip_x = false, flip_y = false;
    for (size_t p = 52; p + 12 <= 52 + size_t(meta);)
    {
        const uint32_t key = le32(d + p + 4), n = le32(d + p + 8);
        if (n > 52 + size_t(meta) - p - 12)
            break;
        if (std::memcmp(d + p, "PVR\x03", 4) == 0 && key == 3 && n >= 2)
        {
            flip_x = d[p + 12] != 0;
            flip_y = d[p + 13] != 0;
        }
        p += 12 + n;
    }
    // Level-major: each level holds every surface's faces' slices (a
    // volume's halving with each level; the page shows the one at its depth).
    const uint32_t ns = std::max(1u, surfaces), nf = std::max(1u, faces), nd = std::max(1u, depth);
    const int levels = int(std::clamp(mips, 1u, 32u));
    out.page_count = int(std::min<uint64_t>(uint64_t(ns) * nf * nd, 1u << 16));
    out.level_count = levels;
    if (part.page >= out.page_count || part.level >= levels)
    {
        error = "the PVR has no such page or level";
        return false;
    }
    if (nf == 6)
        out.page_label = cube_face_label(part.page / int(nd), int(ns));
    const auto slices = [&](int m) { return std::max(1u, nd >> m); };
    size_t offset = 52 + size_t(meta);
    for (int m = 0; m < part.level; ++m)
        offset += sdk_bytes(blocks, channels, std::max(1, w >> m), std::max(1, h >> m)) * ns * nf * slices(m);
    const int lw = std::max(1, w >> part.level), lh = std::max(1, h >> part.level);
    const uint32_t page = uint32_t(part.page), surface_face = page / nd, z = page % nd * slices(part.level) / nd;
    offset += sdk_bytes(blocks, channels, lw, lh) * (size_t(surface_face) * slices(part.level) + z);
    if (offset > size || !decode_sdk_surface(blocks, channels, premultiplied, d + offset, size - offset, lw, lh, out, error))
    {
        if (error.empty())
            error = "PVR data is cut short";
        return false;
    }
    if (flip_y)
        flip_rows(out);
    if (flip_x)
        flip_columns(out);
    out.add(N_("PVR"), N_("Version"), "PowerVR SDK v3");
    out.add(N_("PVR"), N_("Format"), blocks.any() ? blocks.name() : channels_name(*channels));
    if (colour_space == 1)
        out.add(N_("PVR"), N_("Colour space"), "sRGB");
    if (premultiplied)
        out.add(N_("PVR"), N_("Alpha"), "premultiplied");
    if (mips > 1)
        out.add(N_("PVR"), N_("Mipmaps"), std::to_string(mips));
    if (faces > 1)
        out.add(N_("PVR"), N_("Faces"), std::to_string(faces));
    if (surfaces > 1)
        out.add(N_("PVR"), N_("Surfaces"), std::to_string(surfaces));
    if (depth > 1)
        out.add(N_("PVR"), N_("Depth"), rap::i18n::format(_("{0} slices"), depth));
    return true;
}

bool is_v2(const uint8_t* h, size_t n)
{
    return n >= 52 && le32(h) == 52 && std::memcmp(h + 44, "PVR!", 4) == 0;
}

bool is_dreamcast(const uint8_t* h, size_t n)
{
    if (n >= 4 && std::memcmp(h, "PVRT", 4) == 0)
        return true;
    if (n < 8 || std::memcmp(h, "GBIX", 4) != 0)
        return false;
    const size_t at = 8 + size_t(le32(h + 4));
    return at <= 32 && n >= at + 4 && std::memcmp(h + at, "PVRT", 4) == 0;
}

bool sniff(const uint8_t* h, size_t n)
{
    return (n >= 4 && std::memcmp(h, "PVR\x03", 4) == 0) || is_v2(h, n) || is_dreamcast(h, n);
}

bool decode_part(const std::string& path, ImagePart part, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size))
    {
        error = "not a PVR file";
        return false;
    }
    if (size >= 4 && std::memcmp(d, "PVR\x03", 4) == 0)
        return v3_decode(d, size, part, out, error);
    if (is_v2(d, size))
        return v2_decode(d, size, part, out, error);
    if (size >= 8 && (std::memcmp(d, "PVRT", 4) == 0 || std::memcmp(d, "GBIX", 4) == 0))
        return dreamcast_decode(path, d, size, part, out, error);
    error = "not a PVR file";
    return false;
}

}  // namespace

const Decoder& pvr_decoder()
{
    static const Decoder d{"PVR", {"pvr"}, sniff, [](const std::string& path, Image& out, std::string& error)
                           { return decode_part(path, {}, out, error); }, nullptr, false, decode_part};
    return d;
}

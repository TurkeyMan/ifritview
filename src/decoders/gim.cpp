// PSP GIM textures (.gim), as GimConv writes them. A 16-byte signature
// ("MIG.00.1PSP"), then a tree of blocks: the root holds pictures, a
// picture holds an image and, for indexed images, a palette, each with a
// header describing its levels (mipmaps) and frames. Pixels are RGBA5650,
// 5551, 4444 or 8888 (red in the low bits), 4, 8, 16 or 32-bit indices
// into a palette in one of those formats, or the PSP's DXT1/3/5 (blocks
// laid out differently from DirectX's). The "faster" pixel order is the
// PSP's swizzle: 16-byte by 8-row tiles, left to right, top to bottom.
// Big-endian files (PS3) are read the same way with their words swapped.
// The first picture's first frame and level is shown.
#include "bcn.h"
#include "decoder.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    // Little-endian files spell the signature out; big-endian ones store it
    // as the same 32-bit words, so each four bytes come reversed.
    return n >= 16 && (std::memcmp(h, "MIG.00.1", 8) == 0 || std::memcmp(h, ".GIM1.00", 8) == 0);
}

enum Block
{
    kRoot = 2,
    kPicture = 3,
    kImage = 4,
    kPalette = 5,
    kFileInfo = 0xFF
};

enum Format
{
    kRGBA5650 = 0,
    kRGBA5551 = 1,
    kRGBA4444 = 2,
    kRGBA8888 = 3,
    kIndex4 = 4,
    kIndex8 = 5,
    kIndex16 = 6,
    kIndex32 = 7,
    kDXT1 = 8,
    kDXT3 = 9,
    kDXT5 = 10
};

const char* format_name(int f)
{
    static const char* names[] = {"RGBA5650", "RGBA5551", "RGBA4444", "RGBA8888", "INDEX4", "INDEX8",
                                  "INDEX16",  "INDEX32",  "DXT1",     "DXT3",     "DXT5"};
    return f >= 0 && f <= kDXT5 ? names[f] : "unknown";
}

int format_bits(int f)
{
    static const int bits[] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};
    return f >= 0 && f <= kDXT5 ? bits[f] : 0;
}

uint32_t get16(const uint8_t* p, bool big)
{
    return big ? uint32_t(p[0]) << 8 | p[1] : uint32_t(p[0]) | uint32_t(p[1]) << 8;
}
uint32_t get32(const uint8_t* p, bool big)
{
    return big ? get16(p, true) << 16 | get16(p + 2, true) : get16(p, false) | get16(p + 2, false) << 16;
}

struct File
{
    const uint8_t* d;
    size_t size;
    bool big;  // PS3

    uint32_t u16(size_t at) const { return get16(d + at, big); }
    uint32_t u32(size_t at) const { return get32(d + at, big); }
};

// A block found in the tree: where it starts and ends, and its contents.
struct Found
{
    size_t pos = 0, end = 0, data = 0;
    bool ok = false;
};

// Reads the block header at `pos` (within `limit`). Containers' children
// start at the data offset; every block is `size` bytes, children included.
bool block_at(const File& f, size_t pos, size_t limit, int& type, Found& b)
{
    if (pos + 16 > limit)
        return false;
    type = int(f.u16(pos));
    const uint32_t size = f.u32(pos + 4), data = f.u32(pos + 12);
    if (size < 16 || size > limit - pos || data < 16 || data > size)
        return false;
    b.pos = pos;
    b.end = pos + size;
    b.data = pos + data;
    b.ok = true;
    return true;
}

// The first picture's image and palette, and the file info if any.
struct Tree
{
    Found image, palette, info;
    int pictures = 0;
};

void walk(const File& f, size_t pos, size_t limit, int depth, Tree& t)
{
    int type;
    Found b;
    while (depth < 8 && block_at(f, pos, limit, type, b))
    {
        if (type == kRoot)
            walk(f, b.data, b.end, depth + 1, t);
        else if (type == kPicture)
        {
            if (t.pictures++ == 0)
                walk(f, b.data, b.end, depth + 1, t);
        }
        else if (type == kImage && !t.image.ok)
            t.image = b;
        else if (type == kPalette && !t.palette.ok)
            t.palette = b;
        else if (type == kFileInfo && !t.info.ok)
            t.info = b;
        pos = b.end;
    }
}

// An image or palette block's header, and where its first level is.
struct Level
{
    int format = 0, order = 0, w = 0, h = 0, pitch_align = 0, levels = 0, frames = 0;
    const uint8_t* pixels = nullptr;
    size_t bytes = 0;  // available from `pixels` to the end of the block's data
};

bool read_level(const File& f, const Found& b, Level& l, std::string& error)
{
    const size_t hp = b.data;
    if (hp + 0x30 > b.end || f.u16(hp) < 0x30)
    {
        error = "GIM image header is cut short";
        return false;
    }
    l.format = int(f.u16(hp + 4));
    l.order = int(f.u16(hp + 6));
    l.w = int(f.u16(hp + 8));
    l.h = int(f.u16(hp + 10));
    l.pitch_align = int(f.u16(hp + 14));
    const uint32_t index = f.u32(hp + 0x18), end = f.u32(hp + 0x20);
    l.levels = int(f.u16(hp + 0x2A));
    l.frames = int(f.u16(hp + 0x2E));
    if (index > b.end - hp || b.end - hp - index < 4)
    {
        error = "GIM level table is out of bounds";
        return false;
    }
    // Offsets of the first frame's levels, from the header.
    const uint32_t first = f.u32(hp + index);
    const size_t limit = std::min(b.end, hp + std::max<size_t>(end, first));
    if (first < 0x30 || first > limit - hp)
    {
        error = "GIM pixel data is out of bounds";
        return false;
    }
    l.pixels = f.d + hp + first;
    l.bytes = limit - (hp + first);
    if (l.w <= 0 || l.h <= 0)
    {
        error = "bad GIM size";
        return false;
    }
    return true;
}

uint32_t straight(int format, uint32_t v)
{
    uint32_t r, g, b, a = 255;
    switch (format)
    {
        case kRGBA5650:
            r = (v & 0x1F) * 255 / 31, g = (v >> 5 & 0x3F) * 255 / 63, b = (v >> 11 & 0x1F) * 255 / 31;
            break;
        case kRGBA5551:
            r = (v & 0x1F) * 255 / 31, g = (v >> 5 & 0x1F) * 255 / 31, b = (v >> 10 & 0x1F) * 255 / 31, a = v & 0x8000 ? 255 : 0;
            break;
        case kRGBA4444:
            r = (v & 0xF) * 17, g = (v >> 4 & 0xF) * 17, b = (v >> 8 & 0xF) * 17, a = (v >> 12 & 0xF) * 17;
            break;
        default:
            r = v & 0xFF, g = v >> 8 & 0xFF, b = v >> 16 & 0xFF, a = v >> 24;
            break;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

uint32_t premultiply(uint32_t c)
{
    const uint32_t a = c >> 24;
    if (a == 255)
        return c;
    const uint32_t r = ((c >> 16 & 0xFF) * a + 127) / 255, g = ((c >> 8 & 0xFF) * a + 127) / 255, b = ((c & 0xFF) * a + 127) / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

// The level's samples (colours or indices) in raster order, unswizzled
// and with the row padding dropped.
bool samples(const Level& l, bool big, std::vector<uint32_t>& out, std::string& error)
{
    const int bits = format_bits(l.format);
    const size_t row = (size_t(l.w) * bits + 7) / 8;
    const size_t align = l.order == 1 ? 16 : std::clamp(l.pitch_align, 1, 1024);
    const size_t pitch = (row + align - 1) / align * align;
    const size_t rows = l.order == 1 ? (size_t(l.h) + 7) / 8 * 8 : size_t(l.h);
    // The last row needn't carry its padding when the data is linear.
    const size_t need = l.order == 1 ? pitch * rows : pitch * (rows - 1) + row;
    if (l.bytes < need)
    {
        error = "GIM pixel data is cut short";
        return false;
    }
    std::vector<uint8_t> linear;
    const uint8_t* src = l.pixels;
    if (l.order == 1)
    {
        linear.resize(pitch * rows);
        const size_t tiles_across = pitch / 16;
        for (size_t y = 0; y < rows; ++y)
            for (size_t tx = 0; tx < tiles_across; ++tx)
                std::memcpy(&linear[y * pitch + tx * 16], src + ((y / 8) * tiles_across + tx) * 128 + (y % 8) * 16, 16);
        src = linear.data();
    }
    out.resize(size_t(l.w) * l.h);
    for (int y = 0; y < l.h; ++y)
    {
        const uint8_t* s = src + size_t(y) * pitch;
        uint32_t* o = out.data() + size_t(y) * l.w;
        for (int x = 0; x < l.w; ++x)
            switch (bits)
            {
                case 4: o[x] = s[x >> 1] >> (x & 1) * 4 & 0xF; break;  // low nibble first
                case 8: o[x] = s[x]; break;
                case 16: o[x] = get16(s + size_t(x) * 2, big); break;
                default: o[x] = get32(s + size_t(x) * 4, big); break;
            }
    }
    return true;
}

// PSP DXT blocks put the colour indices before the two colours, DXT3/5's
// alpha after the colour block, and DXT5's two alpha values after their
// indices: reorder each block as DirectX has it.
std::vector<uint8_t> dxt_to_directx(int format, const uint8_t* s, size_t blocks)
{
    const size_t bytes = format == kDXT1 ? 8 : 16;
    std::vector<uint8_t> out(blocks * bytes);
    for (size_t i = 0; i < blocks; ++i, s += bytes)
    {
        uint8_t* o = &out[i * bytes];
        uint8_t* colour = format == kDXT1 ? o : o + 8;
        std::memcpy(colour, s + 4, 4);
        std::memcpy(colour + 4, s, 4);
        if (format == kDXT3)
            std::memcpy(o, s + 8, 8);
        else if (format == kDXT5)
        {
            o[0] = s[14];
            o[1] = s[15];
            std::memcpy(o + 2, s + 8, 6);
        }
    }
    return out;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || !sniff(d, size))
    {
        error = "not a GIM file";
        return false;
    }
    File f{d, size, d[0] == '.'};
    Tree t;
    walk(f, 16, size, 0, t);
    if (!t.image.ok)
    {
        error = "GIM file holds no image";
        return false;
    }
    Level img;
    if (!read_level(f, t.image, img, error))
        return false;
    const int fmt = img.format;
    if (fmt < 0 || fmt > kDXT5)
    {
        error = "GIM format " + std::to_string(fmt) + " isn't supported";
        return false;
    }
    const bool indexed = fmt >= kIndex4 && fmt <= kIndex32;

    if (fmt >= kDXT1)
    {
        const Bcn bcn = fmt == kDXT1 ? Bcn::BC1 : fmt == kDXT3 ? Bcn::BC2 : Bcn::BC3;
        const size_t need = bcn_size(bcn, img.w, img.h);
        if (img.bytes < need)
        {
            error = "GIM pixel data is cut short";
            return false;
        }
        const std::vector<uint8_t> blocks = dxt_to_directx(fmt, img.pixels, need / bcn_block_bytes(bcn));
        if (!decode_bcn(bcn, blocks.data(), blocks.size(), img.w, img.h, out, error))
            return false;
    }
    else
    {
        std::vector<uint32_t> px;
        if (!samples(img, f.big, px, error))
            return false;
        std::vector<uint32_t> palette;
        int palette_format = -1;
        if (indexed && t.palette.ok)
        {
            Level pal;
            std::vector<uint32_t> entries;
            if (!read_level(f, t.palette, pal, error))
                return false;
            if (pal.format < kRGBA5650 || pal.format > kRGBA8888)
            {
                error = "GIM palette format " + std::to_string(pal.format) + " isn't supported";
                return false;
            }
            if (!samples(pal, f.big, entries, error))
                return false;
            palette_format = pal.format;
            for (uint32_t e : entries)
                palette.push_back(straight(pal.format, e));
        }
        out.allocate(img.w, img.h);
        bool opaque = true;
        const uint32_t levels = fmt == kIndex4 ? 16 : 256;
        for (int y = 0; y < img.h; ++y)
        {
            const uint32_t* s = px.data() + size_t(y) * img.w;
            uint32_t* o = out.row(y);
            for (int x = 0; x < img.w; ++x)
            {
                uint32_t c;
                if (!indexed)
                    c = straight(fmt, s[x]);
                else if (palette_format >= 0)
                    c = s[x] < palette.size() ? palette[s[x]] : 0xFF000000u;
                else
                    c = 0xFF000000u | std::min(s[x], levels - 1) * 255 / (levels - 1) * 0x010101u;  // no palette: grey
                opaque &= c >> 24 == 255;
                o[x] = premultiply(c);
            }
        }
        out.has_alpha = !opaque;
        using M = PixelFormat::Model;
        switch (fmt)
        {
            case kRGBA5650: out.pixel_format = PixelFormat::widths(M::RGB, "RGB", {5, 6, 5}); break;
            case kRGBA5551: out.pixel_format = PixelFormat::widths(M::RGB, "RGBA", {5, 5, 5, 1}); break;
            case kRGBA4444: out.pixel_format = PixelFormat::widths(M::RGB, "RGBA", {4, 4, 4, 4}); break;
            case kRGBA8888: out.pixel_format = PixelFormat::packed(M::RGB, "RGBA", 8); break;
            default: out.pixel_format = PixelFormat::palette(format_bits(fmt), int(palette.size())); break;
        }
        out.bits_per_sample = fmt == kRGBA4444 ? 4 : fmt <= kRGBA5551 ? 5 : indexed ? std::min(format_bits(fmt), 8) : 8;
        if (indexed)
            out.add("GIM", "Palette", palette_format >= 0 ? std::to_string(palette.size()) + " × " + format_name(palette_format) : "none, shown in grey");
    }
    if (fmt >= kDXT1)
    {
        out.bits_per_sample = 8;
        out.pixel_format.model = PixelFormat::Model::RGB;
        out.pixel_format.notes.push_back(std::string(format_name(fmt)) + " block compression");
    }
    if (img.order == 1)
        out.pixel_format.notes.push_back("swizzled");

    out.format = "GIM";
    out.add("GIM", "Format", format_name(fmt));
    out.add("GIM", "Pixel order", img.order == 1 ? "faster (swizzled)" : "normal");
    const bool ps3 = std::memcmp(d + 8, "PS3", 3) == 0 || std::memcmp(d + 9, "PS3", 3) == 0;
    out.add("GIM", "Platform", std::string(ps3 ? "PS3" : "PSP") + (f.big ? ", big-endian" : ""));
    if (t.pictures > 1)
        out.add("GIM", "Pictures", std::to_string(t.pictures) + ", the first shown");
    if (img.levels > 1)
        out.add("GIM", "Mipmaps", std::to_string(img.levels));
    if (img.frames > 1)
        out.add("GIM", "Frames", std::to_string(img.frames) + ", the first shown");
    if (t.info.ok)
    {
        // Four NUL-terminated strings: project, user, date saved, the tool.
        static const char* keys[] = {"Project", "User", "Saved", "Originator"};
        size_t p = t.info.data;
        for (const char* key : keys)
        {
            const uint8_t* e = static_cast<const uint8_t*>(std::memchr(d + p, 0, t.info.end - p));
            if (!e)
                break;
            std::string s(reinterpret_cast<const char*>(d + p), size_t(e - (d + p)));
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            if (!s.empty())
                out.add("GIM", key, s);
            p = size_t(e - d) + 1;
            if (p >= t.info.end)
                break;
        }
    }
    return true;
}

}  // namespace

const Decoder& gim_decoder()
{
    static const Decoder d{"GIM", {"gim"}, sniff, decode};
    return d;
}

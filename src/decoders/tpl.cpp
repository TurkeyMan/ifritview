// GameCube and Wii textures: TPL (a big-endian table of images, each with an
// optional palette) and BTI (J3D's single texture, a 32-byte header with no
// magic, claimed by extension only). Both carry the GX texture formats, all
// stored as tiles read left to right, top to bottom, partial tiles at the
// right and bottom edges padded out:
//   I4 (8×8 tiles), I8, IA4 (8×4), IA8, RGB565, RGB5A3 (4×4),
//   RGBA32 (4×4, the AR pairs of a tile in its first 32 bytes, GB in the next),
//   C4 (8×8), C8 (8×4), C14X2 (4×4) indexing an IA8, RGB565 or RGB5A3 palette,
//   CMPR (8×8 tiles of four DXT1-like 4×4 blocks, big-endian, decoded here
//   rather than by bcn.cpp: its colours are blended as the GPU does, at 3/8
//   and 5/8 rather than DXT1's thirds, and its index bits run MSB first).
// Intensity formats are shown opaque grey: the GPU also feeds I to alpha
// when sampling, but that is a use of the texel, not stored transparency.
// The first image of a TPL and the largest mipmap are shown.
#include "decoder.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{

uint16_t be16(const uint8_t* p)
{
    return uint16_t(p[0] << 8 | p[1]);
}

uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

enum Gx : uint32_t
{
    I4 = 0,
    I8 = 1,
    IA4 = 2,
    IA8 = 3,
    RGB565 = 4,
    RGB5A3 = 5,
    RGBA32 = 6,
    C4 = 8,
    C8 = 9,
    C14X2 = 10,
    CMPR = 14,
};

// Tile size and depth of each format; null name for numbers that aren't one.
struct Layout
{
    const char* name;
    int tile_w, tile_h, bits;
};

Layout layout(uint32_t f)
{
    switch (f)
    {
        case I4: return {"I4", 8, 8, 4};
        case I8: return {"I8", 8, 4, 8};
        case IA4: return {"IA4", 8, 4, 8};
        case IA8: return {"IA8", 4, 4, 16};
        case RGB565: return {"RGB565", 4, 4, 16};
        case RGB5A3: return {"RGB5A3", 4, 4, 16};
        case RGBA32: return {"RGBA32", 4, 4, 32};
        case C4: return {"C4", 8, 8, 4};
        case C8: return {"C8", 8, 4, 8};
        case C14X2: return {"C14X2", 4, 4, 16};
        case CMPR: return {"CMPR", 8, 8, 4};
    }
    return {nullptr, 0, 0, 0};
}

bool is_paletted(uint32_t f)
{
    return f == C4 || f == C8 || f == C14X2;
}

const char* palette_name(uint32_t f)
{
    switch (f)
    {
        case 0: return "IA8";
        case 1: return "RGB565";
        case 2: return "RGB5A3";
    }
    return nullptr;
}

// The bytes of a w × h surface: whole tiles, partial ones padded out.
size_t surface_size(const Layout& l, int w, int h)
{
    const size_t tiles = size_t((w + l.tile_w - 1) / l.tile_w) * size_t((h + l.tile_h - 1) / l.tile_h);
    return tiles * size_t(l.tile_w * l.tile_h * l.bits / 8);
}

// Bit replication from n bits to 8, as the GPU expands them.
uint32_t expand3(uint32_t v)
{
    return v << 5 | v << 2 | v >> 1;
}
uint32_t expand4(uint32_t v)
{
    return v << 4 | v;
}
uint32_t expand5(uint32_t v)
{
    return v << 3 | v >> 2;
}
uint32_t expand6(uint32_t v)
{
    return v << 2 | v >> 4;
}

uint32_t argb(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    if (a == 0)
        return 0;
    if (a < 255)
    {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

uint32_t grey(uint32_t i, uint32_t a = 255)
{
    return argb(i, i, i, a);
}

uint32_t rgb565(uint16_t c)
{
    return argb(expand5(c >> 11), expand6(c >> 5 & 0x3F), expand5(c & 0x1F), 255);
}

// Top bit set: opaque RGB555; clear: 3-bit alpha and RGB444.
uint32_t rgb5a3(uint16_t c)
{
    if (c & 0x8000)
        return argb(expand5(c >> 10 & 0x1F), expand5(c >> 5 & 0x1F), expand5(c & 0x1F), 255);
    return argb(expand4(c >> 8 & 0xF), expand4(c >> 4 & 0xF), expand4(c & 0xF), expand3(c >> 12 & 7));
}

// IA8: alpha in the first byte, intensity in the second.
uint32_t ia8(uint16_t c)
{
    return grey(c & 0xFF, c >> 8);
}

uint32_t palette_colour(uint32_t format, uint16_t c)
{
    return format == 0 ? ia8(c) : format == 1 ? rgb565(c) : rgb5a3(c);
}

// One CMPR sub-block (8 bytes) into a 4 × 4 square of `tile` (row pitch 8).
void cmpr_block(const uint8_t* s, uint32_t* tile)
{
    const uint16_t c0 = be16(s), c1 = be16(s + 2);
    const uint32_t r0 = expand5(c0 >> 11), g0 = expand6(c0 >> 5 & 0x3F), b0 = expand5(c0 & 0x1F);
    const uint32_t r1 = expand5(c1 >> 11), g1 = expand6(c1 >> 5 & 0x3F), b1 = expand5(c1 & 0x1F);
    uint32_t colour[4];
    colour[0] = argb(r0, g0, b0, 255);
    colour[1] = argb(r1, g1, b1, 255);
    if (c0 > c1)
    {
        const auto blend = [](uint32_t a, uint32_t b) { return (a * 5 + b * 3) >> 3; };
        colour[2] = argb(blend(r0, r1), blend(g0, g1), blend(b0, b1), 255);
        colour[3] = argb(blend(r1, r0), blend(g1, g0), blend(b1, b0), 255);
    }
    else
    {
        colour[2] = argb((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        colour[3] = 0;  // the same average, transparent
    }
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            tile[y * 8 + x] = colour[s[4 + y] >> (6 - 2 * x) & 3];
}

// One tile of `l` at `s` into `tile` (row pitch tile_w), premultiplied ARGB.
void decode_tile(uint32_t format, const Layout& l, const uint8_t* s, const std::vector<uint32_t>& palette, uint32_t* tile)
{
    const int n = l.tile_w * l.tile_h;
    const auto lookup = [&](uint32_t i) { return i < palette.size() ? palette[i] : 0u; };
    switch (format)
    {
        case I4:
            for (int i = 0; i < n; ++i)
                tile[i] = grey(expand4(s[i / 2] >> (i & 1 ? 0 : 4) & 0xF));
            break;
        case I8:
            for (int i = 0; i < n; ++i)
                tile[i] = grey(s[i]);
            break;
        case IA4:  // alpha in the high nibble
            for (int i = 0; i < n; ++i)
                tile[i] = grey(expand4(s[i] & 0xF), expand4(s[i] >> 4));
            break;
        case IA8:
            for (int i = 0; i < n; ++i)
                tile[i] = ia8(be16(s + 2 * i));
            break;
        case RGB565:
            for (int i = 0; i < n; ++i)
                tile[i] = rgb565(be16(s + 2 * i));
            break;
        case RGB5A3:
            for (int i = 0; i < n; ++i)
                tile[i] = rgb5a3(be16(s + 2 * i));
            break;
        case RGBA32:
            for (int i = 0; i < n; ++i)
                tile[i] = argb(s[2 * i + 1], s[32 + 2 * i], s[33 + 2 * i], s[2 * i]);
            break;
        case C4:
            for (int i = 0; i < n; ++i)
                tile[i] = lookup(s[i / 2] >> (i & 1 ? 0 : 4) & 0xF);
            break;
        case C8:
            for (int i = 0; i < n; ++i)
                tile[i] = lookup(s[i]);
            break;
        case C14X2:
            for (int i = 0; i < n; ++i)
                tile[i] = lookup(be16(s + 2 * i) & 0x3FFF);
            break;
        case CMPR:  // sub-blocks top-left, top-right, bottom-left, bottom-right
            for (int b = 0; b < 4; ++b)
                cmpr_block(s + 8 * b, tile + (b / 2) * 32 + (b % 2) * 4);
            break;
    }
}

// Decodes a w × h surface of `format` at `data`, `size` bytes available.
bool decode_surface(uint32_t format, const uint8_t* data, size_t size, int w, int h, const std::vector<uint32_t>& palette,
                    Image& out, std::string& error)
{
    const Layout l = layout(format);
    if (size < surface_size(l, w, h))
    {
        error = "texture data is cut short";
        return false;
    }
    out.allocate(w, h);
    const size_t tile_bytes = size_t(l.tile_w * l.tile_h * l.bits / 8);
    uint32_t tile[64];
    bool opaque = true;
    for (int ty = 0; ty < h; ty += l.tile_h)
        for (int tx = 0; tx < w; tx += l.tile_w, data += tile_bytes)
        {
            decode_tile(format, l, data, palette, tile);
            for (int y = 0; y < l.tile_h && ty + y < h; ++y)
            {
                uint32_t* px = out.row(ty + y) + tx;
                for (int x = 0; x < l.tile_w && tx + x < w; ++x)
                {
                    px[x] = tile[y * l.tile_w + x];
                    opaque = opaque && px[x] >> 24 == 255;
                }
            }
        }
    out.has_alpha = !opaque;
    return true;
}

// The palette's entries, decoded; false if they run past the file.
bool read_palette(const uint8_t* d, size_t size, size_t offset, uint32_t count, uint32_t format, std::vector<uint32_t>& palette,
                  std::string& error)
{
    if (!palette_name(format))
    {
        error = "palette format " + std::to_string(format) + " isn't a GX one";
        return false;
    }
    if (offset > size || (size - offset) / 2 < count)
    {
        error = "palette is cut short";
        return false;
    }
    palette.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        palette[i] = palette_colour(format, be16(d + offset + 2 * i));
    return true;
}

void describe(Image& out, const char* group, uint32_t format, int palette_format, size_t palette_entries, int mips)
{
    const Layout l = layout(format);
    out.format = group;
    out.bits_per_sample = 8;
    using M = PixelFormat::Model;
    switch (format)
    {
        case I4: out.pixel_format = PixelFormat::packed(M::L, "L", 4); break;  // GameCube calls grey "intensity"
        case I8: out.pixel_format = PixelFormat::packed(M::L, "L", 8); break;
        case IA4: out.pixel_format = PixelFormat::packed(M::L, "AL", 4); break;
        case IA8: out.pixel_format = PixelFormat::packed(M::L, "AL", 8); break;
        case RGB565: out.pixel_format = PixelFormat::widths(M::RGB, "RGB", {5, 6, 5}); break;
        case RGB5A3:
            out.pixel_format = PixelFormat::widths(M::RGB, "RGB", {5, 5, 5}, 16);
            out.pixel_format.notes.push_back("or ARGB 3-4-4-4");
            break;
        case RGBA32: out.pixel_format = PixelFormat::packed(M::RGB, "ARGB", 8); break;
        case C4:
        case C8:
        case C14X2: out.pixel_format = PixelFormat::palette(format == C4 ? 4 : format == C8 ? 8 : 14, int(palette_entries)); break;
        case CMPR:
            out.pixel_format = PixelFormat::widths(M::RGB, "RGB", {5, 6, 5}, 4);  // the endpoints
            out.pixel_format.notes.push_back("CMPR block compression");
            break;
    }
    out.pixel_format.notes.push_back(std::to_string(l.tile_w) + "×" + std::to_string(l.tile_h) + " tiles");
    out.add(group, "Format", l.name);
    if (palette_format >= 0)
        out.add(group, "Palette", std::string(palette_name(uint32_t(palette_format))) + ", " + std::to_string(palette_entries) + " entries");
    if (mips > 1)
        out.add(group, "Mipmaps", std::to_string(mips) + ", the largest shown");
}

// TPL: magic, image count, offset of the table of (image header, palette
// header) offset pairs.
constexpr uint32_t kTplMagic = 0x0020AF30;

bool tpl_sniff(const uint8_t* h, size_t n)
{
    return n >= 12 && be32(h) == kTplMagic && be32(h + 4) > 0;
}

bool tpl_decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || !tpl_sniff(d, size))
    {
        error = "not a TPL file";
        return false;
    }
    const uint32_t count = be32(d + 4), table = be32(d + 8);
    if (table > size || size - table < 8)
    {
        error = "TPL image table is cut short";
        return false;
    }
    const uint32_t image = be32(d + table), pal = be32(d + table + 4);
    // Image header: height, width, format, data offset, wrap S/T, filters,
    // LOD bias, edge LOD, min LOD, max LOD, unpacked.
    if (image > size || size - image < 36)
    {
        error = "TPL image header is cut short";
        return false;
    }
    const int h = be16(d + image), w = be16(d + image + 2);
    const uint32_t format = be32(d + image + 4), data = be32(d + image + 8);
    const int min_lod = d[image + 33], max_lod = d[image + 34];
    const Layout l = layout(format);
    if (!l.name)
    {
        error = "TPL format " + std::to_string(format) + " isn't a GX one";
        return false;
    }
    if (w == 0 || h == 0)
    {
        error = "bad TPL size";
        return false;
    }
    std::vector<uint32_t> palette;
    int palette_format = -1;
    if (is_paletted(format))
    {
        // Palette header: entry count, unpacked flag, padding, format, data offset.
        if (pal == 0 || pal > size || size - pal < 12)
        {
            error = "TPL palette header is missing or cut short";
            return false;
        }
        palette_format = int(be32(d + pal + 4));
        if (!read_palette(d, size, be32(d + pal + 8), be16(d + pal), uint32_t(palette_format), palette, error))
            return false;
    }
    if (data > size)
    {
        error = "texture data is cut short";
        return false;
    }
    if (!decode_surface(format, d + data, size - data, w, h, palette, out, error))
        return false;
    describe(out, "TPL", format, palette_format, palette.size(), max_lod > min_lod ? max_lod - min_lod + 1 : 1);
    if (count > 1)
        out.add("TPL", "Images", std::to_string(count) + ", the first shown");
    return true;
}

// BTI header (big-endian): format, alpha setting, width, height, wrap S/T,
// palettes enabled, palette format, palette entries, palette offset, ...,
// filters, LOD range, mipmap count, LOD bias, image data offset. Offsets
// count from the header, at the start of a standalone file.
bool bti_decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 32)
    {
        error = "not a BTI file";
        return false;
    }
    const uint32_t format = d[0];
    const int w = be16(d + 2), h = be16(d + 4);
    const int palette_format = d[9];
    const uint32_t entries = be16(d + 10), palette_offset = be32(d + 12);
    const int mips = d[0x18];
    const uint32_t data = be32(d + 0x1C);
    // No magic, so the header has to make sense: a GX format, a size the
    // GPU can sample (1024 at most), sane wrap modes and data after it.
    const Layout l = layout(format);
    if (!l.name || w == 0 || h == 0 || w > 1024 || h > 1024 || d[6] > 2 || d[7] > 2 || d[8] > 1 || data < 32)
    {
        error = "not a BTI file";
        return false;
    }
    std::vector<uint32_t> palette;
    if (is_paletted(format))
    {
        if (!read_palette(d, size, palette_offset, entries, uint32_t(palette_format), palette, error))
            return false;
    }
    if (data > size)
    {
        error = "texture data is cut short";
        return false;
    }
    if (!decode_surface(format, d + data, size - data, w, h, palette, out, error))
        return false;
    describe(out, "BTI", format, is_paletted(format) ? palette_format : -1, palette.size(), mips);
    return true;
}

}  // namespace

const Decoder& tpl_decoder()
{
    static const Decoder d{"TPL", {"tpl"}, tpl_sniff, tpl_decode};
    return d;
}

const Decoder& bti_decoder()
{
    static const Decoder d{"BTI", {"bti"}, nullptr, bti_decode};
    return d;
}

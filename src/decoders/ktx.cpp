// Khronos textures, KTX 1 and KTX 2: a header naming the pixel format (GL
// enums for KTX 1, a VkFormat for KTX 2), key/value metadata, then the
// mipmap levels, largest first. The first image is shown: level 0, its
// first array layer, cube face or volume slice. Block-compressed data goes
// through bcn.cpp (BC1-BC7), etc.cpp (ETC1/ETC2/EAC) and astc.cpp (ASTC);
// uncompressed data in the common 8-bit and packed 16-bit layouts is read
// here. KTX 1 comes in either byte order. KTX 2 levels may be supercompressed
// with Zstandard (when built with libzstd); Basis Universal's BasisLZ/ETC1S
// and UASTC need its transcoder and aren't supported, nor is zlib.
// KTXorientation is honoured, so a bottom-up texture shows the right way up.
#include "astc.h"
#include "bcn.h"
#include "decoder.h"
#include "texel.h"
#include "etc.h"

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{

const uint8_t kKtx1Id[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
const uint8_t kKtx2Id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t le64(const uint8_t* p)
{
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}

uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

bool sniff1(const uint8_t* h, size_t n)
{
    return n >= 12 && std::memcmp(h, kKtx1Id, 12) == 0;
}

bool sniff2(const uint8_t* h, size_t n)
{
    return n >= 12 && std::memcmp(h, kKtx2Id, 12) == 0;
}

// Uncompressed pixels described by masks over a word of `bytes` bytes, read
// little-endian (or big-endian, for 16-bit words in a big-endian KTX 1).
struct Masks
{
    int bytes;
    uint32_t r, g, b, a;
    bool luminance = false;   // r is grey
    bool alpha_only = false;  // a alone, shown grey
};

// Wider samples (texel.h).
using Wide = WideTexels;

// What a file's format is, for the decoders here.
struct Texture
{
    std::optional<Bcn> bcn;
    std::optional<Etc> etc;
    int astc_w = 0, astc_h = 0;
    AstcProfile astc_profile = AstcProfile::LDR;
    std::optional<Masks> masks;
    std::optional<Wide> wide;
    bool opaque_bc1 = false;  // BC1 without alpha: its transparent texels are black
    std::string name;
};

Texture bcn_texture(Bcn f, bool srgb, bool opaque = false)
{
    Texture t;
    t.bcn = f;
    t.opaque_bc1 = opaque;
    t.name = std::string(bcn_name(f)) + (opaque ? " RGB" : "") + (srgb ? ", sRGB" : "");
    return t;
}

Texture etc_texture(Etc f, bool srgb)
{
    Texture t;
    t.etc = f;
    t.name = std::string(etc_name(f)) + (srgb ? ", sRGB" : "");
    return t;
}

// ASTC footprints in the order both GL and Vulkan number them.
const int kAstcFootprints[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

Texture astc_texture(int index, AstcProfile profile)
{
    Texture t;
    t.astc_w = kAstcFootprints[index][0];
    t.astc_h = kAstcFootprints[index][1];
    t.astc_profile = profile;
    t.name = "ASTC " + std::to_string(t.astc_w) + "x" + std::to_string(t.astc_h) +
             (profile == AstcProfile::sRGB ? ", sRGB" : profile == AstcProfile::HDR ? ", HDR" : "");
    return t;
}

Texture masks_texture(Masks m, const char* name)
{
    Texture t;
    t.masks = m;
    t.name = name;
    return t;
}

Texture wide_texture(WideTexels::Kind kind, int channels, const char* name)
{
    Texture t;
    t.wide = Wide{kind, channels};
    t.name = name;
    return t;
}

// KTX 1: compressed formats by glInternalFormat; uncompressed ones by
// glFormat and glType.
std::optional<Texture> gl_texture(uint32_t internal, uint32_t format, uint32_t type)
{
    if (type == 0)
    {
        switch (internal)
        {
            case 0x83F0: return bcn_texture(Bcn::BC1, false, true);  // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
            case 0x8C4C: return bcn_texture(Bcn::BC1, true, true);
            case 0x83F1: return bcn_texture(Bcn::BC1, false);  // ..._RGBA_S3TC_DXT1_EXT
            case 0x8C4D: return bcn_texture(Bcn::BC1, true);
            case 0x83F2: return bcn_texture(Bcn::BC2, false);
            case 0x8C4E: return bcn_texture(Bcn::BC2, true);
            case 0x83F3: return bcn_texture(Bcn::BC3, false);
            case 0x8C4F: return bcn_texture(Bcn::BC3, true);
            case 0x8DBB: return bcn_texture(Bcn::BC4, false);  // GL_COMPRESSED_RED_RGTC1
            case 0x8DBC: return bcn_texture(Bcn::BC4S, false);
            case 0x8DBD: return bcn_texture(Bcn::BC5, false);  // GL_COMPRESSED_RG_RGTC2
            case 0x8DBE: return bcn_texture(Bcn::BC5S, false);
            case 0x8E8C: return bcn_texture(Bcn::BC7, false);  // GL_COMPRESSED_RGBA_BPTC_UNORM
            case 0x8E8D: return bcn_texture(Bcn::BC7, true);
            case 0x8E8E: return bcn_texture(Bcn::BC6HS, false);  // GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT
            case 0x8E8F: return bcn_texture(Bcn::BC6H, false);
            case 0x8D64: return etc_texture(Etc::ETC1, false);  // GL_ETC1_RGB8_OES
            case 0x9270: return etc_texture(Etc::EAC_R11, false);  // GL_COMPRESSED_R11_EAC
            case 0x9271: return etc_texture(Etc::EAC_R11S, false);
            case 0x9272: return etc_texture(Etc::EAC_RG11, false);
            case 0x9273: return etc_texture(Etc::EAC_RG11S, false);
            case 0x9274: return etc_texture(Etc::ETC2_RGB, false);  // GL_COMPRESSED_RGB8_ETC2
            case 0x9275: return etc_texture(Etc::ETC2_RGB, true);
            case 0x9276: return etc_texture(Etc::ETC2_RGBA1, false);
            case 0x9277: return etc_texture(Etc::ETC2_RGBA1, true);
            case 0x9278: return etc_texture(Etc::ETC2_RGBA, false);
            case 0x9279: return etc_texture(Etc::ETC2_RGBA, true);
        }
        // GL_COMPRESSED_RGBA_ASTC_4x4_KHR... and the SRGB8_ALPHA8 ones. GL
        // has one enum for LDR and HDR: the blocks tell.
        if (internal >= 0x93B0 && internal <= 0x93BD)
            return astc_texture(int(internal - 0x93B0), AstcProfile::Auto);
        if (internal >= 0x93D0 && internal <= 0x93DD)
            return astc_texture(int(internal - 0x93D0), AstcProfile::sRGB);
        return std::nullopt;
    }
    constexpr uint32_t UNSIGNED_BYTE = 0x1401, UNSIGNED_SHORT_5_6_5 = 0x8363, UNSIGNED_SHORT_4_4_4_4 = 0x8033, UNSIGNED_SHORT_5_5_5_1 = 0x8034;
    constexpr uint32_t ALPHA = 0x1906, RGB = 0x1907, RGBA = 0x1908, LUMINANCE = 0x1909, LUMINANCE_ALPHA = 0x190A, RED = 0x1903, RG = 0x8227, BGR = 0x80E0, BGRA = 0x80E1;
    if (type == UNSIGNED_BYTE)
        switch (format)
        {
            case RGBA: return masks_texture({4, 0xFF, 0xFF00, 0xFF0000, 0xFF000000u}, "RGBA8");
            case BGRA: return masks_texture({4, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u}, "BGRA8");
            case RGB: return masks_texture({3, 0xFF, 0xFF00, 0xFF0000, 0}, "RGB8");
            case BGR: return masks_texture({3, 0xFF0000, 0xFF00, 0xFF, 0}, "BGR8");
            case RG: return masks_texture({2, 0xFF, 0xFF00, 0, 0}, "RG8");
            case RED: return masks_texture({1, 0xFF, 0, 0, 0, true}, "R8");
            case LUMINANCE: return masks_texture({1, 0xFF, 0, 0, 0, true}, "Luminance 8");
            case LUMINANCE_ALPHA: return masks_texture({2, 0xFF, 0, 0, 0xFF00, true}, "Luminance-alpha 8");
            case ALPHA: return masks_texture({1, 0, 0, 0, 0xFF, false, true}, "Alpha 8");
        }
    constexpr uint32_t UNSIGNED_SHORT = 0x1403, HALF_FLOAT = 0x140B, FLOAT = 0x1406, UNSIGNED_INT_10F_11F_11F_REV = 0x8C3B, UNSIGNED_INT_5_9_9_9_REV = 0x8C3E;
    const int channels = format == RED ? 1 : format == RG ? 2 : format == RGB ? 3 : format == RGBA ? 4 : 0;
    if (channels && (type == UNSIGNED_SHORT || type == HALF_FLOAT || type == FLOAT))
    {
        static const char* const names[3][4] = {{"R16", "RG16", "RGB16", "RGBA16"},
                                                 {"R16F", "RG16F", "RGB16F", "RGBA16F"},
                                                 {"R32F", "RG32F", "RGB32F", "RGBA32F"}};
        const int kind = type == UNSIGNED_SHORT ? 0 : type == HALF_FLOAT ? 1 : 2;
        static const WideTexels::Kind kinds[3] = {WideTexels::Kind::Unorm16, WideTexels::Kind::Half, WideTexels::Kind::Float};
        return wide_texture(kinds[kind], channels, names[kind][channels - 1]);
    }
    if (type == UNSIGNED_INT_10F_11F_11F_REV && format == RGB)
        return wide_texture(WideTexels::Kind::B10G11R11, 3, "R11F G11F B10F");
    if (type == UNSIGNED_INT_5_9_9_9_REV && format == RGB)
        return wide_texture(WideTexels::Kind::E5B9G9R9, 3, "RGB9 E5");
    if (type == UNSIGNED_SHORT_5_6_5 && format == RGB)
        return masks_texture({2, 0xF800, 0x07E0, 0x001F, 0}, "RGB565");
    if (type == UNSIGNED_SHORT_4_4_4_4 && format == RGBA)
        return masks_texture({2, 0xF000, 0x0F00, 0x00F0, 0x000F}, "RGBA4");
    if (type == UNSIGNED_SHORT_5_5_5_1 && format == RGBA)
        return masks_texture({2, 0xF800, 0x07C0, 0x003E, 0x0001}, "RGB5 A1");
    return std::nullopt;
}

// KTX 2: by VkFormat.
std::optional<Texture> vk_texture(uint32_t f)
{
    switch (f)
    {
        case 131: return bcn_texture(Bcn::BC1, false, true);  // VK_FORMAT_BC1_RGB_UNORM_BLOCK
        case 132: return bcn_texture(Bcn::BC1, true, true);
        case 133: return bcn_texture(Bcn::BC1, false);
        case 134: return bcn_texture(Bcn::BC1, true);
        case 135: return bcn_texture(Bcn::BC2, false);
        case 136: return bcn_texture(Bcn::BC2, true);
        case 137: return bcn_texture(Bcn::BC3, false);
        case 138: return bcn_texture(Bcn::BC3, true);
        case 139: return bcn_texture(Bcn::BC4, false);
        case 140: return bcn_texture(Bcn::BC4S, false);
        case 141: return bcn_texture(Bcn::BC5, false);
        case 142: return bcn_texture(Bcn::BC5S, false);
        case 143: return bcn_texture(Bcn::BC6H, false);
        case 144: return bcn_texture(Bcn::BC6HS, false);
        case 145: return bcn_texture(Bcn::BC7, false);
        case 146: return bcn_texture(Bcn::BC7, true);
        case 147: return etc_texture(Etc::ETC2_RGB, false);  // VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK
        case 148: return etc_texture(Etc::ETC2_RGB, true);
        case 149: return etc_texture(Etc::ETC2_RGBA1, false);
        case 150: return etc_texture(Etc::ETC2_RGBA1, true);
        case 151: return etc_texture(Etc::ETC2_RGBA, false);
        case 152: return etc_texture(Etc::ETC2_RGBA, true);
        case 153: return etc_texture(Etc::EAC_R11, false);
        case 154: return etc_texture(Etc::EAC_R11S, false);
        case 155: return etc_texture(Etc::EAC_RG11, false);
        case 156: return etc_texture(Etc::EAC_RG11S, false);
        case 9: return masks_texture({1, 0xFF, 0, 0, 0, true}, "R8");  // VK_FORMAT_R8_UNORM
        case 15: return masks_texture({1, 0xFF, 0, 0, 0, true}, "R8, sRGB");
        case 16: return masks_texture({2, 0xFF, 0xFF00, 0, 0}, "R8G8");
        case 22: return masks_texture({2, 0xFF, 0xFF00, 0, 0}, "R8G8, sRGB");
        case 23: return masks_texture({3, 0xFF, 0xFF00, 0xFF0000, 0}, "R8G8B8");
        case 29: return masks_texture({3, 0xFF, 0xFF00, 0xFF0000, 0}, "R8G8B8, sRGB");
        case 30: return masks_texture({3, 0xFF0000, 0xFF00, 0xFF, 0}, "B8G8R8");
        case 36: return masks_texture({3, 0xFF0000, 0xFF00, 0xFF, 0}, "B8G8R8, sRGB");
        case 37: return masks_texture({4, 0xFF, 0xFF00, 0xFF0000, 0xFF000000u}, "R8G8B8A8");
        case 43: return masks_texture({4, 0xFF, 0xFF00, 0xFF0000, 0xFF000000u}, "R8G8B8A8, sRGB");
        case 44: return masks_texture({4, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u}, "B8G8R8A8");
        case 50: return masks_texture({4, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u}, "B8G8R8A8, sRGB");
        case 2: return masks_texture({2, 0xF000, 0x0F00, 0x00F0, 0x000F}, "R4G4B4A4");  // VK_FORMAT_R4G4B4A4_UNORM_PACK16
        case 3: return masks_texture({2, 0x00F0, 0x0F00, 0xF000, 0x000F}, "B4G4R4A4");
        case 4: return masks_texture({2, 0xF800, 0x07E0, 0x001F, 0}, "R5G6B5");
        case 5: return masks_texture({2, 0x001F, 0x07E0, 0xF800, 0}, "B5G6R5");
        case 6: return masks_texture({2, 0xF800, 0x07C0, 0x003E, 0x0001}, "R5G5B5A1");
        case 7: return masks_texture({2, 0x003E, 0x07C0, 0xF800, 0x0001}, "B5G5R5A1");
        case 8: return masks_texture({2, 0x7C00, 0x03E0, 0x001F, 0x8000}, "A1R5G5B5");
        case 70: return wide_texture(WideTexels::Kind::Unorm16, 1, "R16");  // VK_FORMAT_R16_UNORM
        case 77: return wide_texture(WideTexels::Kind::Unorm16, 2, "R16G16");
        case 84: return wide_texture(WideTexels::Kind::Unorm16, 3, "R16G16B16");
        case 91: return wide_texture(WideTexels::Kind::Unorm16, 4, "R16G16B16A16");
        case 76: return wide_texture(WideTexels::Kind::Half, 1, "R16 float");  // VK_FORMAT_R16_SFLOAT
        case 83: return wide_texture(WideTexels::Kind::Half, 2, "R16G16 float");
        case 90: return wide_texture(WideTexels::Kind::Half, 3, "R16G16B16 float");
        case 97: return wide_texture(WideTexels::Kind::Half, 4, "R16G16B16A16 float");
        case 100: return wide_texture(WideTexels::Kind::Float, 1, "R32 float");  // VK_FORMAT_R32_SFLOAT
        case 103: return wide_texture(WideTexels::Kind::Float, 2, "R32G32 float");
        case 106: return wide_texture(WideTexels::Kind::Float, 3, "R32G32B32 float");
        case 109: return wide_texture(WideTexels::Kind::Float, 4, "R32G32B32A32 float");
        case 122: return wide_texture(WideTexels::Kind::B10G11R11, 3, "B10G11R11 float");
        case 123: return wide_texture(WideTexels::Kind::E5B9G9R9, 3, "E5B9G9R9 float");
    }
    // VK_FORMAT_ASTC_4x4_UNORM_BLOCK..., alternating with the _SRGB_BLOCKs,
    // then VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK... for HDR.
    if (f >= 157 && f <= 184)
        return astc_texture(int(f - 157) / 2, (f - 157) % 2 ? AstcProfile::sRGB : AstcProfile::LDR);
    if (f >= 1000066000 && f <= 1000066013)
        return astc_texture(int(f - 1000066000), AstcProfile::HDR);
    return std::nullopt;
}

// The bytes of the first w × h image (rows of uncompressed data padded to
// `row_align` bytes).
size_t image_bytes(const Texture& t, int w, int h, size_t row_align)
{
    if (t.bcn)
        return bcn_size(*t.bcn, w, h);
    if (t.etc)
        return etc_size(*t.etc, w, h);
    if (t.astc_w)
        return astc_size(t.astc_w, t.astc_h, w, h);
    const size_t row = (size_t(w) * (t.masks ? t.masks->bytes : t.wide->pixel_bytes()) + row_align - 1) / row_align * row_align;
    return row * h;
}

uint32_t extract(uint32_t v, uint32_t mask)
{
    if (!mask)
        return 0;
    int shift = 0;
    while (!(mask >> shift & 1))
        ++shift;
    const uint32_t max = mask >> shift;
    return uint32_t((uint64_t((v & mask) >> shift) * 255 + max / 2) / max);
}

bool decode_masks(const Masks& m, const uint8_t* d, size_t size, int w, int h, size_t row_align, bool big_endian, bool premultiplied,
                  Image& out, std::string& error)
{
    const size_t row = (size_t(w) * m.bytes + row_align - 1) / row_align * row_align;
    if (size < row * h)
    {
        error = "KTX data is cut short";
        return false;
    }
    out.allocate(w, h);
    bool opaque = true;
    for (int y = 0; y < h; ++y)
    {
        uint32_t* px = out.row(y);
        const uint8_t* s = d + size_t(y) * row;
        for (int x = 0; x < w; ++x, s += m.bytes)
        {
            uint32_t v = 0;
            for (int i = 0; i < m.bytes; ++i)
                v |= uint32_t(s[i]) << (8 * i);
            if (big_endian && m.bytes == 2)
                v = (v >> 8 | v << 8) & 0xFFFF;
            uint32_t r, g, b, a = 255;
            if (m.alpha_only)
                r = g = b = extract(v, m.a);
            else
            {
                r = extract(v, m.r);
                g = m.luminance ? r : extract(v, m.g);
                b = m.luminance ? r : extract(v, m.b);
                if (m.a)
                    a = extract(v, m.a);
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
    out.has_alpha = !opaque;
    return true;
}

// The channels a mask layout holds, in R, G, B, A order, with their widths.
PixelFormat masks_pixel_format(const Masks& m)
{
    const auto bits = [](uint32_t mask)
    {
        int n = 0;
        for (; mask; mask &= mask - 1)
            ++n;
        return n;
    };
    using M = PixelFormat::Model;
    if (m.alpha_only)
        return PixelFormat::widths(M::L, "A", {bits(m.a)}, m.bytes * 8);
    if (m.luminance)
        return m.a ? PixelFormat::widths(M::L, "LA", {bits(m.r), bits(m.a)}, m.bytes * 8) : PixelFormat::widths(M::L, "L", {bits(m.r)}, m.bytes * 8);
    std::string names;
    std::vector<int> widths;
    for (const auto& [name, mask] : {std::pair{'R', m.r}, std::pair{'G', m.g}, std::pair{'B', m.b}, std::pair{'A', m.a}})
        if (mask)
        {
            names += name;
            widths.push_back(bits(mask));
        }
    return PixelFormat::widths(M::RGB, names.c_str(), widths, m.bytes * 8);
}

// Decodes the first image and fills in what's common to both versions.
// `big_endian`: samples wider than a byte are stored most significant byte
// first (a big-endian KTX 1); rows of uncompressed data are padded to
// `row_align` bytes.
bool decode_texture(const Texture& t, const uint8_t* d, size_t size, int w, int h, size_t row_align, bool big_endian, bool premultiplied,
                    Image& out, std::string& error)
{
    bool ok;
    if (t.bcn)
        ok = decode_bcn(*t.bcn, d, size, w, h, out, error, premultiplied);
    else if (t.etc)
        ok = decode_etc(*t.etc, d, size, w, h, out, error, premultiplied);
    else if (t.astc_w)
        ok = decode_astc(d, size, t.astc_w, t.astc_h, w, h, t.astc_profile, out, error, premultiplied);
    else if (t.wide)
        ok = decode_wide(*t.wide, d, size, w, h, row_align, big_endian, premultiplied, out, error);
    else
        ok = decode_masks(*t.masks, d, size, w, h, row_align, big_endian, premultiplied, out, error);
    if (!ok)
        return false;
    if (t.opaque_bc1)
    {
        for (int y = 0; y < h; ++y)
            for (uint32_t* px = out.row(y); px != out.row(y) + w; ++px)
                *px |= 0xFF000000u;  // transparent texels hold black
        out.has_alpha = false;
    }

    using M = PixelFormat::Model;
    if (t.bcn)
    {
        const Bcn f = *t.bcn;
        const bool hdr = f == Bcn::BC6H || f == Bcn::BC6HS;
        out.bits_per_sample = hdr ? 16 : 8;
        out.pixel_format = f == Bcn::BC4 || f == Bcn::BC4S   ? PixelFormat::packed(M::L, "R", 8)
                           : f == Bcn::BC5 || f == Bcn::BC5S ? PixelFormat::packed(M::RGB, "RG", 8)
                           : hdr                             ? PixelFormat::packed(M::RGB, "RGB", 16)
                           : t.opaque_bc1                    ? PixelFormat::packed(M::RGB, "RGB", 8)
                                                             : PixelFormat::packed(M::RGB, "RGBA", 8);
        if (hdr)
            out.pixel_format.sample = PixelFormat::Sample::Float;
        else if (f == Bcn::BC4S || f == Bcn::BC5S)
            out.pixel_format.sample = PixelFormat::Sample::Signed;
        out.pixel_format.bits_per_pixel = int(bcn_block_bytes(f)) * 8 / 16;
        out.pixel_format.notes.push_back(std::string(bcn_name(f)) + " block compression");
    }
    else if (t.etc)
    {
        out.bits_per_sample = *t.etc >= Etc::EAC_R11 ? 11 : 8;
        out.pixel_format = etc_pixel_format(*t.etc);
    }
    else if (t.astc_w)
        out.pixel_format = astc_pixel_format(t.astc_w, t.astc_h, out.bits_per_sample > 8);  // decode_astc set the depth
    else if (t.wide)
    {
        const Wide& f = *t.wide;
        static const char* const names[4] = {"R", "RG", "RGB", "RGBA"};
        if (f.kind == WideTexels::Kind::B10G11R11)
            out.pixel_format = PixelFormat::widths(M::RGB, "RGB", {11, 11, 10});
        else if (f.kind == WideTexels::Kind::E5B9G9R9)
            out.pixel_format = PixelFormat::widths(M::RGB, "RGBX", {9, 9, 9, 5});
        else
            out.pixel_format = PixelFormat::packed(f.channels == 1 ? M::L : M::RGB, names[f.channels - 1], f.kind == WideTexels::Kind::Float ? 32 : 16);
        if (f.kind != WideTexels::Kind::Unorm16)
            out.pixel_format.sample = PixelFormat::Sample::Float;
        out.bits_per_sample = f.kind == WideTexels::Kind::Float ? 32 : 16;
    }
    else
    {
        out.bits_per_sample = 8;
        out.pixel_format = masks_pixel_format(*t.masks);
    }
    out.pixel_format.premultiplied = premultiplied;
    return true;
}

// Text values only: some keys hold binary data.
bool printable(const std::string& s)
{
    for (unsigned char c : s)
        if (c < 32 && c != '\t' && c != '\n')
            return false;
    return !s.empty();
}

// Key/value pairs: each a 32-bit length, the key and its NUL, the value
// (text values carry a NUL too), then padding to 4 bytes.
std::vector<std::pair<std::string, std::string>> key_values(const uint8_t* p, size_t n, bool big_endian)
{
    std::vector<std::pair<std::string, std::string>> kv;
    size_t i = 0;
    while (i + 4 <= n)
    {
        const size_t len = big_endian ? be32(p + i) : le32(p + i);
        i += 4;
        if (len > n - i)
            break;
        const char* s = reinterpret_cast<const char*>(p + i);
        const void* nul = std::memchr(s, '\0', len);
        const size_t key_len = nul ? size_t(static_cast<const char*>(nul) - s) : len;
        if (key_len < len)
        {
            std::string value(s + key_len + 1, len - key_len - 1);
            while (!value.empty() && value.back() == '\0')
                value.pop_back();
            kv.emplace_back(std::string(s, key_len), value);
        }
        i += (len + 3) & ~size_t(3);
    }
    return kv;
}

// KTXorientation ("S=r,T=d" in KTX 1, "rd" in KTX 2): the direction of
// increasing x and y. Anything but right and down is flipped to be so.
void orient(Image& out, const std::string& value)
{
    bool left = false, up = false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (c == 'S' || c == 'T' || c == 'R' || c == '=' || c == ',')
            continue;
        left |= c == 'l';
        up |= c == 'u';
    }
    if (left || up)
        out.apply_orientation(left && up ? 3 : up ? 4 : 2);
}

void add_metadata(Image& out, const char* group, const std::vector<std::pair<std::string, std::string>>& kv)
{
    for (const auto& [key, value] : kv)
    {
        if (key == "KTXorientation")
            orient(out, value);
        if (!printable(value) || value.size() > 1000)
            continue;
        if (key == "KTXwriter")
            out.add(group, "Writer", value);
        else if (key == "KTXorientation")
            out.add(group, "Orientation", value);
        else
            out.add(group, key, value);
    }
}

void add_layout(Image& out, const char* group, uint32_t levels, uint32_t layers, uint32_t faces, uint32_t depth)
{
    if (levels > 1)
        out.add(group, "Mipmaps", std::to_string(levels));
    if (faces == 6)
        out.add(group, "Cube map", "first face shown");
    if (layers > 1)
        out.add(group, "Array", std::to_string(layers) + " textures, the first shown");
    if (depth > 1)
        out.add(group, "Volume", std::to_string(depth) + " slices, the first shown");
}

bool check_size(uint32_t w, uint32_t h, const char* what, std::string& error)
{
    if (!Image::fits(w, h))
    {
        error = std::string("bad ") + what + " size";
        return false;
    }
    return true;
}

bool decode1(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 64 || !sniff1(d, size))
    {
        error = "not a KTX file";
        return false;
    }
    // The endianness word is 0x04030201 as the writer stored it.
    const uint32_t endianness = le32(d + 12);
    if (endianness != 0x04030201 && endianness != 0x01020304)
    {
        error = "bad KTX byte order";
        return false;
    }
    const bool be = endianness == 0x01020304;
    const auto u32 = [&](size_t offset) { return be ? be32(d + offset) : le32(d + offset); };
    const uint32_t type = u32(16), type_size = u32(20), format = u32(24), internal = u32(28);
    const uint32_t w = u32(36), h = std::max(1u, u32(40)), depth = u32(44);  // height 0: a 1D texture
    const uint32_t layers = u32(48), faces = u32(52), levels = u32(56), kvd = u32(60);
    if (!check_size(w, h, "KTX", error))
        return false;
    const std::optional<Texture> t = gl_texture(internal, format, type);
    if (!t)
    {
        char hex[64];
        if (type == 0)
            std::snprintf(hex, sizeof hex, "internal format 0x%04X", internal);
        else
            std::snprintf(hex, sizeof hex, "format 0x%04X, type 0x%04X", format, type);
        error = std::string("KTX with GL ") + hex + " isn't supported";
        return false;
    }
    if (kvd > size - 64 || size - 64 - kvd < 4)
    {
        error = "KTX data is cut short";
        return false;
    }
    // Each level: its size, then its images (the first is all that's read).
    const size_t offset = 64 + size_t(kvd) + 4;
    const size_t image_size = u32(64 + kvd);
    // Byte order matters to samples wider than a byte (glTypeSize).
    const bool swap = be && type_size > 1;
    if (!decode_texture(*t, d + offset, std::min(image_size, size - offset), int(w), int(h), 4, swap, false, out, error))
        return false;

    out.format = "KTX";
    out.add("KTX", "Format", t->name);
    if (be)
        out.add("KTX", "Byte order", "big-endian");
    add_layout(out, "KTX", levels, layers, faces, depth);
    add_metadata(out, "KTX", key_values(d + 64, kvd, be));
    return true;
}

#ifdef HAVE_ZSTD
// The first `want` bytes of a Zstandard stream: a level holds every layer,
// face and slice, of which only the first image is shown.
bool unzstd(const uint8_t* src, size_t n, size_t want, std::vector<uint8_t>& dst, std::string& error)
{
    // Zstandard can't expand more than ~32768 times (a 128 KB block in a
    // 4-byte RLE block): a header asking for more isn't believed.
    const unsigned long long frame = ZSTD_getFrameContentSize(src, n);
    if (want / 32768 > n || (frame != ZSTD_CONTENTSIZE_UNKNOWN && frame != ZSTD_CONTENTSIZE_ERROR && frame < want))
    {
        error = "KTX2 Zstandard data is cut short";
        return false;
    }
    dst.resize(want);
    ZSTD_DStream* z = ZSTD_createDStream();
    if (!z)
    {
        error = "out of memory";
        return false;
    }
    ZSTD_inBuffer in{src, n, 0};
    ZSTD_outBuffer o{dst.data(), want, 0};
    size_t r = 0;
    while (o.pos < o.size)
    {
        const size_t before = o.pos;
        r = ZSTD_decompressStream(z, &o, &in);
        if (ZSTD_isError(r) || (in.pos == in.size && o.pos == before))
            break;  // corrupt, or the input ran out
    }
    ZSTD_freeDStream(z);
    if (o.pos < want)
    {
        error = ZSTD_isError(r) ? std::string("KTX2 Zstandard data is corrupt: ") + ZSTD_getErrorName(r) : "KTX2 Zstandard data is cut short";
        return false;
    }
    return true;
}
#endif

bool decode2(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 80 || !sniff2(d, size))
    {
        error = "not a KTX2 file";
        return false;
    }
    const uint32_t vk = le32(d + 12), w = le32(d + 20), h = std::max(1u, le32(d + 24)), depth = le32(d + 28);
    const uint32_t layers = le32(d + 32), faces = le32(d + 36), levels = le32(d + 40), scheme = le32(d + 44);
    const uint32_t dfd_offset = le32(d + 48), dfd_length = le32(d + 52), kvd_offset = le32(d + 56), kvd_length = le32(d + 60);
    if (!check_size(w, h, "KTX2", error))
        return false;
    const size_t level_count = std::max(1u, levels);
    if (level_count > 32 || size < 80 + level_count * 24)
    {
        error = "KTX2 level index is cut short";
        return false;
    }

    // The data format descriptor's basic block: colour model and flags.
    uint8_t colour_model = 0, flags = 0;
    if (dfd_length >= 16 && dfd_offset <= size && dfd_length <= size - dfd_offset)
    {
        colour_model = d[dfd_offset + 12];
        flags = d[dfd_offset + 15];
    }
    // BasisLZ (ETC1S), UASTC and Basis Universal's newer HDR and XUASTC
    // codecs: an undefined VkFormat with a colour model of their own, to be
    // transcoded by Basis Universal. (UASTC HDR 4x4 is ASTC, and says so.)
    if (scheme == 1 || (vk == 0 && colour_model >= 160))
    {
        const char* codec = colour_model == 163 ? "ETC1S" : colour_model == 166 ? "UASTC" : "supercompressed";
        error = std::string("KTX2 with Basis Universal ") + codec + " data isn't supported";
        return false;
    }
    const std::optional<Texture> t = vk_texture(vk);
    if (!t)
    {
        error = "KTX2 with VkFormat " + std::to_string(vk) + " isn't supported";
        return false;
    }
    const bool premultiplied = (flags & 1) != 0;  // KHR_DF_FLAG_ALPHA_PREMULTIPLIED

    // Level 0 is the first in the index, whatever its place in the file.
    const uint64_t level_offset = le64(d + 80), level_length = le64(d + 88);
    if (level_offset > size || level_length > size - level_offset)
    {
        error = "KTX2 data is cut short";
        return false;
    }
    const uint8_t* data = d + level_offset;
    size_t data_size = size_t(level_length);
    const char* supercompression = nullptr;
    std::vector<uint8_t> inflated;
    if (scheme == 2)
    {
#ifdef HAVE_ZSTD
        if (!unzstd(data, data_size, image_bytes(*t, int(w), int(h), 1), inflated, error))
            return false;
        data = inflated.data();
        data_size = inflated.size();
        supercompression = "Zstandard";
#else
        error = "KTX2 with Zstandard supercompression isn't supported (built without libzstd)";
        return false;
#endif
    }
    else if (scheme == 3)
    {
        error = "KTX2 with zlib supercompression isn't supported";
        return false;
    }
    else if (scheme != 0)
    {
        error = "KTX2 supercompression scheme " + std::to_string(scheme) + " isn't supported";
        return false;
    }
    if (!decode_texture(*t, data, data_size, int(w), int(h), 1, false, premultiplied, out, error))
        return false;

    out.format = "KTX2";
    out.add("KTX2", "Format", t->name);
    if (supercompression)
        out.add("KTX2", "Supercompression", supercompression);
    if (premultiplied)
        out.add("KTX2", "Alpha", "premultiplied");
    add_layout(out, "KTX2", levels, layers, faces, depth);
    if (kvd_offset <= size && kvd_length <= size - kvd_offset)
        add_metadata(out, "KTX2", key_values(d + kvd_offset, kvd_length, false));
    return true;
}

}  // namespace

const Decoder& ktx_decoder()
{
    static const Decoder d{"KTX", {"ktx"}, sniff1, decode1};
    return d;
}

const Decoder& ktx2_decoder()
{
    static const Decoder d{"KTX2", {"ktx2"}, sniff2, decode2};
    return d;
}

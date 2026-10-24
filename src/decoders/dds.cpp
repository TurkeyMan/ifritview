// DirectDraw Surface: a 124-byte header, an optional DX10 extension naming
// a DXGI format, then the surfaces, largest mipmap first. Block-compressed
// data (DXT1-5, ATI1/2, BC4-BC7) goes through bcn.cpp; uncompressed data is
// described by bit masks (legacy) or a DXGI format, or is 16/32-bit float.
// A cube map, array or volume shows its first face or slice.
#include "bcn.h"
#include "decoder.h"


#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

constexpr uint32_t fourcc(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 8 && std::memcmp(h, "DDS ", 4) == 0 && le32(h + 4) == 124;
}

// Pixel format flags.
constexpr uint32_t kAlphaPixels = 0x1, kAlpha = 0x2, kFourCC = 0x4, kPalette8 = 0x20, kRGB = 0x40, kLuminance = 0x20000, kBumpDuDv = 0x80000;

// Uncompressed pixels described by masks over a little-endian word.
struct Masks
{
    int bytes;
    uint32_t r, g, b, a;
    bool luminance = false;  // r is grey
    bool alpha_only = false;  // a alone, shown grey
};

// Unpacked by kind of sample, for the formats masks can't describe.
enum class Wide
{
    Unorm16,  // per channel, little-endian
    Snorm16,
    Half,
    Float
};
struct WideFormat
{
    Wide kind;
    int channels;  // R, RG or RGBA
};

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

void put(uint32_t* px, uint32_t r, uint32_t g, uint32_t b, uint32_t a, bool& opaque)
{
    if (a < 255)
    {
        opaque = false;
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }
    *px = a << 24 | r << 16 | g << 8 | b;
}

bool decode_masks(const Masks& m, const uint8_t* d, size_t size, int w, int h, Image& out, std::string& error)
{
    const size_t row = size_t(w) * m.bytes;
    if (size < row * h)
    {
        error = "DDS data is cut short";
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
            if (m.alpha_only)
            {
                const uint32_t a = extract(v, m.a);
                put(px + x, a, a, a, 255, opaque);
                continue;
            }
            const uint32_t r = extract(v, m.r);
            const uint32_t g = m.luminance ? r : extract(v, m.g), b = m.luminance ? r : extract(v, m.b);
            put(px + x, r, g, b, m.a ? extract(v, m.a) : 255, opaque);
        }
    }
    out.has_alpha = !opaque;
    return true;
}

// Kept as they're stored, as the image's source, which renders the pixels:
// floats as linear light (with an exposure when they go past 1), 16-bit
// integers as they're encoded.
bool decode_wide(const WideFormat& f, const uint8_t* d, size_t size, int w, int h, Image& out, std::string& error)
{
    using S = SourceData::Sample;
    const S sample = f.kind == Wide::Float ? S::F32 : f.kind == Wide::Half ? S::F16 : f.kind == Wide::Snorm16 ? S::I16 : S::U16;
    auto deep = std::make_shared<SourceData>(sample, f.channels, w, h, sample == S::F16 || sample == S::F32);
    if (size < deep->data.size())
    {
        error = "DDS data is cut short";
        return false;
    }
    std::memcpy(deep->data.data(), d, deep->data.size());  // little-endian, as the machine is
    out.set_source(std::move(deep));
    return true;
}

// What a DXGI format (from the DX10 header) holds, if it's one shown here.
struct Dxgi
{
    std::optional<Bcn> bcn;
    std::optional<Masks> masks;
    std::optional<WideFormat> wide;
    const char* name;
};

std::optional<Dxgi> dxgi(uint32_t f)
{
    switch (f)
    {
        case 70: case 71: case 72: return Dxgi{Bcn::BC1, {}, {}, "BC1"};
        case 73: case 74: case 75: return Dxgi{Bcn::BC2, {}, {}, "BC2"};
        case 76: case 77: case 78: return Dxgi{Bcn::BC3, {}, {}, "BC3"};
        case 79: case 80: return Dxgi{Bcn::BC4, {}, {}, "BC4"};
        case 81: return Dxgi{Bcn::BC4S, {}, {}, "BC4 signed"};
        case 82: case 83: return Dxgi{Bcn::BC5, {}, {}, "BC5"};
        case 84: return Dxgi{Bcn::BC5S, {}, {}, "BC5 signed"};
        case 94: case 95: return Dxgi{Bcn::BC6H, {}, {}, "BC6H"};
        case 96: return Dxgi{Bcn::BC6HS, {}, {}, "BC6H signed"};
        case 97: case 98: case 99: return Dxgi{Bcn::BC7, {}, {}, "BC7"};
        case 27: case 28: case 29: return Dxgi{{}, Masks{4, 0xFF, 0xFF00, 0xFF0000, 0xFF000000u}, {}, "R8G8B8A8"};
        case 87: case 90: case 91: return Dxgi{{}, Masks{4, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u}, {}, "B8G8R8A8"};
        case 88: case 92: case 93: return Dxgi{{}, Masks{4, 0xFF0000, 0xFF00, 0xFF, 0}, {}, "B8G8R8X8"};
        case 23: case 24: return Dxgi{{}, Masks{4, 0x3FF, 0xFFC00, 0x3FF00000, 0xC0000000u}, {}, "R10G10B10A2"};
        case 85: return Dxgi{{}, Masks{2, 0xF800, 0x07E0, 0x001F, 0}, {}, "B5G6R5"};
        case 86: return Dxgi{{}, Masks{2, 0x7C00, 0x03E0, 0x001F, 0x8000}, {}, "B5G5R5A1"};
        case 115: return Dxgi{{}, Masks{2, 0x0F00, 0x00F0, 0x000F, 0xF000}, {}, "B4G4R4A4"};
        case 60: case 61: return Dxgi{{}, Masks{1, 0xFF, 0, 0, 0, true}, {}, "R8"};
        case 65: return Dxgi{{}, Masks{1, 0, 0, 0, 0xFF, false, true}, {}, "A8"};
        case 48: case 49: return Dxgi{{}, Masks{2, 0xFF, 0xFF00, 0, 0}, {}, "R8G8"};
        case 11: return Dxgi{{}, {}, WideFormat{Wide::Unorm16, 4}, "R16G16B16A16"};
        case 10: return Dxgi{{}, {}, WideFormat{Wide::Half, 4}, "R16G16B16A16 float"};
        case 2: return Dxgi{{}, {}, WideFormat{Wide::Float, 4}, "R32G32B32A32 float"};
        case 13: return Dxgi{{}, {}, WideFormat{Wide::Snorm16, 4}, "R16G16B16A16 signed"};
        case 56: return Dxgi{{}, {}, WideFormat{Wide::Unorm16, 1}, "R16"};
        case 58: return Dxgi{{}, {}, WideFormat{Wide::Snorm16, 1}, "R16 signed"};
        case 54: return Dxgi{{}, {}, WideFormat{Wide::Half, 1}, "R16 float"};
        case 41: return Dxgi{{}, {}, WideFormat{Wide::Float, 1}, "R32 float"};
    }
    return std::nullopt;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 128 || !sniff(d, size))
    {
        error = "not a DDS file";
        return false;
    }
    const int h = int(le32(d + 12)), w = int(le32(d + 16));
    const uint32_t depth = le32(d + 24), mips = le32(d + 28);
    const uint32_t pf_flags = le32(d + 80), cc = le32(d + 84), bits = le32(d + 88);
    const uint32_t rmask = le32(d + 92), gmask = le32(d + 96), bmask = le32(d + 100), amask = le32(d + 104);
    const uint32_t caps2 = le32(d + 112);
    if (w <= 0 || h <= 0 || int64_t(w) * h > (int64_t(1) << 30))
    {
        error = "bad DDS size";
        return false;
    }
    size_t offset = 128;
    std::optional<Bcn> bcn;
    std::optional<Masks> masks;
    std::optional<WideFormat> wide;
    bool premultiplied = false, rxgb = false;
    std::string format;
    uint32_t array_size = 1;
    if ((pf_flags & kFourCC) && cc == fourcc("DX10"))
    {
        if (size < 148)
        {
            error = "DDS DX10 header is cut short";
            return false;
        }
        const uint32_t f = le32(d + 128);
        array_size = std::max(1u, le32(d + 140));
        offset = 148;
        const auto x = dxgi(f);
        if (!x)
        {
            error = "DDS format DXGI " + std::to_string(f) + " isn't supported";
            return false;
        }
        bcn = x->bcn;
        masks = x->masks;
        wide = x->wide;
        format = x->name;
    }
    else if (pf_flags & kFourCC)
    {
        switch (cc)
        {
            case fourcc("DXT1"): bcn = Bcn::BC1; break;
            case fourcc("DXT2"): bcn = Bcn::BC2, premultiplied = true; break;
            case fourcc("DXT3"): bcn = Bcn::BC2; break;
            case fourcc("DXT4"): bcn = Bcn::BC3, premultiplied = true; break;
            case fourcc("DXT5"): bcn = Bcn::BC3; break;
            case fourcc("RXGB"): bcn = Bcn::BC3, rxgb = true; break;  // Doom 3's normal maps: red in the alpha
            case fourcc("ATI1"):
            case fourcc("BC4U"): bcn = Bcn::BC4; break;
            case fourcc("BC4S"): bcn = Bcn::BC4S; break;
            case fourcc("ATI2"):
            case fourcc("BC5U"): bcn = Bcn::BC5; break;
            case fourcc("BC5S"): bcn = Bcn::BC5S; break;
            // Direct3D 9 format numbers in place of a code.
            case 36: wide = WideFormat{Wide::Unorm16, 4}; break;
            case 111: wide = WideFormat{Wide::Half, 1}; break;
            case 113: wide = WideFormat{Wide::Half, 4}; break;
            case 114: wide = WideFormat{Wide::Float, 1}; break;
            case 116: wide = WideFormat{Wide::Float, 4}; break;
            default:
            {
                std::string code;
                for (int i = 0; i < 4; ++i)
                {
                    const char c = char(cc >> (8 * i));
                    code += c >= 32 && c < 127 ? c : '?';
                }
                error = "DDS format " + (cc < 256 ? std::to_string(cc) : "'" + code + "'") + " isn't supported";
                return false;
            }
        }
        if (bcn)
            format = std::string(bcn_name(*bcn)) + (premultiplied ? ", premultiplied" : "") + (rxgb ? ", RXGB" : "");
        else
            format = "Direct3D format " + std::to_string(cc);
    }
    else if (pf_flags & kPalette8)
    {
        // 256 BGRA entries before the indices.
        if (size < offset + 1024 + size_t(w) * h)
        {
            error = "DDS data is cut short";
            return false;
        }
        const uint8_t* pal = d + offset;
        const uint8_t* idx = pal + 1024;
        out.allocate(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const uint8_t* e = pal + idx[size_t(y) * w + x] * 4;
                out.row(y)[x] = 0xFF000000u | uint32_t(e[2]) << 16 | uint32_t(e[1]) << 8 | e[0];
            }
        out.format = "DDS";
        out.bits_per_sample = 8;
        out.pixel_format = PixelFormat::palette(8, 256);
        return true;
    }
    else if (pf_flags & (kRGB | kLuminance | kAlpha | kBumpDuDv))
    {
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
        {
            error = "DDS with " + std::to_string(bits) + " bits a pixel isn't supported";
            return false;
        }
        Masks m{int(bits / 8), rmask, gmask, bmask, (pf_flags & (kAlphaPixels | kAlpha)) ? amask : 0};
        m.luminance = (pf_flags & kLuminance) != 0;
        m.alpha_only = (pf_flags & kAlpha) && !(pf_flags & (kRGB | kLuminance));
        // Some writers fill in masks that don't fit the pixel (0xFF000000 for
        // 8-bit luminance): the usual layout for its size, then.
        const uint32_t fits = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
        if (m.luminance && ((m.r & ~fits) || !m.r))
        {
            m.r = bits == 16 && m.a ? 0xFF : fits;
            if (m.a)
                m.a = bits == 16 ? 0xFF00 : m.a & fits;
        }
        if (m.alpha_only && ((m.a & ~fits) || !m.a))
            m.a = fits;
        masks = m;
        format = std::to_string(bits) + "-bit " + (m.alpha_only ? "alpha" : m.luminance ? "luminance" : "RGB");
    }
    else
    {
        error = "DDS pixel format isn't recognised";
        return false;
    }

    if (offset > size)
    {
        error = "DDS data is cut short";
        return false;
    }
    bool ok;
    if (bcn)
        ok = decode_bcn(*bcn, d + offset, size - offset, w, h, out, error, premultiplied);
    else if (masks)
        ok = decode_masks(*masks, d + offset, size - offset, w, h, out, error);
    else
        ok = decode_wide(*wide, d + offset, size - offset, w, h, out, error);
    if (!ok)
        return false;
    if (rxgb)
    {
        // The red channel is stored in alpha: put it back and make it opaque.
        for (int y = 0; y < h; ++y)
            for (uint32_t* px = out.row(y); px != out.row(y) + w; ++px)
                *px = 0xFF000000u | (*px >> 24) << 16 | (*px & 0xFFFF);
        out.has_alpha = false;
    }

    out.format = "DDS";
    using M = PixelFormat::Model;
    if (bcn)
    {
        out.bits_per_sample = *bcn == Bcn::BC6H || *bcn == Bcn::BC6HS ? 16 : 8;
        out.pixel_format.model = *bcn == Bcn::BC4 || *bcn == Bcn::BC4S ? M::L : M::RGB;
        out.pixel_format.notes.push_back(std::string(bcn_name(*bcn)) + " block compression");
    }
    else if (wide)
    {
        out.bits_per_sample = wide->kind == Wide::Float ? 32 : 16;
        out.pixel_format = PixelFormat::packed(wide->channels == 1 ? M::L : M::RGB, wide->channels == 1 ? "R" : "RGBA", out.bits_per_sample);
        if (wide->kind == Wide::Half || wide->kind == Wide::Float)
            out.pixel_format.sample = PixelFormat::Sample::Float;
        else if (wide->kind == Wide::Snorm16)
            out.pixel_format.sample = PixelFormat::Sample::Signed;
    }
    else
    {
        out.bits_per_sample = 8;
        out.pixel_format.model = masks->luminance || masks->alpha_only ? M::L : M::RGB;
        out.pixel_format.bits_per_pixel = masks->bytes * 8;
    }
    out.add("DDS", "Format", format);
    if (mips > 1)
        out.add("DDS", "Mipmaps", std::to_string(mips));
    if (caps2 & 0x200)
        out.add("DDS", "Cube map", "first face shown");
    if (caps2 & 0x200000 && depth > 1)
        out.add("DDS", "Volume", std::to_string(depth) + " slices, the first shown");
    if (array_size > 1)
        out.add("DDS", "Array", std::to_string(array_size) + " textures, the first shown");
    return true;
}

}  // namespace

const Decoder& dds_decoder()
{
    static const Decoder d{"DDS", {"dds"}, sniff, decode};
    return d;
}

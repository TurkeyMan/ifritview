// Valve Texture Format (Source engine), versions 7.0 to 7.5: a header, a
// small DXT1 thumbnail (skipped), then the high-res image as every mipmap,
// smallest first, each holding every frame, cube face and depth slice.
// From 7.3 a resource directory says where the high-res data starts. The
// largest mipmap of the first frame, face and slice is shown. Block-
// compressed data (DXT1/3/5, ATI1N/ATI2N) goes through bcn.cpp; the
// uncompressed formats are 8-bit channels in various orders, packed 16-bit
// pixels, 16-bit integer or half-float and 32-bit float (clipped to 0..1).
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

uint16_t le16(const uint8_t* p)
{
    return uint16_t(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 12 && std::memcmp(h, "VTF\0", 4) == 0 && le32(h + 4) == 7 && le32(h + 8) <= 5;
}

// How a format's pixels are stored.
enum class Kind
{
    Bytes,   // 8-bit channels at the given byte offsets
    Packed,  // 16-bit little-endian word, channels by mask
    Unorm16,  // 16-bit integer channels, RGBA
    Half,    // 16-bit float channels
    Float,   // 32-bit float channels
    Block,   // 4 × 4 blocks
};

struct Format
{
    const char* name;
    Kind kind;
    int bytes = 0;  // per pixel; unused for blocks
    // Bytes: offset of each channel in the pixel, -1 when absent (colour 0,
    // alpha opaque); a grey format points all three at the same byte.
    // Packed: masks over the word. Wide kinds: the channel count in r.
    uint32_t r = 0, g = 0, b = 0, a = 0;
    Bcn bcn = Bcn::BC1;
    bool bluescreen = false;  // pure blue is transparent
};

constexpr uint32_t kNone = 0xFFFFFFFFu;

// VTFLib's numbering, which Valve's tools share.
std::optional<Format> format(uint32_t f)
{
    switch (f)
    {
        case 0: return Format{"RGBA8888", Kind::Bytes, 4, 0, 1, 2, 3};
        case 1: return Format{"ABGR8888", Kind::Bytes, 4, 3, 2, 1, 0};
        case 2: return Format{"RGB888", Kind::Bytes, 3, 0, 1, 2, kNone};
        case 3: return Format{"BGR888", Kind::Bytes, 3, 2, 1, 0, kNone};
        case 4: return Format{"RGB565", Kind::Packed, 2, 0x001F, 0x07E0, 0xF800, 0};  // red in the low bits
        case 5: return Format{"I8", Kind::Bytes, 1, 0, 0, 0, kNone};
        case 6: return Format{"IA88", Kind::Bytes, 2, 0, 0, 0, 1};
        case 8: return Format{"A8", Kind::Bytes, 1, 0, 0, 0, kNone};  // alpha alone, shown grey
        case 9: return Format{"RGB888 bluescreen", Kind::Bytes, 3, 0, 1, 2, kNone, Bcn::BC1, true};
        case 10: return Format{"BGR888 bluescreen", Kind::Bytes, 3, 2, 1, 0, kNone, Bcn::BC1, true};
        // Not the order the name says: the bytes are G, B, A, R, as VTFLib
        // (and every tool built on it) reads and writes them.
        case 11: return Format{"ARGB8888", Kind::Bytes, 4, 3, 0, 1, 2};
        case 12: return Format{"BGRA8888", Kind::Bytes, 4, 2, 1, 0, 3};
        case 13: return Format{"DXT1", Kind::Block, 0, 0, 0, 0, 0, Bcn::BC1};
        case 14: return Format{"DXT3", Kind::Block, 0, 0, 0, 0, 0, Bcn::BC2};
        case 15: return Format{"DXT5", Kind::Block, 0, 0, 0, 0, 0, Bcn::BC3};
        case 16: return Format{"BGRX8888", Kind::Bytes, 4, 2, 1, 0, kNone};
        case 17: return Format{"BGR565", Kind::Packed, 2, 0xF800, 0x07E0, 0x001F, 0};
        case 18: return Format{"BGRX5551", Kind::Packed, 2, 0x7C00, 0x03E0, 0x001F, 0};
        case 19: return Format{"BGRA4444", Kind::Packed, 2, 0x0F00, 0x00F0, 0x000F, 0xF000};
        case 20: return Format{"DXT1 one-bit alpha", Kind::Block, 0, 0, 0, 0, 0, Bcn::BC1};
        case 21: return Format{"BGRA5551", Kind::Packed, 2, 0x7C00, 0x03E0, 0x001F, 0x8000};
        case 22: return Format{"UV88", Kind::Bytes, 2, 0, 1, kNone, kNone};  // shown as red and green
        case 23: return Format{"UVWQ8888", Kind::Bytes, 4, 0, 1, 2, 3};
        case 24: return Format{"RGBA16161616F", Kind::Half, 8, 4};
        case 25: return Format{"RGBA16161616", Kind::Unorm16, 8, 4};
        case 26: return Format{"UVLX8888", Kind::Bytes, 4, 0, 1, 2, 3};
        case 27: return Format{"R32F", Kind::Float, 4, 1};
        case 28: return Format{"RGB323232F", Kind::Float, 12, 3};
        case 29: return Format{"RGBA32323232F", Kind::Float, 16, 4};
        // The engine's normal-map formats: 37 and 38 in the 2013 SDK, moved
        // to 34 and 35 from Alien Swarm on (where 2013 has depth formats
        // that never appear in files).
        case 34: case 37: return Format{"ATI2N", Kind::Block, 0, 0, 0, 0, 0, Bcn::BC5};
        case 35: case 38: return Format{"ATI1N", Kind::Block, 0, 0, 0, 0, 0, Bcn::BC4};
    }
    return std::nullopt;
}

// Bytes of one w × h image in this format.
uint64_t image_size(const Format& f, int w, int h)
{
    if (f.kind == Kind::Block)
        return bcn_size(f.bcn, w, h);
    return uint64_t(w) * uint64_t(h) * uint64_t(f.bytes);
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

float half_to_float(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    float v;
    if (exp == 0)
        v = std::ldexp(float(mant), -24);
    else if (exp == 31)
        v = mant ? 0.0f : 65504.0f;  // NaN shown black; infinity clipped
    else
        v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    return sign ? -v : v;
}

uint32_t clip(float v)
{
    return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
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

// Uncompressed pixels; the caller has checked there are w × h of them.
void decode_pixels(const Format& f, const uint8_t* d, int w, int h, Image& out)
{
    out.allocate(w, h);
    bool opaque = true;
    for (int y = 0; y < h; ++y)
    {
        uint32_t* px = out.row(y);
        const uint8_t* s = d + size_t(y) * w * f.bytes;
        for (int x = 0; x < w; ++x, s += f.bytes)
        {
            switch (f.kind)
            {
                case Kind::Bytes:
                {
                    const uint32_t r = f.r == kNone ? 0 : s[f.r];
                    const uint32_t g = f.g == kNone ? 0 : s[f.g];
                    const uint32_t b = f.b == kNone ? 0 : s[f.b];
                    uint32_t a = f.a == kNone ? 255 : s[f.a];
                    if (f.bluescreen && r == 0 && g == 0 && b == 255)
                        a = 0;
                    put(px + x, r, g, b, a, opaque);
                    break;
                }
                case Kind::Packed:
                {
                    const uint32_t v = le16(s);
                    put(px + x, extract(v, f.r), extract(v, f.g), extract(v, f.b), f.a ? extract(v, f.a) : 255, opaque);
                    break;
                }
                default:
                {
                    float c[4] = {0, 0, 0, 1};
                    const int sample = f.kind == Kind::Float ? 4 : 2;
                    for (uint32_t i = 0; i < f.r; ++i)
                    {
                        const uint8_t* p = s + i * sample;
                        if (f.kind == Kind::Unorm16)
                            c[i] = float(le16(p)) / 65535.0f;
                        else if (f.kind == Kind::Half)
                            c[i] = half_to_float(le16(p));
                        else
                        {
                            const uint32_t bits = le32(p);
                            std::memcpy(&c[i], &bits, 4);
                        }
                    }
                    if (f.r == 1)
                        c[1] = c[2] = c[0];
                    put(px + x, clip(c[0]), clip(c[1]), clip(c[2]), clip(c[3]), opaque);
                    break;
                }
            }
        }
    }
    out.has_alpha = !opaque;
}

// The storage of an uncompressed format, read from its name: channel
// letters then each channel's width ("BGRA4444", "RGBA16161616F", "IA88").
PixelFormat describe(const Format& f)
{
    std::string letters, digits;
    for (const char* c = f.name; *c && *c != ' '; ++c)
        (*c >= '0' && *c <= '9' ? digits : letters) += *c == 'I' ? 'Y' : *c;
    if (!letters.empty() && letters.back() == 'F')
        letters.pop_back();  // the float suffix
    const size_t each = letters.empty() ? 0 : digits.size() / letters.size();
    std::vector<int> bits;
    for (size_t i = 0; each && i + each <= digits.size(); i += each)
        bits.push_back(std::stoi(digits.substr(i, each)));
    const bool grey = letters == "Y" || letters == "YA" || letters == "A";
    return PixelFormat::widths(grey ? PixelFormat::Model::Gray : PixelFormat::Model::RGB, letters.c_str(), bits);
}

// Header flags shown in the metadata.
constexpr uint32_t kFlagSrgb = 0x40, kFlagNormal = 0x80, kFlagEnvmap = 0x4000;

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || !sniff(d, size))
    {
        error = "not a VTF file";
        return false;
    }
    const uint32_t minor = le32(d + 8);
    // 7.0/7.1 headers end at 63 bytes, 7.2 adds the depth, 7.3 the resource count.
    const size_t fixed = minor >= 3 ? 80 : minor == 2 ? 65 : 63;
    if (size < fixed)
    {
        error = "VTF header is cut short";
        return false;
    }
    const uint32_t header_size = le32(d + 12);
    const int w = le16(d + 16), h = le16(d + 18);
    const uint32_t flags = le32(d + 20);
    const int frames = std::max(1, int(le16(d + 24)));
    const uint16_t first_frame = le16(d + 26);
    const uint32_t hi_format = le32(d + 52);
    const int mips = std::max(1, int(d[56]));
    const uint32_t lo_format = le32(d + 57);
    const int lo_w = d[61], lo_h = d[62];
    const int depth = minor >= 2 ? std::max(1, int(le16(d + 63))) : 1;
    if (w <= 0 || h <= 0 || int64_t(w) * h > (int64_t(1) << 30))
    {
        error = "bad VTF size";
        return false;
    }
    const auto f = format(hi_format);
    if (!f)
    {
        error = "VTF format " + std::to_string(int32_t(hi_format)) + " isn't supported";
        return false;
    }
    // Cube maps hold six faces, plus a sphere map before 7.5 unless the
    // first-frame field says otherwise (VTFLib's rule).
    const int faces = (flags & kFlagEnvmap) ? (minor < 5 && first_frame != 0xFFFF ? 7 : 6) : 1;

    // Where the high-res data starts.
    uint64_t offset = 0;
    if (minor >= 3)
    {
        const uint32_t count = le32(d + 68);
        if (count > 32 || 80 + uint64_t(count) * 8 > size)
        {
            error = "VTF resource directory is damaged";
            return false;
        }
        bool found = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* e = d + 80 + i * 8;
            if (e[0] == 0x30 && e[1] == 0 && e[2] == 0)
            {
                offset = le32(e + 4);
                found = true;
            }
        }
        if (!found)
        {
            error = "VTF has no high-res image";
            return false;
        }
    }
    else
    {
        // The thumbnail (normally DXT1) sits between header and image.
        uint64_t thumbnail = 0;
        if (lo_format != kNone && lo_w > 0 && lo_h > 0)
        {
            const auto lo = format(lo_format);
            if (!lo)
            {
                error = "VTF thumbnail format " + std::to_string(int32_t(lo_format)) + " isn't supported";
                return false;
            }
            thumbnail = image_size(*lo, lo_w, lo_h);
        }
        offset = uint64_t(header_size) + thumbnail;
    }

    // Skip every smaller mipmap: they come first, each with all its frames,
    // faces and slices (volume slices halve with each mipmap, as the engine
    // and VTFLib lay them out).
    for (int m = mips - 1; m >= 1 && offset <= size; --m)
    {
        const auto halve = [m](int n) { return m < 16 ? std::max(1, n >> m) : 1; };  // sizes are 16-bit
        const int mw = halve(w), mh = halve(h), md = halve(depth);
        // Each factor is at most 16 bits wide; stop before the product could wrap.
        const uint64_t level = image_size(*f, mw, mh) * uint64_t(frames) * faces;
        offset += level > size ? level : level * uint64_t(md);
    }
    const uint64_t need = image_size(*f, w, h);
    if (offset > size || need > size - offset)
    {
        error = "VTF data is cut short";
        return false;
    }
    const uint8_t* data = d + offset;
    if (f->kind == Kind::Block)
    {
        if (!decode_bcn(f->bcn, data, size_t(need), w, h, out, error))
            return false;
    }
    else
    {
        decode_pixels(*f, data, w, h, out);
    }

    out.format = "VTF";
    using M = PixelFormat::Model;
    if (f->kind == Kind::Block)
    {
        out.bits_per_sample = 8;
        out.pixel_format = PixelFormat::packed(f->bcn == Bcn::BC4 ? M::Gray : M::RGB, f->bcn == Bcn::BC4 ? "R" : f->bcn == Bcn::BC5 ? "RG" : "RGBA", 8);
        out.pixel_format.bits_per_pixel = 0;
        out.pixel_format.notes.push_back(std::string(bcn_name(f->bcn)) + " block compression");
    }
    else
    {
        out.pixel_format = describe(*f);
        out.bits_per_sample = f->kind == Kind::Float ? 32 : f->kind == Kind::Half || f->kind == Kind::Unorm16 ? 16 : 8;
        if (f->kind == Kind::Half || f->kind == Kind::Float)
            out.pixel_format.sample = PixelFormat::Sample::Float;
    }

    out.add("VTF", "Version", "7." + std::to_string(minor));
    out.add("VTF", "Format", f->name);
    if (mips > 1)
        out.add("VTF", "Mipmaps", std::to_string(mips));
    if (frames > 1)
        out.add("VTF", "Frames", std::to_string(frames) + ", the first shown");
    if (faces > 1)
        out.add("VTF", "Cube map", std::to_string(faces) + " faces, the first shown");
    if (depth > 1)
        out.add("VTF", "Volume", std::to_string(depth) + " slices, the first shown");
    if (flags & kFlagNormal)
        out.add("VTF", "Normal map", "yes");
    if (flags & kFlagSrgb)
        out.add("VTF", "sRGB", "yes");
    return true;
}

}  // namespace

const Decoder& vtf_decoder()
{
    static const Decoder d{"VTF", {"vtf"}, sniff, decode};
    return d;
}

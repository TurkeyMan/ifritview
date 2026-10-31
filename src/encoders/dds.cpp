// DirectDraw Surface: the block-compressed formats GPUs read (BC1-BC5
// through rgbcx, BC7 through bc7enc) and the uncompressed layouts, with a
// mipmap chain. The DX10 header where the format needs one (BC7, an sRGB
// tag), else the one every tool reads, if asked for. Not BC6H yet.
#include "encoder.h"

#include "../i18n.h"

#include "reduce.h"

#include "../decoders/texel.h"
#include "../half.h"

#include "../../third_party/bc7enc/bc7enc.h"
#include "../../third_party/bc7enc/rgbcx.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{

enum class Layout
{
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
    BC7,
    RGBA8,
    BGRA8,
    RGB8,
    B5G6R5,
    B5G5R5A1,
    B4G4R4A4,
    R8,
    RG8,
    RGBA16F,
    RGBA32F,
};

struct Format
{
    Layout layout;
    const char* label;
    uint32_t dxgi, dxgi_srgb;  // dxgi_srgb 0: no sRGB variant
    uint32_t fourcc;           // the legacy header's code; 0 for masks (or none, when `masks` is empty too)
    int bits;                  // a pixel, uncompressed; a block's bytes are bits * 2 when compressed (16 pixels / 8)
    uint32_t masks[4];         // R, G, B, A, uncompressed legacy
    bool compressed;
    bool alpha;
    bool lossy;  // what's saved isn't what's shown (blocks, fewer bits or channels)
};

constexpr uint32_t fourcc(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}

// In the Format setting's order, after Automatic.
const Format kFormats[] = {
    {Layout::BC1, N_("BC1 (DXT1): colour, 4 bits a pixel"), 71, 72, fourcc("DXT1"), 4, {}, true, true, true},
    {Layout::BC2, N_("BC2 (DXT3): colour, sharp alpha"), 74, 75, fourcc("DXT3"), 8, {}, true, true, true},
    {Layout::BC3, N_("BC3 (DXT5): colour, smooth alpha"), 77, 78, fourcc("DXT5"), 8, {}, true, true, true},
    {Layout::BC4, N_("BC4: one channel (red)"), 80, 0, fourcc("ATI1"), 4, {}, true, false, true},
    {Layout::BC5, N_("BC5: two channels (normal maps)"), 83, 0, fourcc("ATI2"), 8, {}, true, false, true},
    {Layout::BC7, N_("BC7: colour and alpha, best"), 98, 99, 0, 8, {}, true, true, true},
    {Layout::RGBA8, N_("RGBA 8-bit, uncompressed"), 28, 29, 0, 32, {0xFF, 0xFF00, 0xFF0000, 0xFF000000u}, false, true, false},
    {Layout::BGRA8, N_("BGRA 8-bit, uncompressed"), 87, 91, 0, 32, {0xFF0000, 0xFF00, 0xFF, 0xFF000000u}, false, true, false},
    // Three bytes a pixel, a quarter less than with alpha: the old header
    // only (DXGI has no 24-bit layout). Tools read it; GPUs don't, so
    // engines convert it as they load it.
    {Layout::RGB8, N_("RGB 8-bit, uncompressed (no alpha)"), 0, 0, 0, 24, {0xFF0000, 0xFF00, 0xFF, 0}, false, false, false},
    {Layout::B5G6R5, N_("RGB 565 (16-bit)"), 85, 0, 0, 16, {0xF800, 0x07E0, 0x001F, 0}, false, false, true},
    {Layout::B5G5R5A1, N_("ARGB 1555 (16-bit)"), 86, 0, 0, 16, {0x7C00, 0x03E0, 0x001F, 0x8000}, false, true, true},
    {Layout::B4G4R4A4, N_("ARGB 4444 (16-bit)"), 115, 0, 0, 16, {0x0F00, 0x00F0, 0x000F, 0xF000}, false, true, true},
    {Layout::R8, N_("R8: one channel"), 61, 0, 0, 8, {0xFF, 0, 0, 0}, false, false, true},
    {Layout::RG8, N_("RG8: two channels"), 49, 0, 0, 16, {0xFF, 0xFF00, 0, 0}, false, false, true},
    {Layout::RGBA16F, N_("RGBA half float (HDR)"), 10, 0, 113, 64, {}, false, true, false},
    {Layout::RGBA32F, N_("RGBA float (HDR)"), 2, 0, 116, 128, {}, false, true, false},
};
constexpr int kFormatCount = int(std::size(kFormats));

bool floating(Layout l)
{
    return l == Layout::RGBA16F || l == Layout::RGBA32F;
}

// A mip level as straight RGBA floats: 0..1 for the 8-bit layouts, linear
// light (past 1 for HDR) for the floating ones.
struct Level
{
    int width, height;
    std::vector<float> rgba;
    const float* at(int x, int y) const { return &rgba[(size_t(y) * width + x) * 4]; }
};

// The image as the format takes it. The floating layouts are linear light:
// an HDR or float source as it is, anything else off sRGB's curve.
Level first_level(const Image& image, Layout layout)
{
    Level l{image.width, image.height, std::vector<float>(size_t(image.width) * image.height * 4)};
    float* d = l.rgba.data();
    if (floating(layout) && image.source)
    {
        const SourceData& s = *image.source;
        const bool curve = !s.linear;
        for (int y = 0; y < s.height; ++y)
            for (int x = 0; x < s.width; ++x, d += 4)
            {
                const uint8_t* px = s.pixel(x, y);
                const bool grey = s.channels < 3;
                for (int c = 0; c < 3; ++c)
                {
                    const float v = s.value(px, grey ? 0 : c);
                    d[c] = curve ? srgb_to_linear(std::clamp(v, 0.0f, 1.0f)) : v;
                }
                d[3] = s.has_alpha() ? s.value(px, s.channels - 1) : 1.0f;
            }
        return l;
    }
    const std::vector<uint8_t> px = to_rgba8(image);
    for (size_t i = 0; i < px.size(); i += 4, d += 4)
        for (int c = 0; c < 4; ++c)
        {
            const float v = px[i + size_t(c)] / 255.0f;
            d[c] = floating(layout) && c < 3 ? srgb_to_linear(v) : v;
        }
    return l;
}

// Half the size, each pixel the average of the 2 x 2 it covers (the last
// row or column again, where a side is odd), weighted by alpha so clear
// pixels' colour doesn't bleed in; colour off sRGB's curve while averaged
// when it's on one, so the smaller images aren't darker.
Level half_level(const Level& in, bool curve)
{
    Level out{std::max(1, in.width / 2), std::max(1, in.height / 2), {}};
    out.rgba.resize(size_t(out.width) * out.height * 4);
    float* d = out.rgba.data();
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x, d += 4)
        {
            float sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                {
                    const float* p = in.at(std::min(in.width - 1, x * 2 + dx), std::min(in.height - 1, y * 2 + dy));
                    for (int c = 0; c < 3; ++c)
                        sum[c] += (curve ? srgb_to_linear(p[c]) : p[c]) * p[3];
                    sum[3] += p[3];
                }
            for (int c = 0; c < 3; ++c)
            {
                const float v = sum[3] > 0 ? sum[c] / sum[3] : 0.0f;
                d[c] = curve ? linear_to_srgb(v) : v;
            }
            d[3] = sum[3] / 4;
        }
    return out;
}

uint8_t to_byte(float v)
{
    return uint8_t(unit_to_byte(v));
}

// Rows of blocks shared out over the cores (the encoders are slow at their
// best); `row(by)` writes block row `by`.
template <typename F>
void each_block_row(int rows, F row)
{
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, std::min(rows, 32));
    if (threads <= 1)
    {
        for (int by = 0; by < rows; ++by)
            row(by);
        return;
    }
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back(
            [&, t]
            {
                for (int by = t; by < rows; by += threads)
                    row(by);
            });
    for (std::thread& th : pool)
        th.join();
}

// BC1's clear pixels: rgbcx ignores alpha, so a block with any is redone
// in the three-colour mode, its endpoints in that mode's order, clear
// pixels the clear index, the rest the nearest of the endpoints and their
// midpoint. `px` is the block's 16 RGBA pixels.
void punch_through(uint8_t* block, const uint8_t* px)
{
    bool any = false;
    for (int i = 0; i < 16; ++i)
        any = any || px[i * 4 + 3] < 128;
    if (!any)
        return;
    uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    if (c0 > c1)
        std::swap(c0, c1);
    const auto rgb = [](uint16_t c)
    {
        const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
        return std::array<int, 3>{r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2};
    };
    const std::array<int, 3> e0 = rgb(c0), e1 = rgb(c1);
    const std::array<int, 3> palette[3] = {e0, e1, {(e0[0] + e1[0]) / 2, (e0[1] + e1[1]) / 2, (e0[2] + e1[2]) / 2}};
    uint32_t indices = 0;
    for (int i = 0; i < 16; ++i)
    {
        const uint8_t* p = px + i * 4;
        uint32_t index = 3;
        if (p[3] >= 128)
        {
            int best = INT_MAX;
            for (uint32_t k = 0; k < 3; ++k)
            {
                const int dr = p[0] - palette[k][0], dg = p[1] - palette[k][1], db = p[2] - palette[k][2];
                if (const int d = dr * dr + dg * dg + db * db; d < best)
                    best = d, index = k;
            }
        }
        indices |= index << (2 * i);
    }
    block[0] = uint8_t(c0), block[1] = uint8_t(c0 >> 8), block[2] = uint8_t(c1), block[3] = uint8_t(c1 >> 8);
    for (int b = 0; b < 4; ++b)
        block[4 + b] = uint8_t(indices >> (8 * b));
}

// BC7 for an opaque block: bc7enc's mode 6 can leave its alpha at 254 (a
// p-bit of 0 under alpha endpoints of 127), which makes an opaque image
// faintly transparent. Both alpha endpoints and p-bits set make it 255; the
// p-bits also end the colour endpoints, which move by 1/255 at most.
void opaque_bc7(uint8_t* block, const uint8_t* px)
{
    for (int i = 0; i < 16; ++i)
        if (px[i * 4 + 3] != 255)
            return;
    if ((block[0] & 0x7F) != 0x40)  // mode 6: six zero bits, then a one
        return;
    uint64_t lo, hi;
    std::memcpy(&lo, block, 8);
    std::memcpy(&hi, block + 8, 8);
    lo |= ((uint64_t(1) << 14) - 1) << 49;  // A0 and A1, bits 49-62
    lo |= uint64_t(1) << 63;                // P0
    hi |= 1;                                // P1, bit 64
    std::memcpy(block, &lo, 8);
    std::memcpy(block + 8, &hi, 8);
}

// A block's clear pixels take the colour of the rest (weighted by alpha),
// so the colour fit isn't pulled towards what nobody sees (black, from
// unpremultiplying).
void fill_clear(uint8_t* px)
{
    uint32_t sum[3] = {0, 0, 0}, weight = 0;
    bool any_clear = false;
    for (int i = 0; i < 16; ++i)
    {
        const uint8_t* p = px + i * 4;
        any_clear = any_clear || p[3] == 0;
        for (int c = 0; c < 3; ++c)
            sum[c] += uint32_t(p[c]) * p[3];
        weight += p[3];
    }
    if (!any_clear || weight == 0)
        return;
    for (int i = 0; i < 16; ++i)
        if (px[i * 4 + 3] == 0)
            for (int c = 0; c < 3; ++c)
                px[i * 4 + c] = uint8_t((sum[c] + weight / 2) / weight);
}

// BC2's alpha: 4 bits a pixel, as they are.
void explicit_alpha(uint8_t* block, const uint8_t* px)
{
    for (int i = 0; i < 16; i += 2)
        block[i / 2] = uint8_t((px[i * 4 + 3] * 15 + 127) / 255 | (px[(i + 1) * 4 + 3] * 15 + 127) / 255 << 4);
}

struct Settings
{
    uint32_t level;  // rgbcx's, 0..18
    bc7enc_compress_block_params bc7;
};

void compress(const Level& l, Layout layout, const Settings& s, uint8_t* out)
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       rgbcx::init();
                       bc7enc_compress_block_init();
                   });
    const int bw = (l.width + 3) / 4, bh = (l.height + 3) / 4;
    const size_t block_bytes = layout == Layout::BC1 || layout == Layout::BC4 ? 8 : 16;
    each_block_row(bh,
                   [&](int by)
                   {
                       for (int bx = 0; bx < bw; ++bx)
                       {
                           // Past the edge, the edge pixels again.
                           uint8_t px[64];
                           for (int i = 0; i < 16; ++i)
                           {
                               const float* p = l.at(std::min(l.width - 1, bx * 4 + i % 4), std::min(l.height - 1, by * 4 + i / 4));
                               for (int c = 0; c < 4; ++c)
                                   px[i * 4 + c] = to_byte(p[c]);
                           }
                           if (layout != Layout::BC4 && layout != Layout::BC5)  // their channels are data, alpha or not
                               fill_clear(px);
                           uint8_t* dst = out + (size_t(by) * bw + bx) * block_bytes;
                           switch (layout)
                           {
                               case Layout::BC1:
                                   rgbcx::encode_bc1(s.level, dst, px, true, false);
                                   punch_through(dst, px);
                                   break;
                               case Layout::BC2:
                                   explicit_alpha(dst, px);
                                   rgbcx::encode_bc1(s.level, dst + 8, px, false, false);  // BC2's colour has only the four-colour mode
                                   break;
                               case Layout::BC3: rgbcx::encode_bc3(s.level, dst, px); break;
                               case Layout::BC4: rgbcx::encode_bc4(dst, px); break;
                               case Layout::BC5: rgbcx::encode_bc5(dst, px); break;
                               default:
                                   bc7enc_compress_block(dst, px, &s.bc7);
                                   opaque_bc7(dst, px);
                                   break;
                           }
                       }
                   });
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}

// The pixels of one level, uncompressed, little-endian.
void store(const Level& l, const Format& f, bool dither, std::vector<uint8_t>& out)
{
    const size_t n = size_t(l.width) * l.height;
    if (f.layout == Layout::RGBA16F || f.layout == Layout::RGBA32F)
    {
        for (size_t i = 0; i < n * 4; ++i)
        {
            if (f.layout == Layout::RGBA16F)
            {
                const uint16_t h = float_to_half(std::isnan(l.rgba[i]) ? 0.0f : std::clamp(l.rgba[i], -65504.0f, 65504.0f));
                out.push_back(uint8_t(h));
                out.push_back(uint8_t(h >> 8));
            }
            else
            {
                uint32_t bits;
                std::memcpy(&bits, &l.rgba[i], 4);
                put32(out, bits);
            }
        }
        return;
    }
    std::vector<uint8_t> px(n * 4);
    for (size_t i = 0; i < n * 4; ++i)
        px[i] = to_byte(l.rgba[i]);
    switch (f.layout)
    {
        case Layout::B5G6R5: reduce_channels(px, l.width, l.height, 5, 6, 5, 0, dither); break;
        case Layout::B5G5R5A1: reduce_channels(px, l.width, l.height, 5, 5, 5, 1, dither); break;
        case Layout::B4G4R4A4: reduce_channels(px, l.width, l.height, 4, 4, 4, 4, dither); break;
        default: break;
    }
    const int bytes = f.bits / 8;
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t* p = &px[i * 4];
        uint32_t v = 0;
        for (int c = 0; c < 4; ++c)
            if (const uint32_t m = f.masks[c])
            {
                // The channel's top bits, into its mask.
                int shift = 0, width = 0;
                while (!(m >> shift & 1))
                    ++shift;
                while (m >> (shift + width) & 1)
                    ++width;
                v |= (uint32_t(p[c]) >> (8 - width)) << shift;
            }
        for (int b = 0; b < bytes; ++b)
            out.push_back(uint8_t(v >> (8 * b)));
    }
}

Layout automatic(const Image& image)
{
    return image.hdr() ? Layout::RGBA16F : image.has_alpha ? Layout::BGRA8 : Layout::RGB8;
}

const Format& format_for(const Image& image, const EncodeOptions& options)
{
    const int choice = dds_encoder().value(options, "format");
    if (choice >= 1 && choice <= kFormatCount)
        return kFormats[choice - 1];
    // Automatic: as it is, nothing lost: half floats for what only floats
    // hold (alpha with them, as there's no three-channel half), else 8 bits,
    // with alpha only where it's needed. Compression is a choice.
    const Layout want = automatic(image);
    for (const Format& f : kFormats)
        if (f.layout == want)
            return f;
    return kFormats[0];
}

bool encode_dds(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const Encoder& self = dds_encoder();
    const Format& f = format_for(image, options);
    const bool mipmaps = self.value(options, "mipmaps") != 0;
    // The old header where asked for, or where there's no DXGI format (24-bit RGB).
    const bool legacy = (self.value(options, "legacy") != 0 || !f.dxgi) && (f.fourcc || f.masks[0]);
    const bool colour = self.value(options, "srgb") != 0;  // off: data, as normal maps are
    const bool srgb = colour && !floating(f.layout);       // the floating layouts are linear
    const bool dither = self.value(options, "dither") != 0;
    const int effort = self.value(options, "effort");  // fast, balanced, best

    Settings s;
    s.level = effort == 0 ? 2 : effort == 2 ? rgbcx::MAX_LEVEL : 10;
    bc7enc_compress_block_params_init(&s.bc7);  // perceptual
    if (!colour)
        bc7enc_compress_block_params_init_linear_weights(&s.bc7);  // data: every channel counts the same
    s.bc7.m_uber_level = effort == 0 ? 0 : effort == 2 ? BC7ENC_MAX_UBER_LEVEL : 1;
    s.bc7.m_max_partitions_mode = effort == 0 ? 16 : BC7ENC_MAX_PARTITIONS1;

    // Down to 1 x 1, each level half the last.
    uint32_t count = 1;
    for (int lw = image.width, lh = image.height; mipmaps && (lw > 1 || lh > 1); lw = std::max(1, lw / 2), lh = std::max(1, lh / 2))
        ++count;

    const int w = image.width, h = image.height;
    std::vector<uint8_t> out;
    out.insert(out.end(), {'D', 'D', 'S', ' '});
    const size_t top_size = f.compressed ? size_t((w + 3) / 4) * ((h + 3) / 4) * (f.bits * 2) : size_t(w) * f.bits / 8;
    // Caps, height, width, pixel format, and the size of the top level or a row of it.
    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000 | (f.compressed ? 0x80000 : 0x8);
    if (count > 1)
        flags |= 0x20000;
    put32(out, 124);
    put32(out, flags);
    put32(out, uint32_t(h));
    put32(out, uint32_t(w));
    put32(out, uint32_t(f.compressed ? top_size : (size_t(w) * f.bits + 7) / 8));
    put32(out, 0);  // depth
    put32(out, count);
    out.insert(out.end(), 11 * 4, 0);
    // The pixel format.
    put32(out, 32);
    if (legacy && f.fourcc)
    {
        put32(out, 0x4);  // a code
        put32(out, f.fourcc);
        out.insert(out.end(), 5 * 4, 0);
    }
    else if (legacy)
    {
        const bool alpha = f.masks[3] != 0;
        put32(out, f.layout == Layout::R8 ? 0x20000 /* luminance */ : 0x40 /* RGB */ | (alpha ? 0x1 : 0));
        put32(out, 0);
        put32(out, uint32_t(f.bits));
        for (const uint32_t m : f.masks)
            put32(out, m);
    }
    else
    {
        put32(out, 0x4);
        put32(out, fourcc("DX10"));
        out.insert(out.end(), 5 * 4, 0);
    }
    put32(out, 0x1000 | (count > 1 ? 0x400000 | 0x8 : 0));  // a texture; with mipmaps, complex
    out.insert(out.end(), 4 * 4, 0);                                 // caps 2-4, reserved
    if (!legacy)
    {
        put32(out, srgb && f.dxgi_srgb ? f.dxgi_srgb : f.dxgi);
        put32(out, 3);  // a 2D texture
        put32(out, 0);
        put32(out, 1);  // one of them
        put32(out, f.alpha && image.has_alpha ? 1 /* straight alpha */ : 0);
    }

    // Each level is made from the last and written, and the last let go: a
    // level is 16 bytes a pixel, more than the whole file.
    Level l = first_level(image, f.layout);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (i > 0)
            l = half_level(l, srgb);
        if (f.compressed)
        {
            const size_t start = out.size();
            out.resize(start + size_t((l.width + 3) / 4) * ((l.height + 3) / 4) * (f.bits * 2));
            compress(l, f.layout, s, out.data() + start);
        }
        else
        {
            store(l, f, dither, out);
        }
    }
    return write_file(path, out.data(), out.size(), error);
}

// The format chosen, or null for Automatic (format_for).
const Format* chosen(const EncodeOptions& options)
{
    const int choice = options.get("format", 0);
    return choice >= 1 && choice <= kFormatCount ? &kFormats[choice - 1] : nullptr;
}

bool lossy(const EncodeOptions& options)
{
    const Format* f = chosen(options);
    return f && f->lossy;  // Automatic loses nothing
}

}  // namespace

const Encoder& dds_encoder()
{
    static const Encoder e = []
    {
        using Kind = EncodeSetting::Kind;
        std::vector<const char*> formats{N_("Automatic: as it is (RGB, RGBA, or half float for HDR)")};
        for (const Format& f : kFormats)
            formats.push_back(f.label);
        Encoder d{"DDS", "dds",
                  {{"format", N_("Format"), Kind::Choice, 0, kFormatCount, 0, formats},
                   {"effort", N_("Effort"), Kind::Choice, 0, 2, 1, {N_("Fast"), N_("Balanced"), N_("Best (slow)")}, N_("block-compressed formats"),
                    nullptr, [](const EncodeOptions& o) { return chosen(o) != nullptr && chosen(o)->compressed; }},
                   {"mipmaps", N_("Mipmaps"), Kind::Toggle, 0, 1, 1, {}, N_("each half the size, down to 1 px")},
                   // Not only a tag: the mipmaps are averaged in linear light, and
                   // block colours weighed as the eye does, when it's on.
                   {"srgb", N_("sRGB colour"), Kind::Toggle, 0, 1, 1, {}, N_("off for data: normal maps, masks"), nullptr,
                    [](const EncodeOptions& o) { return chosen(o) == nullptr || !floating(chosen(o)->layout); }},
                   {"dither", N_("Dither"), Kind::Toggle, 0, 1, 1, {}, N_("16-bit formats"), nullptr,
                    [](const EncodeOptions& o) { return chosen(o) != nullptr && chosen(o)->bits == 16 && !chosen(o)->compressed && chosen(o)->layout != Layout::RG8; }},
                   {"legacy", N_("Old-style header"), Kind::Toggle, 0, 1, 0, {}, N_("for tools before DirectX 10"), nullptr,
                    [](const EncodeOptions& o) { return chosen(o) != nullptr && chosen(o)->dxgi && (chosen(o)->fourcc || chosen(o)->masks[0]); }}},
                  encode_dds};
        d.reduces = lossy;
        d.metadata = false;
        return d;
    }();
    return e;
}

int dds_save_format(uint32_t dxgi)
{
    for (int i = 0; i < kFormatCount; ++i)
        if (kFormats[i].dxgi == dxgi || (kFormats[i].dxgi_srgb && kFormats[i].dxgi_srgb == dxgi))
            return i + 1;
    return 0;
}

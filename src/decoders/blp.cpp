// Blizzard's BLP textures: a header with sixteen mipmap offsets and sizes,
// then the mipmaps; the largest is shown. BLP1 (Warcraft III) is either
// JPEG, each mipmap a JPEG body completed by a header shared between them
// and holding B, G, R, A as the four components with no colour transform,
// or a 256-colour BGRA palette with the alpha as a separate plane. BLP2
// (World of Warcraft) is a palette with 0, 1, 4 or 8-bit alpha, DXT1/3/5
// (through bcn.cpp), or plain BGRA.
#include "bcn.h"
#include "../i18n.h"
#include "../bytes.h"
#include "decoder.h"
#include "texel.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 4 && (std::memcmp(h, "BLP1", 4) == 0 || std::memcmp(h, "BLP2", 4) == 0);
}

// Pixel i of an alpha plane `bits` deep, packed from the low bits up.
uint32_t alpha_at(const uint8_t* plane, size_t i, int bits)
{
    switch (bits)
    {
        case 1: return (plane[i / 8] >> (i % 8) & 1) ? 255 : 0;
        case 4: return (plane[i / 2] >> (i % 2 * 4) & 0xF) * 17;
        case 8: return plane[i];
    }
    return 255;
}

// Palette indices then the alpha plane, as both versions store them.
bool decode_palette(const uint8_t* palette, const uint8_t* d, size_t size, int w, int h, int alpha_bits, Image& out,
                    std::string& error)
{
    const size_t pixels = size_t(w) * h;
    const size_t need = pixels + (pixels * alpha_bits + 7) / 8;
    if (size < need)
    {
        error = "BLP data is cut short";
        return false;
    }
    out.allocate(w, h);
    bool opaque = true;
    for (int y = 0; y < h; ++y)
    {
        uint32_t* px = out.row(y);
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            const uint8_t* e = palette + d[i] * 4;
            put_argb(px + x, e[2], e[1], e[0], alpha_at(d + pixels, i, alpha_bits), opaque);
        }
    }
    out.has_alpha = !opaque;
    out.pixel_format = PixelFormat::palette(8, 256);
    if (alpha_bits)
        out.pixel_format.notes.push_back(std::to_string(alpha_bits) + "-bit alpha");
    return true;
}

bool decode_jpeg(const uint8_t* d, size_t size, const uint8_t* mip, size_t mip_size, int width, int height,
                 int alpha_bits, Image& out, std::string& error)
{
#ifdef HAVE_JPEG
    // The shared header (tables and markers) runs on into the mipmap's body.
    if (size < 160)
    {
        error = "BLP JPEG header is cut short";
        return false;
    }
    const uint32_t header = le32(d + 156);
    if (header > size - 160)
    {
        error = "BLP JPEG header is cut short";
        return false;
    }
    std::vector<uint8_t> jpeg(d + 160, d + 160 + header);
    jpeg.insert(jpeg.end(), mip, mip + mip_size);
    std::vector<uint8_t> samples;
    int jw = 0, jh = 0, n = 0;
    if (!decode_jpeg_samples(jpeg.data(), jpeg.size(), samples, jw, jh, n, error))
        return false;
    // The header's size wins over a JPEG that claims more.
    const int w = std::min(jw, width), h = std::min(jh, height);
    out.allocate(w, h);
    bool opaque = true;
    for (int y = 0; y < h; ++y)
    {
        uint32_t* px = out.row(y);
        const uint8_t* s = samples.data() + size_t(y) * jw * n;
        for (int x = 0; x < w; ++x, s += n)
        {
            if (n == 1)
                put_argb(px + x, s[0], s[0], s[0], 255, opaque);
            else  // B, G, R, then alpha when there are four and the header says so
                put_argb(px + x, s[2], s[1], s[0], n == 4 && alpha_bits ? s[3] : 255, opaque);
        }
    }
    out.has_alpha = !opaque;
    out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, n == 4 ? "BGRA" : n == 3 ? "BGR" : "L", 8);
    out.pixel_format.notes.push_back("JPEG");
    return true;
#else
    (void)d, (void)size, (void)mip, (void)mip_size, (void)width, (void)height, (void)alpha_bits, (void)out;
    error = "BLP JPEG needs libjpeg, which this build lacks";
    return false;
#endif
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || !sniff(d, size))
    {
        error = "not a BLP file";
        return false;
    }
    const bool blp2 = d[3] == '2';
    // BLP1: compression, alpha bits, width, height, picture type, has mipmaps
    // (28 bytes), then the offsets and sizes. BLP2: type, then encoding,
    // alpha depth, alpha type and has-mipmaps as bytes, width, height (20).
    const size_t tables = blp2 ? 20 : 28;
    if (size < tables + 128)
    {
        error = "BLP header is cut short";
        return false;
    }
    const uint32_t compression = le32(d + 4);
    const uint32_t width = le32(d + 12), height = le32(d + 16);
    uint32_t alpha_bits = blp2 ? d[9] : le32(d + 8);
    // Some Warcraft III tools write odd BLP1 alpha depths (9 for 8): any
    // other non-zero value means a full alpha byte there.
    if (!blp2 && alpha_bits != 0 && alpha_bits != 1 && alpha_bits != 4)
        alpha_bits = 8;
    const uint8_t encoding = blp2 ? d[8] : 0, alpha_type = blp2 ? d[10] : 0;
    const uint32_t offset = le32(d + tables), length = le32(d + tables + 64);
    int mips = 0;
    for (int i = 0; i < 16; ++i)
        mips += le32(d + tables + i * 4) && le32(d + tables + 64 + i * 4);
    if (width == 0 || height == 0 || width > 65535 || height > 65535 || uint64_t(width) * height > (uint64_t(1) << 30))
    {
        error = "bad BLP size";
        return false;
    }
    if (alpha_bits != 0 && alpha_bits != 1 && alpha_bits != 4 && alpha_bits != 8)
    {
        error = "BLP alpha depth " + std::to_string(alpha_bits) + " isn't supported";
        return false;
    }
    if (offset > size || length > size - offset)
    {
        error = "BLP data is cut short";
        return false;
    }
    const int w = int(width), h = int(height);
    const uint8_t* mip = d + offset;
    std::string kind;
    bool ok = false;
    if (!blp2 && compression == 0)
    {
        kind = "JPEG";
        ok = decode_jpeg(d, size, mip, length, w, h, int(alpha_bits), out, error);
    }
    else if ((!blp2 && compression == 1) || (blp2 && compression == 1 && encoding == 1))
    {
        // BLP1's palette follows its 156-byte header, BLP2's its 148.
        const size_t at = tables + 128;
        if (size < at + 1024)
        {
            error = "BLP palette is cut short";
            return false;
        }
        kind = "palette";
        ok = decode_palette(d + at, mip, length, w, h, int(alpha_bits), out, error);
    }
    else if (blp2 && compression == 1 && encoding == 2)
    {
        Bcn bcn;
        switch (alpha_type)
        {
            case 0: bcn = Bcn::BC1; kind = "DXT1"; break;
            case 1: bcn = Bcn::BC2; kind = "DXT3"; break;
            case 7: bcn = Bcn::BC3; kind = "DXT5"; break;
            default:
                error = "BLP alpha type " + std::to_string(alpha_type) + " isn't supported";
                return false;
        }
        ok = decode_bcn(bcn, mip, length, w, h, out, error);
        if (ok && bcn == Bcn::BC1 && alpha_bits == 0 && out.has_alpha)
        {
            // Without alpha, DXT1's transparent entry is plain black.
            for (int y = 0; y < h; ++y)
                for (uint32_t* px = out.row(y); px != out.row(y) + w; ++px)
                    *px |= 0xFF000000u;
            out.has_alpha = false;
        }
        if (ok)
        {
            out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, "RGBA", 8);
            out.pixel_format.bits_per_pixel = 0;
            out.pixel_format.notes.push_back(std::string(bcn_name(bcn)) + " block compression");
        }
    }
    else if (blp2 && compression == 1 && encoding == 3)
    {
        if (length < uint64_t(w) * h * 4)
        {
            error = "BLP data is cut short";
            return false;
        }
        kind = "BGRA";
        out.allocate(w, h);
        bool opaque = true;
        for (int y = 0; y < h; ++y)
        {
            uint32_t* px = out.row(y);
            const uint8_t* s = mip + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x, s += 4)
                put_argb(px + x, s[2], s[1], s[0], alpha_bits ? s[3] : 255, opaque);
        }
        out.has_alpha = !opaque;
        out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, alpha_bits ? "BGRA" : "BGRX", 8);
        ok = true;
    }
    else
    {
        error = std::string(blp2 ? "BLP2" : "BLP1") + " compression " + std::to_string(compression) +
                (blp2 ? ", encoding " + std::to_string(encoding) : "") + " isn't supported";
        return false;
    }
    if (!ok)
        return false;

    out.format = "BLP";
    out.bits_per_sample = 8;
    out.add(N_("BLP"), N_("Version"), blp2 ? "BLP2" : "BLP1");
    out.add(N_("BLP"), N_("Compression"), kind);
    out.add(N_("BLP"), N_("Alpha"), alpha_bits ? std::to_string(alpha_bits) + "-bit" : "none");
    if (mips > 1)
        out.add(N_("BLP"), N_("Mipmaps"), std::to_string(mips));
    return true;
}

}  // namespace

const Decoder& blp_decoder()
{
    static const Decoder d{"BLP", {"blp"}, sniff, decode};
    return d;
}

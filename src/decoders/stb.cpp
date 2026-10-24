// Niche formats with no system library worth linking: Photoshop PSD
// (flattened composite), Targa, BMP (every pixel format IfritView writes,
// and on Windows, where gdk-pixbuf's loaders aren't there), Softimage PIC,
// Radiance HDR and the PNM family, all via the vendored stb_image.
#include "decoder.h"
#include "icc.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PSD
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_PIC
#define STBI_ONLY_HDR
#define STBI_NO_FAILURE_STRINGS
#include "../../third_party/stb_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    if (n >= 4 && std::memcmp(h, "8BPS", 4) == 0)
        return true;  // PSD
    if (n >= 18 && h[0] == 'B' && h[1] == 'M')
        return true;  // BMP
    if (n >= 10 && std::memcmp(h, "#?RADIANCE", 10) == 0)
        return true;  // HDR
    if (n >= 6 && std::memcmp(h, "#?RGBE", 6) == 0)
        return true;  // HDR
    if (n >= 3 && h[0] == 'P' && h[1] >= '1' && h[1] <= '6' && std::isspace(h[2]))
        return true;  // PNM
    return false;
}

std::string format_name(const std::string& path)
{
    const std::string ext = file_extension(path);
    if (ext == "psd" || ext == "psb")
        return "Photoshop PSD";
    if (ext == "tga" || ext == "targa")
        return "Targa";
    if (ext == "bmp" || ext == "dib")
        return "BMP";
    if (ext == "pic")
        return "Softimage PIC";
    if (ext == "hdr")
        return "Radiance HDR";
    return "stb_image";
}

// A BMP's header as the save dialog's BMP settings: its pixel format (the
// 16-bit layouts by their red mask) and row order.
void bmp_encoding(FILE* f, Image& out)
{
    uint8_t head[58];
    const size_t n = std::fread(head, 1, sizeof head, f);
    auto u16 = [&](size_t at) { return unsigned(head[at] | head[at + 1] << 8); };
    auto u32 = [&](size_t at) { return uint32_t(u16(at)) | uint32_t(u16(at + 2)) << 16; };
    if (n < 34 || head[0] != 'B' || head[1] != 'M' || u32(14) < 40)
        return;  // an OS/2 header keeps none of this
    out.encoded_as = "bmp";
    out.encoding["topdown"] = int32_t(u32(22)) < 0;
    const unsigned bits = u16(28);
    const uint32_t red = u32(30) == 3 && n >= 58 ? u32(54) : 0;  // BI_BITFIELDS
    const int format = bits == 32 ? 1 : bits == 24 ? 2 : bits == 8 ? 6
                     : bits == 16 ? (red == 0xF800 ? 3 : red == 0x0F00 ? 5 : 4)  // 565, 4444, else 1555 (as BI_RGB's)
                                  : 0;
    if (format)
        out.encoding["format"] = format;
    // A V5 header's embedded colour profile ('MBED'), placed by its offset
    // from the header's start.
    if (u32(14) >= 124)
    {
        uint8_t v5[124 - 40];
        if (std::fseek(f, 14 + 40, SEEK_SET) == 0 && std::fread(v5, 1, sizeof v5, f) == sizeof v5)
        {
            auto at = [&](size_t o) { return uint32_t(v5[o] | v5[o + 1] << 8 | v5[o + 2] << 16 | uint32_t(v5[o + 3]) << 24); };
            const uint32_t type = at(56 - 40), where = at(112 - 40), size = at(116 - 40);
            std::vector<uint8_t> icc(size);
            if (type == 0x4D424544 && size > 128 && size < (64u << 20) && std::fseek(f, long(14 + where), SEEK_SET) == 0 &&
                std::fread(icc.data(), 1, size, f) == size)
            {
                FileMetadata m;
                m.icc = std::move(icc);
                out.color_space = icc_profile_description(m.icc.data(), m.icc.size());
                out.stored = std::make_shared<const FileMetadata>(std::move(m));
            }
        }
    }
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    int w = 0, h = 0, channels = 0;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!file_bytes(path, bytes, size))
    {
        error = "cannot read file";
        return false;
    }
    // Radiance HDR as its floats, 16-bit files (PSD) as their 16 bits: kept
    // as the image's source beside the 8-bit pixels.
    const bool hdr = stbi_is_hdr_from_memory(bytes, int(size));
    const bool deep = !hdr && stbi_is_16_bit_from_memory(bytes, int(size));
    void* data = hdr    ? static_cast<void*>(stbi_loadf_from_memory(bytes, int(size), &w, &h, &channels, 3))
                 : deep ? static_cast<void*>(stbi_load_16_from_memory(bytes, int(size), &w, &h, &channels, 4))
                        : static_cast<void*>(stbi_load_from_memory(bytes, int(size), &w, &h, &channels, 4));
    if (!data)
    {
        error = "stb_image could not decode the file";
        return false;
    }
    out.allocate(w, h);
    if (hdr)
    {
        // RGBE's shared exponent: exactly representable as floats.
        auto deep = std::make_shared<SourceData>(SourceData::Sample::F32, 3, w, h, true);
        std::memcpy(deep->data.data(), data, deep->data.size());
        out.set_source(std::move(deep));
    }
    else if (deep)
        out.from_rgba16(static_cast<const uint16_t*>(data), w * 8);
    else
        out.from_rgba8(static_cast<const uint8_t*>(data), w * 4);
    stbi_image_free(data);
    out.has_alpha = channels == 4 || channels == 2;
    // stb always reports PSDs as 4-channel; the header knows better.
    if (FILE* f = open_file(path))
    {
        uint8_t head[14];
        if (std::fread(head, 1, 14, f) == 14 && std::memcmp(head, "8BPS", 4) == 0)
            out.has_alpha = ((head[12] << 8) | head[13]) >= 4;
        std::fclose(f);
    }
    out.bits_per_sample = deep ? 16 : 8;
    {
        using M = PixelFormat::Model;
        const char* names = channels == 1 ? "L" : channels == 2 ? "LA" : channels == 4 ? "RGBA" : "RGB";
        out.pixel_format = PixelFormat::packed(channels <= 2 ? M::L : M::RGB, names, out.bits_per_sample);
        if (FILE* f = open_file(path))
        {  // TGA: the header knows the packed depth stb expands
            uint8_t h[18];
            if (format_name(path) == "Targa" && std::fread(h, 1, 18, f) == 18)
            {
                const int type = h[2], depth = h[16], attr = h[17] & 0x0F;
                if (type == 1 || type == 9)
                    out.pixel_format = PixelFormat::palette(depth);
                else if (depth == 16)
                    out.pixel_format = attr ? PixelFormat::widths(M::RGB, "ARGB", {1, 5, 5, 5}, 16) : PixelFormat::widths(M::RGB, "XRGB", {1, 5, 5, 5}, 16);
                else if (depth == 15)
                    out.pixel_format = PixelFormat::widths(M::RGB, "RGB", {5, 5, 5}, 15);
                else if (depth == 32)
                    out.pixel_format = PixelFormat::packed(M::RGB, "BGRA", 8);
                else if (depth == 24)
                    out.pixel_format = PixelFormat::packed(M::RGB, "BGR", 8);
                if (type >= 9)
                    out.pixel_format.notes.push_back("RLE");
                // As the save dialog's Targa settings.
                const int format = (type == 1 || type == 9) ? 4 : depth == 32 ? 1 : depth == 24 ? 2 : (depth == 16 || depth == 15) ? 3 : 0;
                out.encoded_as = "tga";
                if (format)
                    out.encoding["format"] = format;
                out.encoding["rle"] = type >= 9;
                out.encoding["topleft"] = (h[17] & 0x20) != 0;
            }
            else if (format_name(path) == "BMP")
                bmp_encoding(f, out);
            std::fclose(f);
        }
        if (format_name(path) == "Radiance HDR")
            out.pixel_format = PixelFormat::widths(M::RGB, "RGBE", {8, 8, 8, 8}, 32), out.pixel_format.notes.push_back("shared exponent");
    }
    out.format = format_name(path);
    return true;
}

}  // namespace

const Decoder& stb_decoder()
{
    static const Decoder d{"stb_image", {"psd", "tga", "targa", "bmp", "dib", "pic", "hdr"}, sniff, decode};
    return d;
}

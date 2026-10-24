// Niche formats with no system library worth linking: Photoshop PSD
// (flattened composite), Targa, BMP (every pixel format IfritView writes,
// and on Windows, where gdk-pixbuf's loaders aren't there), Softimage PIC,
// Radiance HDR and the PNM family, all via the vendored stb_image.
#include "decoder.h"

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
    auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
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
    unsigned char* data = stbi_load_from_memory(bytes, int(size), &w, &h, &channels, 4);
    if (!data)
    {
        error = "stb_image could not decode the file";
        return false;
    }
    out.allocate(w, h);
    out.from_rgba8(data, w * 4);
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
    out.bits_per_sample = stbi_is_16_bit_from_memory(bytes, int(size)) ? 16 : 8;
    {
        using M = PixelFormat::Model;
        const char* names = channels == 1 ? "Y" : channels == 2 ? "YA" : channels == 4 ? "RGBA" : "RGB";
        out.pixel_format = PixelFormat::packed(channels <= 2 ? M::Gray : M::RGB, names, out.bits_per_sample);
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
            }
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

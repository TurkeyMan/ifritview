// Niche formats with no system library worth linking: Photoshop PSD
// (flattened composite), Targa, Softimage PIC, Radiance HDR and the PNM
// family, all via the vendored stb_image.
#include "decoder.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PSD
#define STBI_ONLY_TGA
#define STBI_ONLY_PIC
#define STBI_ONLY_HDR
#define STBI_ONLY_PNM
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
    if (ext == "pic")
        return "Softimage PIC";
    if (ext == "hdr")
        return "Radiance HDR";
    if (ext == "pgm")
        return "PGM";
    if (ext == "ppm")
        return "PPM";
    if (ext == "pnm")
        return "PNM";
    return "stb_image";
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
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
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        uint8_t head[14];
        if (std::fread(head, 1, 14, f) == 14 && std::memcmp(head, "8BPS", 4) == 0)
            out.has_alpha = ((head[12] << 8) | head[13]) >= 4;
        std::fclose(f);
    }
    out.bits_per_sample = stbi_is_16_bit(path.c_str()) ? 16 : 8;
    {
        using M = PixelFormat::Model;
        const char* names = channels == 1 ? "Y" : channels == 2 ? "YA" : channels == 4 ? "RGBA" : "RGB";
        out.pixel_format = PixelFormat::packed(channels <= 2 ? M::Gray : M::RGB, names, out.bits_per_sample);
        if (FILE* f = std::fopen(path.c_str(), "rb"))
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
    static const Decoder d{"stb_image", {"psd", "tga", "targa", "pic", "hdr", "pgm", "ppm", "pnm"}, sniff, decode};
    return d;
}

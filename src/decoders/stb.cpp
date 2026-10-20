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
#include <cstring>

namespace {

bool sniff(const uint8_t* h, size_t n) {
    if (n >= 4 && std::memcmp(h, "8BPS", 4) == 0) return true;                       // PSD
    if (n >= 10 && std::memcmp(h, "#?RADIANCE", 10) == 0) return true;               // HDR
    if (n >= 6 && std::memcmp(h, "#?RGBE", 6) == 0) return true;                     // HDR
    if (n >= 3 && h[0] == 'P' && h[1] >= '1' && h[1] <= '6' && std::isspace(h[2])) return true;  // PNM
    return false;
}

std::string format_name(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == "psd" || ext == "psb") return "Photoshop PSD";
    if (ext == "tga" || ext == "targa") return "Targa";
    if (ext == "pic") return "Softimage PIC";
    if (ext == "hdr") return "Radiance HDR";
    if (ext == "pgm") return "PGM";
    if (ext == "ppm") return "PPM";
    if (ext == "pnm") return "PNM";
    return "stb_image";
}

bool decode(const std::string& path, Image& out, std::string& error) {
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        error = "stb_image could not decode the file";
        return false;
    }
    out.allocate(w, h);
    out.from_rgba8(data, w * 4);
    stbi_image_free(data);
    out.has_alpha = channels == 4 || channels == 2;
    out.bits_per_sample = stbi_is_16_bit(path.c_str()) ? 16 : 8;
    out.format = format_name(path);
    return true;
}

}  // namespace

const Decoder& stb_decoder() {
    static const Decoder d{"stb_image", {"psd", "tga", "targa", "pic", "hdr", "pgm", "ppm", "pnm"}, sniff, decode};
    return d;
}

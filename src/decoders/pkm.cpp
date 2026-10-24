// PKM, the container of Ericsson's etcpack and Google's etc1tool: a 16-byte
// big-endian header ("PKM 10" for ETC1, "PKM 20" for ETC2/EAC; a data type,
// the size padded to whole blocks and the size proper), then one surface of
// blocks, decoded through etc.cpp.
#include "decoder.h"
#include "../i18n.h"
#include "../bytes.h"
#include "etc.h"

#include <cstring>
#include <optional>
#include <string>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 6 && std::memcmp(h, "PKM ", 4) == 0 && (std::memcmp(h + 4, "10", 2) == 0 || std::memcmp(h + 4, "20", 2) == 0);
}

// etcpack's data types; the sRGB ones decode as their linear twins.
std::optional<Etc> etc_type(uint16_t type)
{
    switch (type)
    {
        case 0: return Etc::ETC1;
        case 1: case 9: return Etc::ETC2_RGB;
        case 2: case 3: case 10: return Etc::ETC2_RGBA;  // 2: an early number for the same
        case 4: case 11: return Etc::ETC2_RGBA1;
        case 5: return Etc::EAC_R11;
        case 6: return Etc::EAC_RG11;
        case 7: return Etc::EAC_R11S;
        case 8: return Etc::EAC_RG11S;
    }
    return std::nullopt;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 16 || !sniff(d, size))
    {
        error = "not a PKM file";
        return false;
    }
    const bool v1 = d[4] == '1';
    const uint16_t type = be16(d + 6);
    const int padded_w = be16(d + 8), padded_h = be16(d + 10), w = be16(d + 12), h = be16(d + 14);
    // ETC1 files carry 0 here; some writers fill in garbage, which is only
    // trusted when version 2 says it's a newer format.
    const std::optional<Etc> format = v1 ? Etc::ETC1 : etc_type(type);
    if (!format)
    {
        error = "PKM data type " + std::to_string(type) + " isn't supported";
        return false;
    }
    if (w <= 0 || h <= 0 || padded_w < w || padded_h < h)
    {
        error = "bad PKM size";
        return false;
    }
    // The blocks are laid out over the padded size, normally the size proper
    // rounded up to whole blocks; anything wider is decoded and cropped.
    const bool exact = (padded_w + 3) / 4 == (w + 3) / 4 && (padded_h + 3) / 4 == (h + 3) / 4;
    if (!decode_etc(*format, d + 16, size - 16, exact ? w : padded_w, exact ? h : padded_h, out, error))
        return false;
    if (!exact)
        out = out.crop(0, 0, w, h);
    out.format = "PKM";
    out.bits_per_sample = *format >= Etc::EAC_R11 ? 11 : 8;
    out.pixel_format = etc_pixel_format(*format);
    out.add(N_("PKM"), N_("Format"), etc_name(*format));
    out.add(N_("PKM"), N_("Version"), v1 ? "1.0" : "2.0");
    return true;
}

}  // namespace

const Decoder& pkm_decoder()
{
    static const Decoder d{"PKM", {"pkm"}, sniff, decode};
    return d;
}

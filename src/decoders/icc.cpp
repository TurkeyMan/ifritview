#include "icc.h"
#include "../bytes.h"

#include <cstring>
#include <cmath>

namespace
{

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\n'))
        s.pop_back();
    return s;
}

}  // namespace

std::string icc_profile_description(const uint8_t* data, size_t size)
{
    if (!data || size < 132 || std::memcmp(data + 36, "acsp", 4) != 0)
        return "";
    const uint32_t count = be32(data + 128);
    if (count > 1000 || 132 + size_t(count) * 12 > size)
        return "";
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* tag = data + 132 + i * 12;
        if (std::memcmp(tag, "desc", 4) != 0)
            continue;
        const uint32_t off = be32(tag + 4), len = be32(tag + 8);
        if (off + len > size || len < 12)
            return "";
        const uint8_t* t = data + off;
        if (std::memcmp(t, "desc", 4) == 0)
        {  // v2 textDescriptionType: ASCII count + string
            const uint32_t n = be32(t + 8);
            if (n == 0 || 12 + n > len)
                return "";
            return trim(std::string(reinterpret_cast<const char*>(t + 12), n));
        }
        if (std::memcmp(t, "mluc", 4) == 0)
        {  // v4 multiLocalizedUnicode: take the first record, UTF-16BE
            const uint32_t records = be32(t + 8), rec_size = be32(t + 12);
            if (records == 0 || rec_size < 12 || 16 + rec_size > len)
                return "";
            const uint32_t slen = be32(t + 16 + 4), soff = be32(t + 16 + 8);
            if (soff + slen > len)
                return "";
            std::string out;
            for (uint32_t k = 0; k + 1 < slen; k += 2)
            {
                const uint16_t c = uint16_t((t[soff + k] << 8) | t[soff + k + 1]);
                if (c == 0)
                    break;
                if (c < 0x80)
                    out += char(c);
                else if (c < 0x800)
                {
                    out += char(0xC0 | (c >> 6));
                    out += char(0x80 | (c & 0x3F));
                }
                else
                {
                    out += char(0xE0 | (c >> 12));
                    out += char(0x80 | ((c >> 6) & 0x3F));
                    out += char(0x80 | (c & 0x3F));
                }
            }
            return trim(out);
        }
        return "";
    }
    return "";
}

bool icc_bt709_primaries(const uint8_t* data, size_t size)
{
    if (!data || size < 132 || std::memcmp(data + 36, "acsp", 4) != 0)
        return false;
    const uint32_t count = be32(data + 128);
    if (count > 1000 || size_t(count) * 12 > size - 132)
        return false;
    // ICC colorants are XYZ adapted to D50; these are sRGB's (Bradford).
    // Allow the ICC D50/Bradford variants and s15Fixed16 rounding.
    const char* names[3] = {"rXYZ", "gXYZ", "bXYZ"};
    const float xyz[3][3] = {{.4360747f, .2225045f, .0139322f},
                            {.3850649f, .7168786f, .0971045f},
                            {.1430804f, .0606169f, .7141733f}};
    unsigned found = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* tag = data + 132 + i * 12;
        // A LUT can do more than its matrix colorants describe.
        if (std::memcmp(tag, "A2B", 3) == 0 || std::memcmp(tag, "D2B", 3) == 0)
            return false;
        for (int c = 0; c < 3; ++c)
            if (std::memcmp(tag, names[c], 4) == 0)
            {
                const uint32_t off = be32(tag + 4), len = be32(tag + 8);
                if (off > size || len > size - off || len < 20 || std::memcmp(data + off, "XYZ ", 4) != 0)
                    return false;
                for (int j = 0; j < 3; ++j)
                    if (std::abs(float(int32_t(be32(data + off + 8 + j * 4))) / 65536.f - xyz[c][j]) > .0005f)
                        return false;
                found |= 1u << c;
            }
    }
    return found == 7;
}

Gamut icc_gamut(const uint8_t* data, size_t size)
{
    if (!data || size < 132 || std::memcmp(data + 36, "acsp", 4) != 0 || std::memcmp(data + 16, "RGB ", 4) != 0)
        return Gamut::Unknown;
    const uint32_t count = be32(data + 128);
    if (count > 1000 || size_t(count) * 12 > size - 132)
        return Gamut::Unknown;
    const char* names[3] = {"rXYZ", "gXYZ", "bXYZ"};
    Mat3 m{};
    unsigned found = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* tag = data + 132 + i * 12;
        for (int c = 0; c < 3; ++c)
            if (std::memcmp(tag, names[c], 4) == 0)
            {
                const uint32_t off = be32(tag + 4), len = be32(tag + 8);
                if (off > size || len > size - off || len < 20 || std::memcmp(data + off, "XYZ ", 4) != 0)
                    return Gamut::Unknown;
                for (int j = 0; j < 3; ++j)
                    m[j][c] = double(int32_t(be32(data + off + 8 + j * 4))) / 65536.0;
                found |= 1u << c;
            }
    }
    return found == 7 ? gamut_of_colorants(m) : Gamut::Unknown;
}

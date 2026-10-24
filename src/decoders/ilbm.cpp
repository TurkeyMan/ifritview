// Amiga IFF ILBM / PBM. Planar bitplanes (ILBM) or chunky 8-bit (PBM) in a
// FORM with BMHD, CMAP, optional CAMG (EHB / HAM modes) and a ByteRun1
// compressed BODY. Handles 1–8 planes with a palette, EHB, HAM6/HAM8,
// 24-bit and 32-bit direct colour, and a mask plane.
#include "decoder.h"
#include "../i18n.h"
#include "../bytes.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 12 && std::memcmp(h, "FORM", 4) == 0 && (std::memcmp(h + 8, "ILBM", 4) == 0 || std::memcmp(h + 8, "PBM ", 4) == 0);
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    std::vector<uint8_t> d;
    if (FILE* f = open_file(path))
    {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        d.resize(n > 0 ? size_t(n) : 0);
        if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size())
            d.clear();
        std::fclose(f);
    }
    if (!sniff(d.data(), d.size()))
    {
        error = "not an IFF ILBM/PBM file";
        return false;
    }
    const bool pbm = std::memcmp(d.data() + 8, "PBM ", 4) == 0;

    int w = 0, h = 0, planes = 0, masking = 0, compression = 0, transparent = -1, x_aspect = 1, y_aspect = 1;
    std::vector<uint32_t> cmap;
    uint32_t camg = 0;
    const uint8_t* body = nullptr;
    size_t body_len = 0;
    bool have_bmhd = false;

    for (size_t pos = 12; pos + 8 <= d.size();)
    {
        const uint8_t* ck = d.data() + pos;
        const uint32_t len = be32(ck + 4);
        const uint8_t* data = ck + 8;
        if (pos + 8 + len > d.size())
            break;
        if (std::memcmp(ck, "BMHD", 4) == 0 && len >= 20)
        {
            w = be16(data);
            h = be16(data + 2);
            planes = data[8];
            masking = data[9];
            compression = data[10];
            transparent = be16(data + 12);
            x_aspect = data[14];
            y_aspect = data[15];
            have_bmhd = true;
        }
        else if (std::memcmp(ck, "CMAP", 4) == 0)
        {
            for (uint32_t i = 0; i + 2 < len; i += 3)
                cmap.push_back(0xFF000000u | (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2]);
        }
        else if (std::memcmp(ck, "CAMG", 4) == 0 && len >= 4)
        {
            camg = be32(data);
        }
        else if (std::memcmp(ck, "BODY", 4) == 0)
        {
            body = data;
            body_len = len;
        }
        pos += 8 + len + (len & 1);
    }
    if (!have_bmhd || !body || w <= 0 || h <= 0)
    {
        error = "missing BMHD or BODY";
        return false;
    }
    const bool ham = camg & 0x800, ehb = camg & 0x80;
    const bool has_mask = masking == 1;
    const int rowbytes = ((w + 15) / 16) * 2;
    const int row_planes = planes + (has_mask ? 1 : 0);

    // Unpack BODY into one planar (or chunky) buffer.
    std::vector<uint8_t> raw;
    const size_t need = pbm ? size_t(((w + 1) & ~1)) * h : size_t(rowbytes) * row_planes * h;
    raw.reserve(need);
    if (compression == 1)
    {  // ByteRun1
        for (size_t i = 0; i < body_len && raw.size() < need;)
        {
            const int8_t n = int8_t(body[i++]);
            if (n >= 0)
            {
                for (int k = 0; k <= n && i < body_len; ++k)
                    raw.push_back(body[i++]);
            }
            else if (n != -128)
            {
                if (i >= body_len)
                    break;
                const uint8_t v = body[i++];
                for (int k = 0; k < 1 - n; ++k)
                    raw.push_back(v);
            }
        }
    }
    else
    {
        raw.assign(body, body + std::min(body_len, need));
    }
    if (raw.size() < need)
        raw.resize(need, 0);

    // EHB: the upper 32 colours are the lower 32 at half brightness.
    if (ehb && cmap.size() >= 32 && cmap.size() < 64)
        for (size_t i = 0; i < 32; ++i)
            cmap.push_back(0xFF000000u | ((cmap[i] >> 1) & 0x7F7F7F));

    out.allocate(w, h);
    const bool direct = planes == 24 || planes == 32;
    std::vector<uint8_t> index(w);
    for (int y = 0; y < h; ++y)
    {
        uint32_t* o = out.row(y);
        if (pbm)
        {
            const uint8_t* row = raw.data() + size_t(y) * ((w + 1) & ~1);
            for (int x = 0; x < w; ++x)
                o[x] = row[x] < cmap.size() ? cmap[row[x]] : 0xFF000000u;
            continue;
        }
        const uint8_t* row = raw.data() + size_t(y) * rowbytes * row_planes;
        std::vector<uint32_t> value(w, 0);
        for (int p = 0; p < planes; ++p)
        {
            const uint8_t* plane = row + size_t(p) * rowbytes;
            for (int x = 0; x < w; ++x)
                if (plane[x >> 3] & (0x80 >> (x & 7)))
                    value[x] |= 1u << p;
        }
        const uint8_t* mask = has_mask ? row + size_t(planes) * rowbytes : nullptr;
        uint32_t r = 0, g = 0, b = 0;  // HAM running colour
        if (ham && !cmap.empty())
        {
            r = (cmap[0] >> 16) & 0xFF;
            g = (cmap[0] >> 8) & 0xFF;
            b = cmap[0] & 0xFF;
        }
        for (int x = 0; x < w; ++x)
        {
            uint32_t c;
            if (direct)
            {
                c = 0xFF000000u | ((value[x] & 0xFF) << 16) | (value[x] & 0xFF00) | ((value[x] >> 16) & 0xFF);
                if (planes == 32)
                    c = (c & 0x00FFFFFF) | ((value[x] >> 24) << 24);
            }
            else if (ham)
            {
                const int data_bits = planes - 2;
                const uint32_t ctrl = value[x] >> data_bits, v = value[x] & ((1u << data_bits) - 1);
                const uint32_t scaled = data_bits == 4 ? v * 17 : (v << 2) | (v >> 4);  // HAM6: 4-bit, HAM8: 6-bit
                if (ctrl == 0)
                {
                    const uint32_t pc = v < cmap.size() ? cmap[v] : 0;
                    r = (pc >> 16) & 0xFF;
                    g = (pc >> 8) & 0xFF;
                    b = pc & 0xFF;
                }
                else if (ctrl == 1)
                    b = scaled;
                else if (ctrl == 2)
                    r = scaled;
                else
                    g = scaled;
                c = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            else
            {
                c = value[x] < cmap.size() ? cmap[value[x]] : 0xFF000000u | (value[x] * 0x010101 & 0xFFFFFF);
            }
            if (has_mask && !(mask[x >> 3] & (0x80 >> (x & 7))))
                c = 0;
            else if (masking == 2 && int(value[x]) == transparent)
                c = 0;
            o[x] = c;
        }
    }
    out.has_alpha = has_mask || masking == 2 || planes == 32;
    if (out.has_alpha)
        out.from_bgra8(out.pixels.data(), out.stride);  // straight alpha from the mask -> premultiplied
    out.bits_per_sample = direct ? 8 : planes;
    out.format = pbm ? "IFF PBM" : ham ? (planes == 8 ? "IFF ILBM HAM8" : "IFF ILBM HAM6") : ehb ? "IFF ILBM EHB" : "IFF ILBM";
    using M = PixelFormat::Model;
    if (direct)
        out.pixel_format = PixelFormat::packed(M::RGB, planes == 32 ? "RGBA" : "RGB", 8), out.pixel_format.planar = true;
    else if (ham)
    {
        out.pixel_format = PixelFormat::widths(M::RGB, "X", {planes}, planes);
        out.pixel_format.notes.push_back(planes == 8 ? "HAM8, 6 bpc" : "HAM6, 4 bpc");
        out.pixel_format.planar = true;
    }
    else
    {
        out.pixel_format = PixelFormat::palette(planes, int(cmap.size()));
        out.pixel_format.planar = !pbm;
        if (ehb)
            out.pixel_format.notes.push_back("EHB");
    }
    if (has_mask)
        out.pixel_format.notes.push_back("mask plane");
    if (compression == 1)
        out.pixel_format.notes.push_back("ByteRun1");
    out.add(N_("IFF"), N_("Bitplanes"), std::to_string(planes));
    if (!cmap.empty())
        out.add(N_("IFF"), N_("Palette"), std::to_string(cmap.size()) + " colours");
    if (camg)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%X", camg);
        out.add(N_("IFF"), N_("CAMG view mode"), buf);
    }
    if (x_aspect && y_aspect && x_aspect != y_aspect)
        out.add(N_("IFF"), N_("Pixel aspect"), std::to_string(x_aspect) + ":" + std::to_string(y_aspect));
    if (masking)
        out.add(N_("IFF"), N_("Masking"), masking == 1 ? "mask plane" : masking == 2 ? "transparent colour " + std::to_string(transparent) : "lasso");
    return true;
}

}  // namespace

const Decoder& ilbm_decoder()
{
    static const Decoder d{"IFF ILBM", {"iff", "ilbm", "lbm", "pbm"}, sniff, decode};
    return d;
}

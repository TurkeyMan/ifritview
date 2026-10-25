#include "dng_opcodes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dng
{
namespace
{

struct Reader
{
    const uint8_t* p;
    size_t n;
    bool little;
    bool fits(size_t at, size_t len) const { return at <= n && len <= n - at; }
    uint16_t u16(size_t at) const
    {
        return little ? uint16_t(p[at] | p[at + 1] << 8) : uint16_t(p[at] << 8 | p[at + 1]);
    }
    uint32_t u32(size_t at) const
    {
        return little ? uint32_t(p[at]) | uint32_t(p[at + 1]) << 8 | uint32_t(p[at + 2]) << 16 | uint32_t(p[at + 3]) << 24
                      : uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | uint32_t(p[at + 3]);
    }
};

// Opcodes are big-endian whatever the file's order.
uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

double be_double(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = v << 8 | p[i];
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}

float be_float(const uint8_t* p)
{
    const uint32_t v = be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint32_t bits(float f)
{
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

struct Ifd
{
    uint32_t subfile = 0, width = 0, height = 0;
    size_t list[3] = {}, list_size[3] = {};  // OpcodeList1..3: where, how long
};

void walk(const Reader& r, uint32_t at, std::vector<Ifd>& out, int depth)
{
    for (int guard = 0; at && depth < 4 && guard < 64 && r.fits(at, 2); ++guard)
    {
        const unsigned n = r.u16(at);
        if (!r.fits(at + 2, size_t(n) * 12 + 4))
            return;
        Ifd ifd;
        std::vector<uint32_t> subs;
        for (unsigned i = 0; i < n; ++i)
        {
            const size_t e = at + 2 + size_t(i) * 12;
            const uint16_t tag = r.u16(e), type = r.u16(e + 2);
            const uint32_t count = r.u32(e + 4);
            const uint32_t value = type == 3 && count == 1 ? r.u16(e + 8) : r.u32(e + 8);
            switch (tag)
            {
                case 0xFE: ifd.subfile = value; break;
                case 0x100: ifd.width = value; break;
                case 0x101: ifd.height = value; break;
                case 0x14A:  // SubIFDs
                    if (count == 1)
                        subs.push_back(value);
                    else if (r.fits(value, size_t(count) * 4))
                        for (uint32_t k = 0; k < count && k < 16; ++k)
                            subs.push_back(r.u32(value + k * 4));
                    break;
                case 0xC740:
                case 0xC741:
                case 0xC74E:
                {
                    const int l = tag == 0xC740 ? 0 : tag == 0xC741 ? 1 : 2;
                    if (count > 4 && r.fits(value, count))
                        ifd.list[l] = value, ifd.list_size[l] = count;
                    break;
                }
                default: break;
            }
        }
        out.push_back(ifd);
        for (uint32_t s : subs)
            walk(r, s, out, depth + 1);
        at = r.u32(at + 2 + size_t(n) * 12);
    }
}

struct Map
{
    uint32_t head[12];
    float spacing_v, spacing_h, origin_v, origin_h;
    std::vector<float> gains;
};

// One GainMap's parameters (the DNG spec's order), or false if it's malformed.
bool gain_map(const uint8_t* d, size_t size, Map& m)
{
    if (size < 76)
        return false;
    for (int i = 0; i < 10; ++i)
        m.head[i] = be32(d + i * 4);
    m.spacing_v = float(be_double(d + 40)), m.spacing_h = float(be_double(d + 48));
    m.origin_v = float(be_double(d + 56)), m.origin_h = float(be_double(d + 64));
    const uint32_t planes = be32(d + 72), points = m.head[8] * m.head[9];
    m.head[10] = planes;
    if (!planes || planes > 4 || !points || points > (1u << 20) || size < 76 + size_t(points) * planes * 4 || m.head[2] <= m.head[0] ||
        m.head[3] <= m.head[1] || !m.head[6] || !m.head[7] || !(m.spacing_v > 0) || !(m.spacing_h > 0))
        return false;
    m.gains.resize(size_t(points) * planes);
    for (size_t i = 0; i < m.gains.size(); ++i)
        m.gains[i] = be_float(d + 76 + i * 4);
    return true;
}

const char* name(uint32_t id)
{
    static const char* names[] = {"?", "WarpRectilinear", "WarpFisheye", "FixVignetteRadial", "FixBadPixelsConstant", "FixBadPixelsList",
                                  "TrimBounds", "MapTable", "MapPolynomial", "GainMap", "DeltaPerRow", "DeltaPerColumn",
                                  "ScalePerRow", "ScalePerColumn", "WarpRectilinear2"};
    return id < sizeof names / sizeof *names ? names[id] : "?";
}

}  // namespace

Corrections corrections(const uint8_t* file, size_t size, int width, int height, int active_w, int active_h)
{
    Corrections c;
    if (size < 8 || !(file[0] == 'I' || file[0] == 'M'))
        return c;
    const Reader r{file, size, file[0] == 'I'};
    std::vector<Ifd> ifds;
    walk(r, r.u32(4), ifds, 0);
    const Ifd* raw = nullptr;
    for (const Ifd& i : ifds)
        if (i.subfile == 0 && int(i.width) == width && int(i.height) == height)
            raw = &i;
    if (!raw)
        return c;
    std::vector<Map> maps[2];  // list 2's, list 3's
    bool vignette = false;
    uint32_t warp_planes = 0;
    double warp[3][6] = {}, warp_h = 0.5, warp_v = 0.5;
    double k[5] = {}, centre_h = 0.5, centre_v = 0.5;
    std::string applied, skipped;
    for (int l = 1; l < 3; ++l)
    {
        if (!raw->list[l])
            continue;
        const uint8_t* p = file + raw->list[l];
        const size_t end = raw->list_size[l];
        const uint32_t count = be32(p);
        size_t at = 4;
        for (uint32_t i = 0; i < count && at + 16 <= end; ++i)
        {
            const uint32_t id = be32(p + at), flags = be32(p + at + 8), bytes = be32(p + at + 12);
            const uint8_t* d = p + at + 16;
            if (bytes > end - at - 16)
                break;
            bool used = false;
            Map m;
            if (id == 9 && gain_map(d, bytes, m) && (l == 2 || (m.head[4] == 0 && m.head[5] == 1 && m.head[10] == 1)))
            {
                maps[l - 1].push_back(std::move(m));
                used = true;
            }
            else if (id == 3 && l == 2 && bytes >= 56)
            {
                for (int j = 0; j < 5; ++j)
                    k[j] = be_double(d + j * 8);
                centre_h = be_double(d + 40), centre_v = be_double(d + 48);
                vignette = used = true;
            }
            else if (id == 1 && l == 2 && bytes >= 4)
            {
                const uint32_t planes = be32(d);
                if ((planes == 1 || planes == 3) && bytes >= 4 + planes * 48 + 16)
                {
                    for (uint32_t q = 0; q < planes; ++q)
                        for (int j = 0; j < 6; ++j)
                            warp[q][j] = be_double(d + 4 + q * 48 + j * 8);
                    warp_h = be_double(d + 4 + planes * 48), warp_v = be_double(d + 4 + planes * 48 + 8);
                    warp_planes = planes;
                    used = true;
                }
            }
            std::string& to = used ? applied : skipped;
            to += std::string(to.empty() ? "" : ", ") + name(id) + " (list " + std::to_string(l + 1) + (flags & 1 ? ", optional" : "") + ")";
            at += 16 + bytes;
        }
    }
    c.summary = applied.empty() ? "" : "applied: " + applied;
    if (!skipped.empty())
        c.summary += (c.summary.empty() ? "" : "; ") + std::string("not applied: ") + skipped;
    if (maps[0].empty() && maps[1].empty() && !vignette && !warp_planes)
    {
        c.words.clear();
        return c;
    }
    std::vector<uint32_t>& w = c.words;
    w.assign(48, 0);
    w[0] = uint32_t(maps[0].size()), w[1] = uint32_t(maps[1].size()), w[2] = vignette;
    if (vignette)
    {
        // r^2 against the farthest corner's, from the centre (relative to the image).
        const double cx = centre_h * active_w, cy = centre_v * active_h;
        const double dx = std::max(cx, active_w - cx), dy = std::max(cy, active_h - cy);
        for (int j = 0; j < 5; ++j)
            w[4 + j] = bits(float(k[j]));
        w[9] = bits(float(cx)), w[10] = bits(float(cy)), w[11] = bits(float(1 / (dx * dx + dy * dy)));
    }
    if (warp_planes)
    {
        // Normalised by the distance from the centre to the farthest corner.
        const double cx = warp_h * active_w, cy = warp_v * active_h;
        const double dx = std::max(cx, active_w - cx), dy = std::max(cy, active_h - cy);
        w[16] = warp_planes;
        w[17] = bits(float(cx)), w[18] = bits(float(cy)), w[19] = bits(float(std::sqrt(dx * dx + dy * dy)));
        for (uint32_t q = 0; q < 3; ++q)
            for (int j = 0; j < 6; ++j)
                w[20 + q * 6 + j] = bits(float(warp[std::min(q, warp_planes - 1)][j]));
    }
    const size_t descriptors = w.size();
    w.resize(descriptors + (maps[0].size() + maps[1].size()) * 16);
    size_t d = descriptors;
    for (auto& list : maps)
        for (Map& m : list)
        {
            m.head[11] = uint32_t(w.size());
            for (int j = 0; j < 12; ++j)
                w[d + j] = m.head[j];
            w[d + 12] = bits(m.spacing_v), w[d + 13] = bits(m.spacing_h), w[d + 14] = bits(m.origin_v), w[d + 15] = bits(m.origin_h);
            for (float g : m.gains)
                w.push_back(bits(g));
            d += 16;
        }
    return c;
}

}  // namespace dng

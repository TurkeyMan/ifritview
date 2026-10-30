#include "dng_opcodes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dng
{
namespace
{

// Opcodes are big-endian whatever the file's order.
double be_double(const uint8_t* p)
{
    const uint64_t v = be64(p);
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

struct Ifd
{
    uint32_t subfile = 0, width = 0, height = 0;
    size_t list[3] = {}, list_size[3] = {};  // OpcodeList1..3: where, how long
    ColourTags colour;
};

// The `i`th of the (S)RATIONALs at `at`.
double rational(const FileReader& r, size_t at, int i, bool is_signed)
{
    const uint32_t n = r.u32(at + size_t(i) * 8), d = r.u32(at + size_t(i) * 8 + 4);
    return d == 0 ? 0 : is_signed ? double(int32_t(n)) / int32_t(d) : double(n) / d;
}

void walk(const FileReader& r, uint32_t at, std::vector<Ifd>& out, int depth)
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
                case 0xC629:  // AsShotWhiteXY
                    if (type == 5 && count == 2)
                        ifd.colour.white_xy[0] = rational(r, value, 0, false), ifd.colour.white_xy[1] = rational(r, value, 1, false);
                    break;
                case 0xC6F3:  // CameraCalibrationSignature
                case 0xC6F4:  // ProfileCalibrationSignature
                {
                    const size_t at = count <= 4 ? e + 8 : value;
                    if ((type == 1 || type == 2) && count < 256 && r.fits(at, count))
                    {
                        std::string text(reinterpret_cast<const char*>(r.p + at), count);
                        text.resize(std::strlen(text.c_str()));  // to its terminator
                        (tag == 0xC6F3 ? ifd.colour.camera_signature : ifd.colour.profile_signature) = text;
                    }
                    break;
                }
                case 0xC7A5:  // BaselineExposureOffset
                    if (type == 10 && count == 1)
                        ifd.colour.exposure_offset = rational(r, value, 0, true);
                    break;
                default: break;
            }
        }
        out.push_back(ifd);
        for (uint32_t s : subs)
            walk(r, s, out, depth + 1);
        at = r.u32(at + 2 + size_t(n) * 12);
    }
}

// One GainMap's parameters (the DNG spec's order), or false if it's
// malformed. Its grid, relative to its area in the spec, in photosites.
bool gain_map(const uint8_t* d, size_t size, GainMap& m)
{
    if (size < 76)
        return false;
    uint32_t head[10];
    for (int i = 0; i < 10; ++i)
        head[i] = be32(d + i * 4);
    const double spacing_v = be_double(d + 40), spacing_h = be_double(d + 48);
    const uint32_t planes = be32(d + 72), points = head[8] * head[9];
    if (!planes || planes > 4 || !points || points > (1u << 20) || size < 76 + size_t(points) * planes * 4 || head[2] <= head[0] ||
        head[3] <= head[1] || !head[6] || !head[7] || !(spacing_v > 0) || !(spacing_h > 0))
        return false;
    const double high = head[2] - head[0], wide = head[3] - head[1];
    std::copy_n(head, 4, m.area);
    m.plane = head[4], m.planes = head[5];
    m.pitch[0] = head[6], m.pitch[1] = head[7];
    m.points[0] = head[8], m.points[1] = head[9];
    m.map_planes = planes;
    m.spacing[0] = float(spacing_v * high), m.spacing[1] = float(spacing_h * wide);
    m.origin[0] = float(be_double(d + 56) * high), m.origin[1] = float(be_double(d + 64) * wide);
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

std::vector<uint32_t> with_lens(std::vector<uint32_t> own, const std::vector<uint32_t>& lens)
{
    own.resize(std::max<size_t>(own.size(), 48));
    for (size_t i = 2; i < 38; ++i)
        if (i != 3)
            own[i] = lens.empty() ? 0 : lens[i];
    if (own.size() == 48 && std::all_of(own.begin(), own.end(), [](uint32_t w) { return w == 0; }))
        own.clear();
    return own;
}

void lay_out(Corrections& c, const std::vector<GainMap>& mosaic, const std::vector<GainMap>& rgb, const Readout* readout)
{
    std::vector<uint32_t>& w = c.words;
    w.resize(48);
    w[0] = uint32_t(mosaic.size()), w[1] = uint32_t(rgb.size());
    for (const auto* list : {&mosaic, &rgb})
        for (const GainMap& m : *list)
        {
            const uint32_t d[16] = {m.area[0],          m.area[1],          m.area[2],         m.area[3],        m.plane,      m.planes,
                                    m.pitch[0],         m.pitch[1],         m.points[0],       m.points[1],      m.map_planes, 0,
                                    bits(m.spacing[0]), bits(m.spacing[1]), bits(m.origin[0]), bits(m.origin[1])};
            w.insert(w.end(), d, d + 16);
        }
    size_t d = 48;
    for (const auto* list : {&mosaic, &rgb})
        for (const GainMap& m : *list)
        {
            w[d + 11] = uint32_t(w.size());
            for (float g : m.gains)
                w.push_back(bits(g));
            d += 16;
        }
    if (!readout)
        return;
    const Readout& r = *readout;
    w[3] = 1;
    // Two 16-bit values a word, the first in the low half.
    const auto pack = [&w](const auto& v) -> uint32_t
    {
        if (v.empty())
            return 0;
        const size_t at = w.size();
        for (size_t i = 0; i < v.size(); i += 2)
            w.push_back(uint16_t(v[i]) | uint32_t(i + 1 < v.size() ? uint16_t(v[i + 1]) : 0) << 16);
        return uint32_t(at);
    };
    w[40] = pack(r.row_black);
    w[41] = pack(r.column_black);
    w[42] = uint32_t(r.split[0]), w[43] = uint32_t(r.split[1]);
    w[44] = pack(r.curves);
    if (!r.defects.empty())
    {
        // Where each row's columns start, and where the last's end; then the columns.
        const size_t rows = w.size();
        w.resize(rows + r.defects.size() + 1);
        for (size_t y = 0; y < r.defects.size(); ++y)
        {
            w[rows + y] = uint32_t(w.size());
            w.insert(w.end(), r.defects[y].begin(), r.defects[y].end());
        }
        w[rows + r.defects.size()] = uint32_t(w.size());
        w[45] = uint32_t(rows);
    }
    if (!r.bad_columns.empty())
    {
        // Two bits a column, sixteen columns a word.
        const size_t at = w.size();
        w.resize(at + (r.bad_columns.size() + 15) / 16);
        for (size_t x = 0; x < r.bad_columns.size(); ++x)
            w[at + x / 16] |= uint32_t(r.bad_columns[x] & 3) << (x % 16 * 2);
        w[46] = uint32_t(at);
    }
    w[47] = uint32_t(r.black);
}

Corrections corrections(const uint8_t* file, size_t size, int width, int height, int active_w, int active_h)
{
    Corrections c;
    if (size < 8 || !(file[0] == 'I' || file[0] == 'M'))
        return c;
    const FileReader r{file, size, file[0] == 'M'};
    std::vector<Ifd> ifds;
    walk(r, r.u32(4), ifds, 0);
    const Ifd* raw = nullptr;
    for (const Ifd& i : ifds)
        if (i.subfile == 0 && int(i.width) == width && int(i.height) == height)
            raw = &i;
    if (!raw)
        return c;
    std::vector<GainMap> maps[2];  // list 2's, list 3's
    bool vignette = false;
    uint32_t warp_planes = 0;
    double warp[3][6] = {}, warp_h = 0.5, warp_v = 0.5;
    double k[5] = {}, centre_h = 0.5, centre_v = 0.5;
    std::string applied, skipped;
    for (int l = 1; l < 3; ++l)
    {
        if (!raw->list[l])
            continue;
        each_opcode(file + raw->list[l], raw->list_size[l], [&](uint32_t id, uint32_t flags, const uint8_t* d, size_t bytes)
        {
            bool used = false;
            GainMap m;
            if (id == 9 && gain_map(d, bytes, m) && (l == 2 || (m.plane == 0 && m.planes == 1 && m.map_planes == 1)))
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
            return true;
        });
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
    w[2] = vignette;
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
    lay_out(c, maps[0], maps[1]);
    return c;
}

ColourTags colour_tags(const uint8_t* file, size_t size)
{
    ColourTags c;
    if (size < 8 || !(file[0] == 'I' || file[0] == 'M'))
        return c;
    const FileReader r{file, size, file[0] == 'M'};
    std::vector<Ifd> ifds;
    walk(r, r.u32(4), ifds, 0);
    if (!ifds.empty())
        c = ifds[0].colour;
    return c;
}

}  // namespace dng

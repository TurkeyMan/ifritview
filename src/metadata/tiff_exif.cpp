// EXIF's own directories in a TIFF libtiff has written: the Exif, GPS and
// Interoperability IFDs copied from the saved metadata's EXIF (itself a
// TIFF structure) to the file's end, and the first directory written again
// there with pointers to them. Copying entries as they are, by type, keeps
// every tag without knowing it, which libtiff's typed EXIF fields wouldn't.
#include "tiff_exif.h"

#include "../platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace
{

constexpr uint16_t kExifIfd = 0x8769, kGpsIfd = 0x8825, kInteropIfd = 0xA005, kMakerNote = 0x927C;

// Bytes of one value of each TIFF type (0: unknown, skipped).
int type_size(uint16_t type)
{
    switch (type)
    {
        case 1: case 2: case 6: case 7: return 1;  // BYTE, ASCII, SBYTE, UNDEFINED
        case 3: case 8: return 2;                   // SHORT, SSHORT
        case 4: case 9: case 11: case 13: return 4; // LONG, SLONG, FLOAT, IFD
        case 5: case 10: case 12: return 8;         // RATIONAL, SRATIONAL, DOUBLE
        default: return 0;
    }
}
// What a value is made of, for swapping: RATIONALs are two LONGs.
int unit_size(uint16_t type)
{
    return type == 5 || type == 10 ? 4 : type_size(type);
}

struct Order
{
    bool big;
    uint16_t u16(const uint8_t* p) const { return big ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[1] << 8 | p[0]); }
    uint32_t u32(const uint8_t* p) const
    {
        return big ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]
                   : uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
    }
    void put16(uint8_t* p, uint16_t v) const
    {
        if (big)
            p[0] = uint8_t(v >> 8), p[1] = uint8_t(v);
        else
            p[0] = uint8_t(v), p[1] = uint8_t(v >> 8);
    }
    void put32(uint8_t* p, uint32_t v) const
    {
        for (int i = 0; i < 4; ++i)
            p[big ? 3 - i : i] = uint8_t(v >> (8 * i));
    }
};

bool header(const std::vector<uint8_t>& t, Order& o, uint32_t& ifd0)
{
    if (t.size() < 8 || !((t[0] == 'I' && t[1] == 'I') || (t[0] == 'M' && t[1] == 'M')))
        return false;
    o.big = t[0] == 'M';
    if (o.u16(&t[2]) != 42)  // not BigTIFF
        return false;
    ifd0 = o.u32(&t[4]);
    return ifd0 + 2 <= t.size();
}

class Copier
{
public:
    Copier(const std::vector<uint8_t>& src, Order so, std::vector<uint8_t>& out, Order oo) : src_(src), so_(so), out_(out), oo_(oo) {}

    // The directory at `offset` in the source, appended to the output with its
    // values and any directories it points to; its offset there, or 0.
    uint32_t ifd(uint32_t offset, int depth = 0)
    {
        if (depth > 3 || offset + 2 > src_.size())
            return 0;
        const uint16_t n = so_.u16(&src_[offset]);
        if (offset + 2 + size_t(n) * 12 > src_.size())
            return 0;
        struct Entry
        {
            uint16_t tag, type;
            uint32_t count;
            std::vector<uint8_t> value;  // in the output's order
            uint32_t sub = 0;            // a directory it points to, copied
        };
        std::vector<Entry> entries;
        for (uint16_t i = 0; i < n; ++i)
        {
            const uint8_t* e = &src_[offset + 2 + size_t(i) * 12];
            Entry en{so_.u16(e), so_.u16(e + 2), so_.u32(e + 4), {}};
            if (en.tag == kMakerNote)
                continue;  // its offsets are into the original file; moved, they'd point at nothing
            if (en.tag == kExifIfd || en.tag == kGpsIfd || en.tag == kInteropIfd)
            {
                en.sub = ifd(so_.u32(e + 8), depth + 1);
                if (en.sub)
                    entries.push_back(std::move(en));
                continue;
            }
            const int size = type_size(en.type);
            const uint64_t bytes = uint64_t(size) * en.count;
            if (!size || bytes > (1u << 24))
                continue;
            const uint8_t* data = bytes <= 4 ? e + 8 : nullptr;
            if (!data)
            {
                const uint32_t at = so_.u32(e + 8);
                if (uint64_t(at) + bytes > src_.size())
                    continue;
                data = &src_[at];
            }
            en.value.assign(data, data + bytes);
            swap(en.value, unit_size(en.type));
            entries.push_back(std::move(en));
        }
        // In tag order, as TIFF wants (cameras don't always keep it).
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        // Out-of-line values first, each on a word boundary, then the
        // directory pointing at them.
        std::vector<uint32_t> where(entries.size(), 0);
        for (size_t i = 0; i < entries.size(); ++i)
            if (!entries[i].sub && entries[i].value.size() > 4)
            {
                align();
                where[i] = uint32_t(out_.size());
                out_.insert(out_.end(), entries[i].value.begin(), entries[i].value.end());
            }
        align();
        const uint32_t at = uint32_t(out_.size());
        out_.resize(at + 2 + entries.size() * 12 + 4, 0);
        oo_.put16(&out_[at], uint16_t(entries.size()));
        for (size_t i = 0; i < entries.size(); ++i)
        {
            uint8_t* e = &out_[at + 2 + i * 12];
            const Entry& en = entries[i];
            oo_.put16(e, en.tag);
            oo_.put16(e + 2, en.sub ? uint16_t(4) : en.type);
            oo_.put32(e + 4, en.sub ? 1 : en.count);
            if (en.sub)
                oo_.put32(e + 8, en.sub);
            else if (en.value.size() > 4)
                oo_.put32(e + 8, where[i]);
            else
                std::memcpy(e + 8, en.value.data(), en.value.size());  // left-justified, as TIFF has it
        }
        return at;  // next directory: 0
    }

    void align()
    {
        if (out_.size() & 1)
            out_.push_back(0);
    }

private:
    // Values in the output's byte order.
    void swap(std::vector<uint8_t>& v, int unit) const
    {
        if (so_.big == oo_.big || unit < 2)
            return;
        for (size_t i = 0; i + size_t(unit) <= v.size(); i += size_t(unit))
            for (int a = 0, b = unit - 1; a < b; ++a, --b)
                std::swap(v[i + size_t(a)], v[i + size_t(b)]);
    }

    const std::vector<uint8_t>& src_;
    Order so_;
    std::vector<uint8_t>& out_;
    Order oo_;
};

}  // namespace

bool attach_exif_directories(const std::string& path, const std::vector<uint8_t>& exif, std::string& error)
{
    Order so{}, fo{};
    uint32_t exif_ifd0 = 0, file_ifd0 = 0;
    if (!header(exif, so, exif_ifd0))
        return true;  // no EXIF to carry
    // The source's pointers from its first directory.
    uint32_t exif_at = 0, gps_at = 0;
    const uint16_t n = so.u16(&exif[exif_ifd0]);
    for (uint16_t i = 0; i < n && exif_ifd0 + 2 + size_t(i + 1) * 12 <= exif.size(); ++i)
    {
        const uint8_t* e = &exif[exif_ifd0 + 2 + size_t(i) * 12];
        if (so.u16(e) == kExifIfd)
            exif_at = so.u32(e + 8);
        else if (so.u16(e) == kGpsIfd)
            gps_at = so.u32(e + 8);
    }
    if (!exif_at && !gps_at)
        return true;

    std::vector<uint8_t> file;
    {
        FILE* f = platform::fopen(path, "rb");
        if (!f)
        {
            error = "cannot read the file back to add its EXIF";
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        file.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const bool ok = std::fread(file.data(), 1, file.size(), f) == file.size();
        std::fclose(f);
        if (!ok || !header(file, fo, file_ifd0))
        {
            error = "the TIFF written can't be read back to add its EXIF";
            return false;
        }
    }
    const uint16_t count = fo.u16(&file[file_ifd0]);
    if (file_ifd0 + 2 + size_t(count) * 12 + 4 > file.size())
    {
        error = "the TIFF written can't be read back to add its EXIF";
        return false;
    }

    Copier copy(exif, so, file, fo);
    const uint32_t new_exif = exif_at ? copy.ifd(exif_at) : 0;
    const uint32_t new_gps = gps_at ? copy.ifd(gps_at) : 0;

    // The first directory again, with the pointers, its tags in order; the
    // old one is left where it is, unreferenced.
    struct Raw
    {
        uint16_t tag;
        uint8_t bytes[12];
    };
    std::vector<Raw> entries;
    for (uint16_t i = 0; i < count; ++i)
    {
        Raw r{};
        std::memcpy(r.bytes, &file[file_ifd0 + 2 + size_t(i) * 12], 12);
        r.tag = fo.u16(r.bytes);
        if (r.tag != kExifIfd && r.tag != kGpsIfd)
            entries.push_back(r);
    }
    const uint32_t next = fo.u32(&file[file_ifd0 + 2 + size_t(count) * 12]);
    for (const auto& [tag, at] : {std::pair{kExifIfd, new_exif}, std::pair{kGpsIfd, new_gps}})
        if (at)
        {
            Raw r{tag, {}};
            fo.put16(r.bytes, tag);
            fo.put16(r.bytes + 2, 4);  // LONG
            fo.put32(r.bytes + 4, 1);
            fo.put32(r.bytes + 8, at);
            entries.push_back(r);
        }
    std::sort(entries.begin(), entries.end(), [](const Raw& a, const Raw& b) { return a.tag < b.tag; });
    copy.align();
    const uint32_t ifd0 = uint32_t(file.size());
    file.resize(ifd0 + 2 + entries.size() * 12 + 4, 0);
    fo.put16(&file[ifd0], uint16_t(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i)
        std::memcpy(&file[ifd0 + 2 + i * 12], entries[i].bytes, 12);
    fo.put32(&file[ifd0 + 2 + entries.size() * 12], next);
    fo.put32(&file[4], ifd0);

    FILE* f = platform::fopen(path, "wb");
    if (!f)
    {
        error = "cannot write the file's EXIF";
        return false;
    }
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!ok)
        error = "cannot write the file's EXIF";
    return ok;
}

// EXIF parsing shared by every format that carries an EXIF blob.
#include "decoder.h"

#include <libexif/exif-data.h>
#include <libexif/exif-loader.h>
#include <libexif/exif-mnote-data.h>

#include <cstdio>
#include <cstring>

namespace
{

// An EXIF date ("YYYY:MM:DD hh:mm:ss") in seconds from 1970, as if UTC; 0 if
// it isn't one.
int64_t exif_time(const std::string& text)
{
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    if (std::sscanf(text.c_str(), "%4d:%2d:%2d %2d:%2d:%2d", &Y, &M, &D, &h, &m, &s) != 6 || Y <= 0 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h >= 24 ||
        m < 0 || m >= 60 || s < 0 || s > 60)
        return 0;
    // Days from 1970 by the civil calendar.
    const int y = M <= 2 ? Y - 1 : Y;
    const int era = y / 400, yoe = y - era * 400, mp = (M + 9) % 12;
    const int doy = (153 * mp + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t(era) * 146097 + doe - 719468) * 86400 + h * 3600 + m * 60 + s;
}

struct Visit
{
    Image* out;
    const char* group;
    ExifIfd ifd;
};

void visit_entry(ExifEntry* e, void* user)
{
    auto* v = static_cast<Visit*>(user);
    if (e->tag == EXIF_TAG_MAKER_NOTE)
        return;  // binary; decoded separately below
    const char* name = exif_tag_get_title_in_ifd(e->tag, v->ifd);
    if (!name)
        name = exif_tag_get_name_in_ifd(e->tag, v->ifd);
    char buf[1024];
    const char* value = exif_entry_get_value(e, buf, sizeof buf);
    if (!name || !value || !*value)
        return;
    std::string title = name;
    for (const auto& [from, to] : {std::pair{"Maximum ", "Max "}, std::pair{"Minimum ", "Min "}})
        for (auto pos = title.find(from); pos != std::string::npos; pos = title.find(from))
            title.replace(pos, std::strlen(from), to);
    v->out->add(v->group, title, value);
}

ExifSummary read(ExifData* ed, Image& out)
{
    ExifSummary summary;
    if (!ed)
        return summary;
    if (ExifEntry* e = exif_data_get_entry(ed, EXIF_TAG_ORIENTATION))
    {
        const int o = exif_get_short(e->data, exif_data_get_byte_order(ed));
        if (o >= 1 && o <= 8)
            summary.orientation = o;
    }
    if (ExifEntry* e = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_COLOR_SPACE))
    {
        const int cs = exif_get_short(e->data, exif_data_get_byte_order(ed));
        if (cs == 1)
            summary.color_space = "sRGB";
        else if (cs == 2)
            summary.color_space = "Adobe RGB (1998)";
    }
    if (ExifEntry* e = exif_content_get_entry(ed->ifd[EXIF_IFD_INTEROPERABILITY], EXIF_TAG_INTEROPERABILITY_INDEX))
    {
        if (e->size >= 3 && std::memcmp(e->data, "R03", 3) == 0)
            summary.color_space = "Adobe RGB (1998)";
    }
    // Said only here, no profile: one is made for it (decoder.cpp), unless
    // the file embeds its own.
    if (summary.color_space == "Adobe RGB (1998)")
        out.declared_space = ColorSpace::adobe_rgb();
    if (ExifEntry* e = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL))
        summary.taken = exif_time(std::string(reinterpret_cast<const char*>(e->data), e->size));
    if (ExifEntry* e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL))
    {
        char buf[128];
        if (exif_entry_get_value(e, buf, sizeof buf))
            summary.camera = buf;
    }

    const struct
    {
        ExifIfd ifd;
        const char* group;
    } sections[] = {
        {EXIF_IFD_0, "EXIF"},
        {EXIF_IFD_EXIF, "EXIF"},
        {EXIF_IFD_GPS, "GPS"},
        {EXIF_IFD_INTEROPERABILITY, "Interoperability"},
    };
    for (const auto& s : sections)
    {
        Visit v{&out, s.group, s.ifd};
        exif_content_foreach_entry(ed->ifd[s.ifd], visit_entry, &v);
    }
    // GPS position: three rationals (deg, min, sec) plus a hemisphere letter.
    auto coordinate = [&](ExifTag value_tag, ExifTag ref_tag, double& result)
    {
        ExifEntry* v = exif_content_get_entry(ed->ifd[EXIF_IFD_GPS], value_tag);
        ExifEntry* r = exif_content_get_entry(ed->ifd[EXIF_IFD_GPS], ref_tag);
        if (!v || v->format != EXIF_FORMAT_RATIONAL || v->components < 3)
            return false;
        const ExifByteOrder order = exif_data_get_byte_order(ed);
        double parts[3];
        for (int i = 0; i < 3; ++i)
        {
            const ExifRational q = exif_get_rational(v->data + i * 8, order);
            parts[i] = q.denominator ? double(q.numerator) / q.denominator : 0;
        }
        result = parts[0] + parts[1] / 60 + parts[2] / 3600;
        if (r && r->size > 0 && (r->data[0] == 'S' || r->data[0] == 'W'))
            result = -result;
        return true;
    };
    double lat = 0, lon = 0;
    if (coordinate(static_cast<ExifTag>(EXIF_TAG_GPS_LATITUDE), static_cast<ExifTag>(EXIF_TAG_GPS_LATITUDE_REF), lat) &&
        coordinate(static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE), static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE_REF), lon) && lat >= -90 && lat <= 90 &&
        lon >= -180 && lon <= 180)
    {
        out.has_location = true;
        out.latitude = lat;
        out.longitude = lon;
    }
    if (ExifMnoteData* mn = exif_data_get_mnote_data(ed))
    {
        const unsigned n = std::min(exif_mnote_data_count(mn), 300u);
        for (unsigned i = 0; i < n; ++i)
        {
            const char* name = exif_mnote_data_get_title(mn, i);
            if (!name)
                name = exif_mnote_data_get_name(mn, i);
            char buf[512];
            const char* value = exif_mnote_data_get_value(mn, i, buf, sizeof buf);
            if (name && value && *value)
                out.add("Maker notes", name, value);
        }
    }
    exif_data_unref(ed);
    return summary;
}

}  // namespace

// libexif's default "follow specification" option fabricates mandatory
// tags that the file never had; we only want what is actually there.
ExifData* load(const uint8_t* data, size_t size)
{
    ExifData* ed = exif_data_new();
    exif_data_unset_option(ed, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
    exif_data_load_data(ed, data, unsigned(size));
    return ed;
}

namespace
{

// A TIFF structure read directly, every offset checked against its size.
struct Tiff
{
    const uint8_t* d = nullptr;
    size_t n = 0;
    bool little = true;

    // False unless it starts with a TIFF header.
    bool open(const uint8_t* data, size_t size)
    {
        d = data;
        n = data ? size : 0;
        if (n < 8 || !((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')))
            return false;
        little = d[0] == 'I';
        return little ? d[2] == 42 && d[3] == 0 : d[2] == 0 && d[3] == 42;  // not TIFF's magic: not something to read
    }
    // Only at places already known to be inside the block.
    uint32_t u16(size_t at) const { return little ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 : uint32_t(d[at]) << 8 | uint32_t(d[at + 1]); }
    uint32_t u32(size_t at) const
    {
        return little ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24
                      : uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | uint32_t(d[at + 3]);
    }
    size_t first() const { return u32(4); }
    // The entry for `tag` in the directory at `dir`, or 0.
    size_t find(size_t dir, uint32_t tag) const
    {
        if (dir >= n || n - dir < 2)
            return 0;
        const uint32_t count = u16(dir);
        for (uint32_t i = 0; i < count && (n - dir - 2) / 12 > i; ++i)
            if (const size_t entry = dir + 2 + i * size_t(12); u16(entry) == tag)
                return entry;
        return 0;
    }
    // A SHORT entry's value, or 0.
    uint32_t short_value(size_t entry) const { return entry && u16(entry + 2) == 3 && u32(entry + 4) >= 1 ? u16(entry + 8) : 0; }
    // An ASCII entry's text, up to its first NUL; empty if it's anything else.
    std::string text(size_t entry) const
    {
        if (!entry || u16(entry + 2) != 2)
            return {};
        const size_t count = u32(entry + 4);
        const size_t at = count <= 4 ? entry + 8 : u32(entry + 8);
        if (at > n || n - at < count || count > 4096)
            return {};
        const char* s = reinterpret_cast<const char*>(d + at);
        return std::string(s, std::find(s, s + count, '\0'));
    }
};

// The position in the GPS directory at `gps`.
bool gps_location(const Tiff& t, size_t gps, double& latitude, double& longitude)
{
    const auto coordinate = [&](uint32_t ref_tag, uint32_t value_tag, char negative, double limit, double& result)
    {
        const size_t value = t.find(gps, value_tag), ref = t.find(gps, ref_tag);
        // Exactly three RATIONALs, and a one-letter hemisphere (ASCII, held in the entry).
        if (!value || t.u16(value + 2) != 5 || t.u32(value + 4) != 3 || !ref || t.u16(ref + 2) != 2 || t.u32(ref + 4) < 1 || t.u32(ref + 4) > 4)
            return false;
        const char hemisphere = char(t.d[ref + 8]);
        if (hemisphere != negative && hemisphere != (negative == 'S' ? 'N' : 'E'))
            return false;
        const size_t at = t.u32(value + 8);
        if (at > t.n || t.n - at < 24)
            return false;
        double parts[3];
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t over = t.u32(at + i * size_t(8)), under = t.u32(at + i * size_t(8) + 4);
            parts[i] = under ? double(over) / under : 0;
        }
        result = parts[0] + parts[1] / 60 + parts[2] / 3600;
        if (hemisphere == negative)
            result = -result;
        return result >= -limit && result <= limit;
    };
    return coordinate(1, 2, 'S', 90, latitude) && coordinate(3, 4, 'W', 180, longitude);
}

// The GPS position straight from a TIFF structure. libexif looks no further
// than the first 64 KB of a block, and in some raws (a phone's DNG, a drone's)
// the position's stored after that.
bool tiff_location(const uint8_t* d, size_t n, double& latitude, double& longitude)
{
    Tiff t;
    if (!t.open(d, n))
        return false;
    const size_t pointer = t.find(t.first(), 0x8825);
    if (!pointer || (t.u16(pointer + 2) != 4 && t.u16(pointer + 2) != 13) || t.u32(pointer + 4) != 1)
        return false;  // the GPS directory's place, as a single LONG (or IFD) only
    return gps_location(t, t.u32(pointer + 8), latitude, longitude);
}

}  // namespace

ExifSummary read_exif_blob(const uint8_t* data, size_t size, Image& out)
{
    if (!data || size < 8 || size > (size_t(1) << 28))
        return {};
    // libexif wants the "Exif\0\0" header; containers that store the bare
    // TIFF structure get it prepended.
    ExifSummary summary;
    if (std::memcmp(data, "Exif\0\0", 6) == 0 || (data[0] == 0xFF && data[1] == 0xD8))
        summary = read(load(data, size), out);
    else
    {
        std::vector<uint8_t> buf(6 + size);
        std::memcpy(buf.data(), "Exif\0\0", 6);
        std::memcpy(buf.data() + 6, data, size);
        summary = read(load(buf.data(), buf.size()), out);
    }
    if (!out.has_location)
    {
        const bool prefixed = std::memcmp(data, "Exif\0\0", 6) == 0;
        double latitude = 0, longitude = 0;
        if (tiff_location(data + (prefixed ? 6 : 0), size - (prefixed ? 6 : 0), latitude, longitude))
        {
            out.has_location = true;
            out.latitude = latitude;
            out.longitude = longitude;
        }
    }
    return summary;
}

std::vector<uint8_t> exif_thumbnail(const uint8_t* data, size_t size)
{
    if (!data || size < 8 || size > (size_t(1) << 28))
        return {};
    std::vector<uint8_t> prefixed;
    if (std::memcmp(data, "Exif\0\0", 6) != 0 && !(data[0] == 0xFF && data[1] == 0xD8))
    {
        prefixed.resize(6 + size);
        std::memcpy(prefixed.data(), "Exif\0\0", 6);
        std::memcpy(prefixed.data() + 6, data, size);
        data = prefixed.data();
        size = prefixed.size();
    }
    std::vector<uint8_t> thumbnail;
    if (ExifData* ed = load(data, size))
    {
        if (ed->data && ed->size)
            thumbnail.assign(ed->data, ed->data + ed->size);
        exif_data_unref(ed);
    }
    return thumbnail;
}

ExifSummary read_cr3_exif(std::span<const uint8_t> ifd0, std::span<const uint8_t> exif, std::span<const uint8_t> gps, Image& out)
{
    ExifSummary summary;
    Tiff t;
    if (t.open(ifd0.data(), ifd0.size()))
    {
        if (const uint32_t o = t.short_value(t.find(t.first(), 0x0112)); o >= 1 && o <= 8)
            summary.orientation = int(o);
        summary.camera = t.text(t.find(t.first(), 0x0110));
    }
    if (t.open(exif.data(), exif.size()))
        summary.taken = exif_time(t.text(t.find(t.first(), 0x9003)));
    double latitude = 0, longitude = 0;
    if (t.open(gps.data(), gps.size()) && gps_location(t, t.first(), latitude, longitude))
    {
        out.has_location = true;
        out.latitude = latitude;
        out.longitude = longitude;
    }
    return summary;
}

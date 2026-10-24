// EXIF parsing shared by every format that carries an EXIF blob.
#include "decoder.h"

#include <libexif/exif-data.h>
#include <libexif/exif-loader.h>
#include <libexif/exif-mnote-data.h>

#include <cstring>

namespace
{

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
        coordinate(static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE), static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE_REF), lon))
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

ExifSummary read_exif_file(const std::string& path, Image& out)
{
    ExifLoader* loader = exif_loader_new();
    const uint8_t* bytes = nullptr;
    size_t length = 0;
    if (file_bytes(path, bytes, length))
        exif_loader_write(loader, const_cast<uint8_t*>(bytes), unsigned(length));
    const unsigned char* buf = nullptr;
    unsigned int size = 0;
    exif_loader_get_buf(loader, &buf, &size);
    ExifSummary summary = size ? read(load(buf, size), out) : ExifSummary{};
    exif_loader_unref(loader);
    return summary;
}

ExifSummary read_exif_blob(const uint8_t* data, size_t size, Image& out)
{
    if (!data || size < 8 || size > (size_t(1) << 28))
        return {};
    // libexif wants the "Exif\0\0" header; containers that store the bare
    // TIFF structure get it prepended.
    if (std::memcmp(data, "Exif\0\0", 6) == 0 || (data[0] == 0xFF && data[1] == 0xD8))
        return read(load(data, size), out);
    std::vector<uint8_t> buf(6 + size);
    std::memcpy(buf.data(), "Exif\0\0", 6);
    std::memcpy(buf.data() + 6, data, size);
    return read(load(buf.data(), buf.size()), out);
}

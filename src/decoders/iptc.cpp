// Legacy IPTC-IIM block (captions, keywords, credits) via libiptcdata.
// Lives in a JPEG APP13 segment or a TIFF/PSD resource block.
#include "decoder.h"

#include <libiptcdata/iptc-data.h>
#include <libiptcdata/iptc-jpeg.h>

#include <cstring>

namespace
{

void visit(IptcDataSet* ds, void* user)
{
    auto* out = static_cast<Image*>(user);
    const char* title = iptc_tag_get_title(ds->record, ds->tag);
    if (!title)
        title = iptc_tag_get_name(ds->record, ds->tag);
    char buf[2048];
    if (!title || !iptc_dataset_get_as_str(ds, buf, sizeof buf))
        return;
    out->add("IPTC", title, buf);
}

void read(IptcData* d, Image& out)
{
    if (!d)
        return;
    iptc_data_foreach_dataset(d, visit, &out);
    iptc_data_unref(d);
}

}  // namespace

// The Photoshop APP13 segment holds the IPTC block; find it in the bytes we
// already have rather than letting libiptcdata reopen the file.
void read_iptc_jpeg(const uint8_t* data, size_t size, Image& out)
{
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return;
    for (size_t pos = 2; pos + 4 <= size;)
    {
        if (data[pos] != 0xFF)
            return;
        const uint8_t marker = data[pos + 1];
        if (marker == 0xDA || marker == 0xD9)  // start of scan / end: no more metadata
            return;
        const size_t len = (size_t(data[pos + 2]) << 8) | data[pos + 3];
        if (pos + 2 + len > size)
            return;
        if (marker == 0xED && len > 16 && std::memcmp(data + pos + 4, "Photoshop 3.0", 13) == 0)
        {
            const unsigned char* ps3 = data + pos + 4;
            unsigned int iptc_len = 0;
            const int off = iptc_jpeg_ps3_find_iptc(ps3, unsigned(len - 2), &iptc_len);
            if (off > 0 && iptc_len)
                read(iptc_data_new_from_data(ps3 + off, iptc_len), out);
            return;
        }
        pos += 2 + len;
    }
}

void read_iptc_blob(const uint8_t* data, size_t size, Image& out)
{
    if (data && size)
        read(iptc_data_new_from_data(data, unsigned(size)), out);
}

// Legacy IPTC-IIM block (captions, keywords, credits) via libiptcdata.
// Lives in a JPEG APP13 segment or a TIFF/PSD resource block.
#include "decoder.h"

#include <libiptcdata/iptc-data.h>

namespace {

void visit(IptcDataSet* ds, void* user) {
    auto* out = static_cast<Image*>(user);
    const char* title = iptc_tag_get_title(ds->record, ds->tag);
    if (!title) title = iptc_tag_get_name(ds->record, ds->tag);
    char buf[2048];
    if (!title || !iptc_dataset_get_as_str(ds, buf, sizeof buf)) return;
    out->add("IPTC", title, buf);
}

void read(IptcData* d, Image& out) {
    if (!d) return;
    iptc_data_foreach_dataset(d, visit, &out);
    iptc_data_unref(d);
}

}  // namespace

void read_iptc_jpeg(const std::string& path, Image& out) {
    read(iptc_data_new_from_jpeg(path.c_str()), out);
}

void read_iptc_blob(const uint8_t* data, size_t size, Image& out) {
    if (data && size) read(iptc_data_new_from_data(data, unsigned(size)), out);
}
